// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Parts B, C and D: the pieces of ntdll's smaller unix-side changes that need Wine's
// types but not ntdll's private state (no unix_private.h), so the host tests can link them with
// a fake ios_log: the switches, the volume serial of 32-bit programs, the failed-open log, the
// CPU count, the shared futex queue table and ntdll's unix calls 8-12.

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winioctl.h"
#include "unixlib.h"

#include "ios_s17.h"

extern void ios_log( const char *fmt, ... ) __attribute__((format(printf,1,2)));   /* ios_process.c */

/* ---------------------------------------------------------------- switches (Part B) */

int ios_s17_switch_bits = -1;

int ios_s17_switches_init(void)
{
    int bits = 0;

    if (ios_s17_switch_value( getenv( "MADEIRA_WOW64_BY_TEB" ))) bits |= IOS_S17_WOW64_BY_TEB;
    if (ios_s17_switch_value( getenv( "MADEIRA_IO_STATUS_OWNER" ))) bits |= IOS_S17_IO_STATUS_OWNER;
    if (ios_s17_switch_value( getenv( "MADEIRA_SECTION_PROCESS_LIMIT" ))) bits |= IOS_S17_SECTION_LIMIT;
    /* every racer computes the same value from the same environment */
    __atomic_store_n( &ios_s17_switch_bits, bits, __ATOMIC_RELAXED );
    return bits;
}

/* ---------------------------------------------------------------- Part C1 */

static uint32_t stored_serial;   /* the prefix's stored value once known; 0: not yet */

static uint32_t draw_random(void)
{
    return arc4random();
}

ULONG ios_s17_wow_volume_serial( const char *config_dir, unsigned char letter )
{
    uint32_t stored = __atomic_load_n( &stored_serial, __ATOMIC_RELAXED );

    if (!stored)
    {
        /* racers end with the same file content, so any of them may store it */
        if (!(stored = ios_s17_load_serial( config_dir, draw_random ))) return 0;
        __atomic_store_n( &stored_serial, stored, __ATOMIC_RELAXED );
    }
    return ios_s17_volume_serial( stored, letter );
}

NTSTATUS ios_s17_volume_info( void *buffer, ULONG serial, ULONG_PTR *information )
{
    FILE_FS_VOLUME_INFORMATION *info = buffer;

    if (!serial) return STATUS_NOT_IMPLEMENTED;
    info->VolumeCreationTime.QuadPart = 0;
    info->VolumeSerialNumber = serial;
    info->VolumeLabelLength = 0;
    info->SupportsObjects = TRUE;
    *information = offsetof( FILE_FS_VOLUME_INFORMATION, VolumeLabel );
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- Part C2 */

static struct ios_s17_budgets open_budgets;

/* a line from the budget of its kind; the printable names in unix_buf and nt_buf */
static int failure_line( int is_write, const char *unix_name, const UNICODE_STRING *nt_name,
                         char *unix_buf, char *nt_buf, size_t size )
{
    const uint16_t *name = (nt_name && nt_name->Buffer) ? (const uint16_t *)nt_name->Buffer : NULL;
    size_t len = name ? nt_name->Length / sizeof(WCHAR) : 0;

    if (!ios_s17_budget_take( &open_budgets, ios_s17_budget_kind( is_write, name, len ))) return 0;
    ios_s17_ascii_a( unix_buf, size, unix_name ? unix_name : "(none)" );
    if (name) ios_s17_ascii_w( nt_buf, size, name, len );
    else ios_s17_ascii_a( nt_buf, size, "(none)" );
    return 1;
}

void ios_s17_log_failed_open( const char *api, NTSTATUS status, ULONG disposition,
                              ACCESS_MASK access, ULONG options, ULONG sharing,
                              const char *unix_name, const UNICODE_STRING *nt_name )
{
    char unix_buf[384], nt_buf[384];
    int is_write = ios_s17_open_is_write( access, disposition );

    if (!failure_line( is_write, unix_name, nt_name, unix_buf, nt_buf, sizeof(unix_buf) )) return;

    if (is_write)
        ios_log( "%s failed: status %#x disposition %u access %#x options %#x share %#x unix %s nt %s\n",
                 api, (unsigned int)status, (unsigned int)disposition, (unsigned int)access,
                 (unsigned int)options, (unsigned int)sharing, unix_buf, nt_buf );
    else
        ios_log( "%s failed: status %#x disposition %u access %#x options %#x unix %s nt %s\n",
                 api, (unsigned int)status, (unsigned int)disposition, (unsigned int)access,
                 (unsigned int)options, unix_buf, nt_buf );
}

void ios_s17_log_failed_query( const char *api, NTSTATUS status, const char *unix_name,
                               const UNICODE_STRING *nt_name )
{
    char unix_buf[384], nt_buf[384];

    if (!failure_line( 0, unix_name, nt_name, unix_buf, nt_buf, sizeof(unix_buf) )) return;
    ios_log( "%s failed: status %#x unix %s nt %s\n", api, (unsigned int)status, unix_buf, nt_buf );
}

/* ---------------------------------------------------------------- Part D3 */

static int cpu_override = -1;   /* -1: not decided yet; 0: none */

long ios_s17_cpu_count( long host )
{
    int count = __atomic_load_n( &cpu_override, __ATOMIC_ACQUIRE );

    if (count < 0)
    {
        char value[64] = "";
        int expected = -1;
        const char *env = getenv( "MADEIRA_CPU_COUNT" );

        ios_s17_cfg_file_get( getenv( "MADEIRA_DOCS_DIR" ), "cpu-count", value, sizeof(value) );
        count = ios_s17_cpu_override( env, value );
        if (__atomic_compare_exchange_n( &cpu_override, &expected, count, FALSE,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
        {
            if (count) ios_log( "processor count: reporting %d (host has %ld)\n", count, host );
        }
        else count = expected;
    }
    return count ? count : host;
}

/* ---------------------------------------------------------------- Part D4 */

/* laid out as the PE side's queue head of RtlWaitOnAddress; zero is an empty queue that the PE
 * side initialises on first use */
struct ios_futex_queue
{
    void *next;
    void *prev;
    LONG  lock;
};

#ifdef _WIN64
_Static_assert( sizeof(struct ios_futex_queue) == 24, "futex queue head" );
#endif

/* Clarification 2: one table per pseudo-process, never handed to another one. A process that
 * ended with a thread parked in RtlWaitOnAddress (suspended for good) leaves that thread's wait
 * entry linked in its queues; a later program sharing the table walked into it. */
void *ios_s17_futex_table_get( void **slot )
{
    void *table = __atomic_load_n( slot, __ATOMIC_ACQUIRE ), *expected = NULL;

    if (table) return table;
    if (!(table = calloc( IOS_S17_FUTEX_QUEUES, sizeof(struct ios_futex_queue) ))) return NULL;
    /* two images of one process may ask at once: the first table published wins */
    if (__atomic_compare_exchange_n( slot, &expected, table, FALSE, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE ))
        return table;
    free( table );
    return expected;
}

void ios_s17_futex_table_free( void **slot )
{
    free( __atomic_exchange_n( slot, NULL, __ATOMIC_ACQ_REL ));
}

BOOL ios_s17_query_futex_queues( void *info, ULONG size, ULONG *ret_size, NTSTATUS *status )
{
    void *table;

    if (size != sizeof(void *)) return FALSE;   /* upstream's handling (an unknown class) */
    if (!info)
    {
        *status = STATUS_ACCESS_VIOLATION;
        return TRUE;
    }
    if (!(table = ios_get_process_futex_queues()))
    {
        /* no pseudo-process: the caller (spec 16's PE side) keeps its image's own table */
        *status = STATUS_UNSUCCESSFUL;
        return TRUE;
    }
    *(void **)info = table;
    if (ret_size) *ret_size = sizeof(void *);
    *status = STATUS_SUCCESS;
    return TRUE;
}

/* ---------------------------------------------------------------- Part D6 */

/* This build runs no JIT (spec 10's scope): there are no aliases, nothing to release, no
 * Mono bridge and no emulator arena. The calls check their blocks and answer accordingly. */

NTSTATUS ios_unixcall_push_jit_aliases( void *args )
{
    struct ios_push_jit_aliases_params *params = args;

    if (!params || !params->callback) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS ios_unixcall_register_hold_release( void *args )
{
    struct ios_register_hold_release_params *params = args;

    if (!params || params->size != sizeof(*params) || params->version != IOS_HOLD_RELEASE_VERSION ||
        !params->peb || !params->callback)
        return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;
}

NTSTATUS ios_unixcall_jit_alias_probe( void *args )
{
    struct ios_jit_alias_probe_params *params = args;
    size_t after_addr = offsetof( struct ios_jit_alias_probe_params, addr ) + sizeof(params->addr);

    if (!params || params->size != sizeof(*params) || params->version != IOS_JIT_ALIAS_PROBE_VERSION)
        return STATUS_INVALID_PARAMETER;
    /* every field after addr: "not aliased" */
    memset( (char *)params + after_addr, 0, sizeof(*params) - after_addr );
    return STATUS_SUCCESS;
}

NTSTATUS ios_unixcall_mono_bridge_ptr( void *args )
{
    if (args) *(ULONG64 *)args = 0;
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS ios_unixcall_get_fex_arena( void *args )
{
    struct ios_get_fex_arena_params *params = args;

    if (!params) return STATUS_INVALID_PARAMETER;
    params->base = 0;
    params->end = 0;
    return STATUS_SUCCESS;
}

NTSTATUS ios_unixcall_wow64_not_supported( void *args )
{
    return STATUS_NOT_SUPPORTED;
}
