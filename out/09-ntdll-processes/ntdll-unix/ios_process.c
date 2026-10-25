// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part B: the pseudo-process registry and the per-pseudo-process state.
// See ios_process.h for how upstream's per-process globals map onto struct ios_proc.

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
# include <sys/ucontext.h>
# include <mach-o/dyld.h>
#endif

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "unix_private.h"
#include "ios_pure.h"
#include "ios_dylib.h"
#include "ios_sig_pure.h"   /* ios_sfmt: formatting in a signal handler */

#ifdef WINE_IOS

int ios_teb_key_ready = 0;
int ios_quiet = 0;

/* contract Section A: the app sets it to 1 before __wine_main when the session's first program
 * is a 32-bit (i386) PE, so its guest window is reserved before its first thread starts */
int ios_main_image_i386 = 0;

/* the registry: PEB -> struct ios_proc, lock-free lookups */
static struct ios_registry proc_registry;

/* thread-local override: a booting thread before its TEB exists, or the reaper */
static __thread struct ios_proc *thread_proc;

/* what a thread with no pseudo-process reads: neutral values, never used to talk to a server */
static struct ios_proc no_proc =
{
    .pp_fd_socket        = -1,
    .pp_initial_cwd      = -1,
    .pp_inproc_device_fd = -1,
    .pp_execute_flags    = MEM_EXECUTE_OPTION_DISABLE,
    .pp_process_cookie   = 0xdeadbeef,
    .pp_nb_threads       = 1,
    .pp_teb_list         = { &no_proc.pp_teb_list, &no_proc.pp_teb_list },
    .state               = IOS_PROC_DEAD,
    .name                = "(no process)",
    .boot_socket         = -1,
    .mutex               = PTHREAD_MUTEX_INITIALIZER,
    .cond                = PTHREAD_COND_INITIALIZER,
};

static LONG next_serial;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;


/***********************************************************************
 *           ios_log
 *
 * One line to ntdll's log (debug.c's log fd; stderr until wine_log_set_file).
 */
void ios_log( const char *fmt, ... )
{
    char buffer[1024];
    va_list args;
    int len;

    len = snprintf( buffer, sizeof(buffer), "ntdll-ios: " );
    va_start( args, fmt );
    len += vsnprintf( buffer + len, sizeof(buffer) - len, fmt, args );
    va_end( args );
    if (len >= (int)sizeof(buffer)) len = sizeof(buffer) - 1;
    ios_log_write( buffer, len );
}


/***********************************************************************
 *           ios_vlog
 *
 * prefix + a printf-style message to the log fd in one write (upstream's fatal errors go to
 * stderr, which the app's log doesn't see).
 */
void ios_vlog( const char *prefix, const char *fmt, va_list args )
{
    char buffer[1024];
    int len;

    len = snprintf( buffer, sizeof(buffer), "ntdll-ios: %s", prefix );
    len += vsnprintf( buffer + len, sizeof(buffer) - len, fmt, args );
    if (len >= (int)sizeof(buffer)) len = sizeof(buffer) - 1;
    ios_log_write( buffer, len );
}


/* the last boot step logged, for the fault logger below */
static char boot_last_step[160] = "(none)";

/***********************************************************************
 *           ios_boot_mark
 *
 * One line per boot step, always (PG_QUIET doesn't silence it), written at once with a
 * single write() to the log fd, so a log ends at the last step reached even if the app dies.
 */
void ios_boot_mark( const char *fmt, ... )
{
    char buffer[512];
    va_list args;
    int len, prefix;

    prefix = len = snprintf( buffer, sizeof(buffer), "ntdll-ios: boot: " );
    va_start( args, fmt );
    len += vsnprintf( buffer + len, sizeof(buffer) - len, fmt, args );
    va_end( args );
    if (len >= (int)sizeof(buffer) - 1) len = sizeof(buffer) - 2;
    buffer[len++] = '\n';
    ios_log_write( buffer, len );
    snprintf( boot_last_step, sizeof(boot_last_step), "%.*s", len - prefix - 1, buffer + prefix );
}


/* ---------- faults during the boot, before spec 11's handlers exist ----------
 * Installed at the start of __wine_main, removed just before signal_init_process. A fault writes
 * one line (signal, pc, address, last boot step) with no lock or allocation, puts back the
 * action that was installed before, and returns: the instruction faults again and is handled
 * (or reported as a crash) exactly as without this logger (spec 11 Part B1). */

static const int boot_fault_signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP };
static struct sigaction boot_fault_prev[ARRAY_SIZE(boot_fault_signals)];
static LONG boot_fault_installed;

/* where the main image (the app binary, which holds this library) is loaded, taken at install
 * time: addresses are logged as offsets from it, which are the binary's own addresses minus its
 * __TEXT start, so they can be looked up without the slide */
static unsigned long long boot_image_base, boot_image_slide;

#define BOOT_FRAMES_MAX 12

/* one "name addr" item: an offset from the main image when the address lies above it */
static size_t boot_fault_addr( char *buf, size_t size, const char *name, unsigned long long addr )
{
    if (boot_image_base && addr >= boot_image_base && addr - boot_image_base < 0x100000000ull)
        return ios_sfmt( buf, size, " %s img+%llx", name, addr - boot_image_base );
    return ios_sfmt( buf, size, " %s %llx", name, addr );
}

static void boot_fault_handler( int sig, siginfo_t *info, void *context )
{
    unsigned long long pc = 0, lr = 0, fp = 0;
    char line[1024];
    size_t len;
    unsigned int i;

#if defined(__APPLE__) && defined(__aarch64__)
    {
        ucontext_t *uc = context;
        pc = (unsigned long long)__darwin_arm_thread_state64_get_pc( uc->uc_mcontext->__ss );
        lr = (unsigned long long)__darwin_arm_thread_state64_get_lr( uc->uc_mcontext->__ss );
        fp = (unsigned long long)__darwin_arm_thread_state64_get_fp( uc->uc_mcontext->__ss );
    }
#endif
    len = ios_sfmt( line, sizeof(line), "ntdll-ios: boot: FAULT signal %d, address %llx, after step: %s;",
                    sig, (unsigned long long)(uintptr_t)info->si_addr, boot_last_step );
    len += boot_fault_addr( line + len, sizeof(line) - len, "pc", pc );
    len += boot_fault_addr( line + len, sizeof(line) - len, "lr", lr );
    len += ios_sfmt( line + len, sizeof(line) - len, "; image %llx slide %llx; frames:",
                     boot_image_base, boot_image_slide );
#ifdef __APPLE__
    {
        /* the x29 chain: [fp] the caller's fp, [fp + 8] the return address. Followed only while
         * each fp lies inside this thread's stack and above the previous one, so a broken
         * chain ends the walk instead of faulting again */
        unsigned long long top = (uintptr_t)pthread_get_stackaddr_np( pthread_self() );
        unsigned long long bottom = top - pthread_get_stacksize_np( pthread_self() );

        for (i = 0; i < BOOT_FRAMES_MAX; i++)
        {
            const unsigned long long *frame = (const unsigned long long *)(uintptr_t)fp;

            if (!fp || (fp & 7) || fp < bottom || fp + 16 > top) break;
            len += boot_fault_addr( line + len, sizeof(line) - len, "<-", frame[1] & 0x00007fffffffffffull );
            if (frame[0] <= fp) break;
            fp = frame[0];
        }
    }
#endif
    if (len < sizeof(line) - 1) line[len++] = '\n';
    ios_log_write( line, (int)len );
    for (i = 0; i < ARRAY_SIZE(boot_fault_signals); i++)
        if (boot_fault_signals[i] == sig) sigaction( sig, &boot_fault_prev[i], NULL );
}

void ios_boot_fault_logger_install(void)
{
    struct sigaction sa;
    unsigned int i;

    if (InterlockedExchange( &boot_fault_installed, 1 )) return;
#ifdef __APPLE__
    boot_image_base = (uintptr_t)_dyld_get_image_header( 0 );
    boot_image_slide = (unsigned long long)_dyld_get_image_vmaddr_slide( 0 );
#endif
    memset( &sa, 0, sizeof(sa) );
    sa.sa_sigaction = boot_fault_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset( &sa.sa_mask );
    for (i = 0; i < ARRAY_SIZE(boot_fault_signals); i++)
        sigaction( boot_fault_signals[i], &sa, &boot_fault_prev[i] );
}

void ios_boot_fault_logger_remove(void)
{
    unsigned int i;

    if (!InterlockedExchange( &boot_fault_installed, 0 )) return;
    for (i = 0; i < ARRAY_SIZE(boot_fault_signals); i++)
    {
        struct sigaction cur;

        /* only where it is still ours: a handler installed since then stays */
        if (!sigaction( boot_fault_signals[i], NULL, &cur ) && cur.sa_sigaction == boot_fault_handler)
            sigaction( boot_fault_signals[i], &boot_fault_prev[i], NULL );
    }
}


/* spec 08's hook, called by the server for every thread it kills or must wake */
extern void (*wineserver_thread_alert_hook)( unsigned int unix_tid );

static void init_process_once(void)
{
    const char *quiet = getenv( "PG_QUIET" );

    /* Part E2: our own key, no destructor; never a raw TSD slot */
    if (pthread_key_create( &teb_key, NULL ))
    {
        ios_log( "pthread_key_create failed: %s\n", strerror( errno ));
        return;
    }
    ios_quiet = quiet && quiet[0] == '1';
    /* spec 10 Part C6: Darwin's pthread_getspecific(key) reads slot `key` of the thread's TSD
     * array, which TPIDRRO_EL0 points at */
    ios_teb_tls_slot_offset = (int)(teb_key * sizeof(void *));
    __atomic_store_n( &ios_teb_key_ready, 1, __ATOMIC_RELEASE );
    ios_boot_mark( "TEB key %lu, TSD slot offset %d", (unsigned long)teb_key, ios_teb_tls_slot_offset );

    /* Part D3 (spec 08 clarification 4, spec 11 clarification 2) */
    wineserver_thread_alert_hook = ios_thread_alert;
}

/***********************************************************************
 *           ios_process_init_once
 *
 * App-wide set-up shared by every pseudo-process; called before the first boot.
 */
void ios_process_init_once(void)
{
    pthread_once( &init_once, init_process_once );
}


/***********************************************************************
 *           ios_current_teb
 */
TEB *ios_current_teb(void)
{
    if (!__atomic_load_n( &ios_teb_key_ready, __ATOMIC_ACQUIRE )) return NULL;
    return pthread_getspecific( teb_key );
}


/***********************************************************************
 *           ios_current_peb
 *
 * Part B1: the calling thread's PEB, or NULL on a thread with no TEB.
 */
void *ios_current_peb(void)
{
    TEB *teb = ios_current_teb();
    return teb ? teb->Peb : NULL;
}


/***********************************************************************
 *           ios_proc_from_peb
 */
struct ios_proc *ios_proc_from_peb( const void *peb_ptr )
{
    return ios_registry_lookup( &proc_registry, peb_ptr );
}


/***********************************************************************
 *           ios_proc_current_slow
 *
 * The calling thread's pseudo-process when its TEB doesn't name it (see ios_process.h).
 */
struct ios_proc *ios_proc_current_slow(void)
{
    struct ios_proc *proc;
    TEB *teb;

    if (thread_proc) return thread_proc;
    if ((teb = ios_current_teb()) && (proc = ios_registry_lookup( &proc_registry, teb->Peb )))
        return proc;
    return &no_proc;
}


/***********************************************************************
 *           ios_set_thread_proc
 */
void ios_set_thread_proc( struct ios_proc *proc )
{
    thread_proc = proc;
}


/***********************************************************************
 *           ios_proc_alloc
 *
 * A new pseudo-process with upstream's initial values for every per-process global.
 */
struct ios_proc *ios_proc_alloc( BOOL first )
{
    struct ios_proc *proc = calloc( 1, sizeof(*proc) );

    if (!proc) return NULL;
    proc->pp_fd_socket        = -1;
    proc->pp_initial_cwd      = -1;
    proc->pp_inproc_device_fd = -1;
    proc->pp_execute_flags    = MEM_EXECUTE_OPTION_DISABLE;
    proc->pp_process_cookie   = 0xdeadbeef;
    proc->pp_nb_threads       = 1;
    list_init( &proc->pp_teb_list );

    proc->serial      = InterlockedIncrement( &next_serial );
    proc->state       = IOS_PROC_BOOTING;
    proc->stage       = IOS_STAGE_CREATED;
    proc->first       = first;
    proc->top_level   = first;   /* spec 22: ios_session_start_program sets it for later programs */
    proc->boot_socket = -1;
    proc->wow_slot    = -1;
    pthread_mutex_init( &proc->mutex, NULL );
    pthread_cond_init( &proc->cond, NULL );
    strcpy( proc->name, first ? "(first program)" : "(child)" );
    return proc;
}


/***********************************************************************
 *           ios_proc_free
 *
 * Frees the struct of a pseudo-process that is no longer registered and has no thread left.
 */
void ios_proc_free( struct ios_proc *proc )
{
    if (!proc || proc == &no_proc) return;
    /* a child's arguments are one block (build_argv); argv[0] is a constant. The first
     * program's private copy is rewritten in place by rebuild_argv() and isn't freed. */
    free( proc->boot_argv );
    /* spec 17 clarification 2: no thread of the process can run any more (the struct goes only
     * after the last one left; threads suspended for good never run), so nothing walks its futex
     * queue table again */
    ios_s17_futex_table_free( &proc->futex_queues );
    pthread_mutex_destroy( &proc->mutex );
    pthread_cond_destroy( &proc->cond );
    free( proc );
}


/***********************************************************************
 *           ios_get_process_futex_queues
 *
 * Spec 17 clarification 2: the futex queue table of the calling thread's pseudo-process, created
 * zero-filled at its first use; every ntdll image of that process gets the same one, no other
 * process ever does. Fallback: a thread with no pseudo-process (before the first one exists, or
 * a thread Wine didn't create) gets NULL, and class 0xf00d then fails, so such a caller keeps
 * its own image's table.
 */
void *ios_get_process_futex_queues(void)
{
    struct ios_proc *proc = ios_proc_current();

    if (proc == &no_proc) return NULL;
    return ios_s17_futex_table_get( &proc->futex_queues );
}


/***********************************************************************
 *           ios_proc_register
 *
 * Publishes the pseudo-process under its PEB once the PEB exists. The slot is complete before
 * it becomes visible (ios_registry_publish).
 */
void ios_proc_register( struct ios_proc *proc )
{
    if (ios_registry_publish( &proc_registry, proc->pp_peb, proc ))
        ios_log( "process %u: registry full or PEB %p already registered\n", proc->serial, proc->pp_peb );
}


/***********************************************************************
 *           ios_proc_unregister
 *
 * Forgets a dead pseudo-process's slot, so a later one whose PEB lands at the same address
 * starts clean (Part D1).
 */
void ios_proc_unregister( struct ios_proc *proc )
{
    if (proc->pp_peb) ios_registry_remove( &proc_registry, proc->pp_peb );
}


/***********************************************************************
 *           ios_stage_name
 */
const char *ios_stage_name( LONG stage )
{
    switch (stage)
    {
    case IOS_STAGE_CREATED:      return "created";
    case IOS_STAGE_THREAD:       return "boot thread started";
    case IOS_STAGE_WINDOW:       return "guest window reserved";
    case IOS_STAGE_TEB:          return "TEB and PEB allocated";
    case IOS_STAGE_CONNECTED:    return "connected to the server";
    case IOS_STAGE_STARTUP_INFO: return "main image mapped";
    case IOS_STAGE_NTDLL:        return "ntdll loaded";
    case IOS_STAGE_RUNNING:      return "running";
    }
    return "?";
}


/***********************************************************************
 *           ios_proc_set_stage
 */
void ios_proc_set_stage( struct ios_proc *proc, enum ios_boot_stage stage )
{
    if (proc == &no_proc) return;
    InterlockedExchange( (LONG *)&proc->stage, stage );
    if (stage == IOS_STAGE_RUNNING) InterlockedExchange( &proc->state, IOS_PROC_RUNNING );
    if (!ios_quiet) ios_log( "process %u (%s): %s\n", proc->serial, proc->name, ios_stage_name( stage ));
}


/***********************************************************************
 *           ios_proc_set_main_thread
 *
 * Spec 22 C3: the calling thread boots proc (start_main_thread), so it is its main thread.
 */
void ios_proc_set_main_thread( struct ios_proc *proc )
{
    if (proc == &no_proc) return;
    proc->main_thread = pthread_self();
    __atomic_store_n( &proc->have_main, TRUE, __ATOMIC_RELEASE );
}


/***********************************************************************
 *           ios_wow_base
 *
 * Spec 06: the guest window base of the calling thread's program, 0 for a 64-bit program.
 */
unsigned long ios_wow_base(void)
{
    return ios_proc_current()->wow_base;
}

#endif /* WINE_IOS */
