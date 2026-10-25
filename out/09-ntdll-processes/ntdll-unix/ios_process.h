// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part B: the pseudo-process.
//
// Upstream keeps a process's state in globals of dlls/ntdll/unix/. Here every Windows program is a
// group of threads of one app, so all pseudo-processes share one copy of this library. Every
// global that is really per process (CHANGES.md, "Audit", class (c)) lives in a struct ios_proc
// instead, one per pseudo-process, and the upstream name of the global becomes a macro for the
// calling thread's field: upstream code such as "peb->ProcessParameters" or "if (process_exiting)"
// stays as it is and reads the right pseudo-process.
//
// The calling thread's struct ios_proc is found
//   1. through its TEB (the thread data keeps a pointer, set when the TEB is made), then
//   2. through the registry keyed by the TEB's PEB (lock-free, ios_pure.c), then
//   3. through a thread-local override: a booting thread before it has a TEB, or the reaper
//      thread that cleans up after a dead pseudo-process,
// and, failing all of them, a static "no process" with neutral values, so a native thread that
// strays into this code reads harmless defaults instead of crashing.
//
// This header is included by unix_private.h (only with WINE_IOS) in place of the upstream
// "extern" declarations of the globals it replaces. The fields are named pp_<upstream name> so
// that the macros below don't expand inside this struct or inside ios_process.c.
//
// Specs 10 (memory) and 11 (exceptions and signals) add their per-process fields here.

#ifndef MYIOSDECK_IOS_PROCESS_H
#define MYIOSDECK_IOS_PROCESS_H

#ifdef WINE_IOS

#include <pthread.h>
#include "ios_pure.h"

#define IOS_FD_CACHE_ENTRIES          128   /* = FD_CACHE_ENTRIES in server.c */
#define IOS_INPROC_SYNC_CACHE_ENTRIES 128   /* = INPROC_SYNC_CACHE_ENTRIES in sync.c */

union fd_cache_entry;
struct inproc_sync;
struct dir_data;

/* where a pseudo-process is in its boot (Part C4: a child that dies during boot logs it) */
enum ios_boot_stage
{
    IOS_STAGE_CREATED,        /* struct made, boot thread not started yet */
    IOS_STAGE_THREAD,         /* boot thread running */
    IOS_STAGE_WINDOW,         /* 32-bit: guest window reserved */
    IOS_STAGE_TEB,            /* first TEB and PEB allocated */
    IOS_STAGE_CONNECTED,      /* first server request answered */
    IOS_STAGE_STARTUP_INFO,   /* process parameters built, main image mapped */
    IOS_STAGE_NTDLL,          /* ntdll.dll (and the wow64 ntdll) loaded */
    IOS_STAGE_RUNNING,        /* init_process_done sent, entry point reached */
};

/* life of a pseudo-process */
enum ios_proc_state
{
    IOS_PROC_BOOTING,
    IOS_PROC_RUNNING,
    IOS_PROC_ENDING,          /* one thread is ending it (Part D) */
    IOS_PROC_DEAD,            /* handed to the reaper */
};

struct ios_proc
{
    /* ---- upstream per-process globals, class (c) of the audit ---- */
    /* env.c */
    PEB                       *pp_peb;
    WOW_PEB                   *pp_wow_peb;
    SIZE_T                     pp_startup_info_size;
    BOOL                       pp_is_prefix_bootstrap;
    int                        pp_main_argc;
    char                     **pp_main_argv;
    WCHAR                    **pp_main_wargv;
    /* loader.c */
    SECTION_IMAGE_INFORMATION  pp_main_image_info;
    void                      *pp_pDbgUiRemoteBreakin;
    void                      *pp_pKiRaiseUserExceptionDispatcher;
    void                      *pp_pKiUserExceptionDispatcher;
    void                      *pp_pKiUserApcDispatcher;
    void                      *pp_pKiUserCallbackDispatcher;
    void                      *pp_pKiUserEmulationDispatcher;
    void                      *pp_pLdrInitializeThunk;
    void                      *pp_pRtlUserThreadStart;
    void                      *pp_p__wine_ctrl_routine;
    SYSTEM_DLL_INIT_BLOCK     *pp_pLdrSystemDllInitBlock;
    /* loadorder.c */
    HANDLE                     pp_lo_std_key;
    HANDLE                     pp_lo_app_key;
    BOOL                       pp_lo_init_done;
    BOOL                       pp_lo_main_exe_loaded;
    /* process.c */
    ULONG                      pp_execute_flags;
    UINT                       pp_process_error_mode;
    ULONG                      pp_process_cookie;
    /* server.c */
    BOOL                       pp_process_exiting;
    int                        pp_fd_socket;
    int                        pp_initial_cwd;
    union fd_cache_entry      *pp_fd_cache[IOS_FD_CACHE_ENTRIES];
    /* sync.c */
    HANDLE                     pp_keyed_event;
    int                        pp_inproc_device_fd;
    struct inproc_sync        *pp_inproc_sync_cache[IOS_INPROC_SYNC_CACHE_ENTRIES];
    /* thread.c */
    LONG                       pp_nb_threads;
    void                      *pp_prev_teb;
    /* virtual.c (TEB/PEB blocks; the rest of memory is spec 10) */
    void                      *pp_teb_block;
    void                     **pp_next_free_teb;
    int                        pp_teb_block_pos;
    struct list                pp_teb_list;
    ULONG_PTR                  pp_user_space_wow_limit;
    void                      *pp_arm64ec_view;
    BOOL                       pp_force_exec_prot;
    BOOL                       pp_enable_write_exceptions;
    /* file.c */
    struct dir_data          **pp_dir_data_cache;
    unsigned int               pp_dir_data_cache_size;

    /* ---- MYIOSDECK bookkeeping ---- */
    unsigned int               serial;        /* 1, 2, 3... in start order, for logs */
    LONG                       state;         /* enum ios_proc_state */
    volatile LONG              stage;         /* enum ios_boot_stage */
    BOOL                       first;         /* the session's first program (__wine_main) */
    BOOL                       top_level;     /* spec 22: started by the app, no parent (the first
                                                 program and every later one) */
    BOOL                       have_main;     /* spec 22 C3: main_thread is set */
    pthread_t                  main_thread;   /* the thread that booted it */
    char                       name[64];      /* image file name, for logs and live children */
    unsigned int               child_id;      /* ios_children id; 0 when not tracked */
    unsigned long              wow_base;      /* guest window base (32-bit programs), else 0 */
    unsigned long              wow_size;
    BOOL                       wow_lowfirst;  /* spec 10 E5: allocations try the low 2 GB first */
    int                        wow_slot;      /* spec 10 E2: its window slot, -1: none */
    unsigned int               refused_images[16]; /* spec 10 cl. 8: name hashes already logged */
    unsigned int               refused_count;
    unsigned int               native_images[16];  /* spec 10 cl. 9: native copies already logged */
    unsigned int               native_count;
    unsigned int               reused_images[16];  /* spec 10 cl. 10: second mappings already logged */
    unsigned int               reused_count;
    USHORT                     machine;       /* main image machine known before boot (children) */
    BOOL                       il_only_64;    /* IL-only .NET image without 32BITREQUIRED */
    pthread_t                  boot_thread;   /* children: the thread that boots it (detached) */
    int                        boot_socket;   /* children: their end of the server socket pair */
    int                        boot_argc;     /* children: arguments copied from the parent */
    char                     **boot_argv;
    int                        exit_code;     /* Windows exit code once ending */
    void                      *futex_queues;  /* spec 17 clarification 2: this process's futex
                                                 queue table (class 0xf00d), NULL until asked;
                                                 freed with the struct */
    pthread_mutex_t            mutex;         /* protects the fields below */
    pthread_cond_t             cond;
    BOOL                       session_parked; /* the thread that called __wine_main left the
                                                  program and waits for its end (Part C2) */
    BOOL                       all_left;      /* every thread has left (the struct can go) */
};

/* ---- interfaces/wine_unix_contract.h, the names this library shares with the app ---- */
#include <setjmp.h>
extern void __wine_main( int argc, char *argv[] );                       /* Section A */
extern void wine_log_set_file( const char *path );                        /* Section A */
extern int ios_main_image_i386;                                           /* Section A */
extern int pg_live_game_children( char *buf, int len, double max_age ); /* Section A */
extern _Thread_local jmp_buf wine_ios_exit_jmpbuf;                        /* Section B */
extern _Thread_local volatile int wine_ios_exit_code;
extern _Thread_local pthread_t wine_ios_main_thread;
extern _Thread_local int wine_ios_exit_initialized;
extern void wine_launched_process_did_exit( int status ) __attribute__((weak_import));
extern void pgios_phase( const char *name );

/* ---- the calling thread's pseudo-process ---- */

static inline struct ios_proc *ios_proc_current(void);   /* defined in unix_private.h */
extern struct ios_proc *ios_proc_current_slow(void);
extern struct ios_proc *ios_proc_from_peb( const void *peb_ptr );
extern void ios_set_thread_proc( struct ios_proc *proc );  /* thread-local override */
extern TEB *ios_current_teb(void);                        /* NULL on a thread with no TEB */
extern pthread_key_t teb_key;
extern int ios_teb_key_ready;

/* Part B1 */
extern void *ios_current_peb(void);

/* the upstream globals, per pseudo-process */
/* "peb" has no macro: upstream's uses read ios_cur_peb() and write ios_set_cur_peb()
 * (unix_private.h), so the name stays free for members and parameters */
#define wow_peb                              (ios_proc_current()->pp_wow_peb)
#define startup_info_size                    (ios_proc_current()->pp_startup_info_size)
#define is_prefix_bootstrap                  (ios_proc_current()->pp_is_prefix_bootstrap)
#define main_argc                            (ios_proc_current()->pp_main_argc)
#define main_argv                            (ios_proc_current()->pp_main_argv)
#define main_wargv                           (ios_proc_current()->pp_main_wargv)
#define main_image_info                      (ios_proc_current()->pp_main_image_info)
#define pDbgUiRemoteBreakin                  (ios_proc_current()->pp_pDbgUiRemoteBreakin)
#define pKiRaiseUserExceptionDispatcher      (ios_proc_current()->pp_pKiRaiseUserExceptionDispatcher)
#define pKiUserExceptionDispatcher           (ios_proc_current()->pp_pKiUserExceptionDispatcher)
#define pKiUserApcDispatcher                 (ios_proc_current()->pp_pKiUserApcDispatcher)
#define pKiUserCallbackDispatcher            (ios_proc_current()->pp_pKiUserCallbackDispatcher)
#define pKiUserEmulationDispatcher           (ios_proc_current()->pp_pKiUserEmulationDispatcher)
#define pLdrInitializeThunk                  (ios_proc_current()->pp_pLdrInitializeThunk)
#define pRtlUserThreadStart                  (ios_proc_current()->pp_pRtlUserThreadStart)
#define p__wine_ctrl_routine                 (ios_proc_current()->pp_p__wine_ctrl_routine)
#define pLdrSystemDllInitBlock               (ios_proc_current()->pp_pLdrSystemDllInitBlock)
#define process_cookie                       (ios_proc_current()->pp_process_cookie)
#define process_exiting                      (ios_proc_current()->pp_process_exiting)
#define inproc_device_fd                     (ios_proc_current()->pp_inproc_device_fd)
#define user_space_wow_limit                 (ios_proc_current()->pp_user_space_wow_limit)
/* keyed_event is also a member name (select_op.keyed_event): its few uses name the field */
#define ios_keyed_event                      (ios_proc_current()->pp_keyed_event)

/* SYSTEM_DLL_INIT_BLOCK has members with the names of the macros above; loader.c reads and
 * writes them in a part of the file where the macros are undefined */
#define IOS_INIT_BLOCK_VALUES 8
extern void ios_fill_wow64_init_block( SYSTEM_DLL_INIT_BLOCK *block, ULONG_PTR ntdll,
                                       const ULONG_PTR *values );
extern ULONG_PTR ios_wow64_thread_start(void);   /* the block's pRtlUserThreadStart */

/* ---- logging (to stderr, which the app points at its log) ---- */
extern void ios_log( const char *fmt, ... ) __attribute__((format(printf,1,2)));
extern void ios_log_write( const char *buf, int len );   /* debug.c: the log fd */
extern int ios_quiet;   /* PG_QUIET=1: no routine lines */

/* ---- Part A ---- */
extern NTSTATUS ios_unixlib_table( const char *dll_name, BOOL wow64, const void **funcs );
extern BOOL ios_module_export_name( const void *module, char *buf, size_t size );
extern BOOL ios_unicode_dll_name( const UNICODE_STRING *name, char *buf, size_t size );

/* ---- Part B ---- */
extern void ios_process_init_once(void);
extern struct ios_proc *ios_proc_alloc( BOOL first );
extern void ios_proc_set_stage( struct ios_proc *proc, enum ios_boot_stage stage );
extern void ios_proc_register( struct ios_proc *proc );  /* publish proc under its PEB */
extern void ios_proc_unregister( struct ios_proc *proc );
extern void ios_proc_free( struct ios_proc *proc );
extern const char *ios_stage_name( LONG stage );
/* guest addresses of a 32-bit program (spec 06): below 4 GB they are relative to the window */
extern unsigned long ios_wow_base(void);
static inline void *ios_host_ptr( const void *ptr )
{
    ULONG_PTR addr = (ULONG_PTR)ptr;
    if (addr && addr < ((ULONG_PTR)1 << 32)) return (void *)(addr + ios_wow_base());
    return (void *)ptr;
}

/* ---- Part C ---- */
extern void ios_boot_pseudo_process( struct ios_proc *proc ) __attribute__((noreturn)); /* loader.c */
extern void ios_boot_phase( const char *name );   /* pgios_phase for the first program */
/* one immediate, always-on "ntdll-ios: boot: ..." log line per boot step (ios_process.c) */
extern void ios_boot_mark( const char *fmt, ... ) __attribute__((format(printf,1,2)));
/* a fault logger for the boot thread before spec 11's handlers exist (ios_process.c) */
extern void ios_boot_fault_logger_install(void);
extern void ios_boot_fault_logger_remove(void);
/* prefix + message to the log fd, for upstream's fatal errors that print to stderr */
extern void ios_vlog( const char *prefix, const char *fmt, va_list args );
extern NTSTATUS ios_start_boot_thread( struct ios_proc *proc );   /* ios_spawn.c */
extern NTSTATUS ios_check_refused_spawn( const UNICODE_STRING *image );
extern void ios_mark_system_process(void);
extern WCHAR *ios_initial_directory(void);         /* PG_INITIAL_CWD as an NT path */
extern double ios_monotonic_seconds(void);

/* ---- Part D ---- */
extern void ios_boot_failed( const char *why, NTSTATUS status ) __attribute__((noreturn));
/* Part C2: the thread that called __wine_main; its own exit buffer (ios_exit.c) */
extern BOOL ios_is_session_thread(void);
extern void ios_set_own_exit_buf( jmp_buf *buf );
extern int ios_own_exit_code(void);
extern void ios_assert_fail( const char *expr, const char *file, int line ) __attribute__((noreturn));
extern void ios_thread_started( DWORD tid, unsigned int unix_tid );
extern void ios_end_pseudo_process( int exit_code ) __attribute__((noreturn));
extern void ios_thread_leave( int status ) __attribute__((noreturn));
extern void ios_close_thread_fds(void);            /* thread.c */
extern void ios_thread_alert( unsigned int unix_tid );
extern void ios_alert_own_process(void);
extern void ios_handle_poke(void);
extern void ios_alert_wake( DWORD tid );           /* sync.c: wake tid's in-process waits */
extern void ios_fd_cache_drop( struct ios_proc *proc );    /* server.c */
extern void ios_inproc_cache_drop( struct ios_proc *proc ); /* sync.c */
extern void ios_dir_cache_drop( struct ios_proc *proc );   /* file.c */

/* ---- spec 22 Part C (ios_exit.c, loader.c) ---- */
/* contract Section A: a later program of a kept session, booted on the calling thread */
extern int ios_session_start_program( int argc, char *argv[], int server_fd, int i386 );
/* C6: cross-library name (other unix libraries call it); at most 16, each once */
extern int ios_process_end_hook_add( void (*fn)( const void *peb ));
/* C3: the server ended the calling thread (its pipes closed): mark it, then abort_thread */
extern void ios_thread_killed( int status ) __attribute__((noreturn));
extern BOOL ios_thread_was_killed(void);
/* C3: the calling thread is the one that booted its pseudo-process */
extern void ios_proc_set_main_thread( struct ios_proc *proc );
/* C5 (ios_mach_thread.c): a thread by its Mach port name (its unix tid) */
extern int ios_mach_thread_waiting( unsigned int port );   /* 1 blocked in the kernel, 0 not, -1 unknown */
extern int ios_mach_thread_suspend( unsigned int port );   /* 0 when suspended */

/* memory hooks (spec 10, at the end of virtual.c) */
extern NTSTATUS ios_mem_reserve_wow_window( struct ios_proc *proc );
extern void ios_mem_thread_gone( TEB *teb );
extern void ios_mem_process_ending( struct ios_proc *proc );
extern void ios_mem_release_process( struct ios_proc *proc, BOOL threads_left );
extern void ios_dylib_view_gone( const void *base );         /* spec 22 C7 (ios_dylib.c) */
extern void ios_vm_bind_window( struct ios_proc *proc );     /* spec 10 E3 */
extern void ios_vm_window_abandon( struct ios_proc *proc );  /* spec 10 E8 */

/* spec 10 exports for the other parts */
extern unsigned long ios_wow_base_for_peb( void *peb_ptr );          /* E3 */
extern int ios_wow_in_window( const void *addr );                    /* E9 */
extern ULONG ios_wow_guest_addr( const void *host );                 /* E9 */
extern unsigned long long ios_get_real_usd(void);                    /* clarification 1 */
extern uint64_t ios_lowwin_translate( uint64_t addr, void *peb_ptr ); /* clarification 1 */
extern void ios_wow_fixup_peb64_ptrs(void);                          /* clarification 2 */
extern int ios_wow_session_armed(void);                              /* clarification 3 */

/* ---- Part E ---- */
extern void ios_set_thread_qos( pthread_attr_t *attr );
extern void wineserver_request_wake(void);         /* spec 08 */
struct syscall_frame;
extern void ios_cpu_area_sync( struct syscall_frame *frame, const ULONG64 *args ); /* signal_arm64_ios.c */

/* ---- spec 11 (signal_arm64_ios.c) ---- */
extern void ios_signal_thread_leave(void);   /* Part B/D: called by ios_thread_leave */

/* ---- Part F ---- */
extern void ios_create_dos_devices(void);
extern void ios_write_hardware_registry(void);

#endif /* WINE_IOS */

#endif /* MYIOSDECK_IOS_PROCESS_H */
