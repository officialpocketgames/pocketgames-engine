// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Parts B, C and D: what ntdll's modified upstream files call (ios_s17.c). Included by
// unix_private.h (WINE_IOS only), after Wine's types; ios_s17.c includes it on its own.

#ifndef MYIOSDECK_IOS_S17_H
#define MYIOSDECK_IOS_S17_H

#include "ios_s17_pure.h"

/* ---- the switches of Part B, read once per app run ---- */

#define IOS_S17_WOW64_BY_TEB     0x1   /* MADEIRA_WOW64_BY_TEB, B1 */
#define IOS_S17_IO_STATUS_OWNER  0x2   /* MADEIRA_IO_STATUS_OWNER, B2 */
#define IOS_S17_SECTION_LIMIT    0x4   /* MADEIRA_SECTION_PROCESS_LIMIT, B4 */

extern int ios_s17_switch_bits;        /* -1 until read */
extern int ios_s17_switches_init(void);

/* one load in the steady state */
static inline int ios_s17_switch( int bit )
{
    int bits = __atomic_load_n( &ios_s17_switch_bits, __ATOMIC_RELAXED );
    if (bits < 0) bits = ios_s17_switches_init();
    return (bits & bit) != 0;
}

/* ---- Part C ---- */

/* C1: the serial of a 32-bit caller's volume (the stored value of the prefix, cached for the
 * app run, hashed with the drive letter the mount query left); 0 if there is no stored value */
extern ULONG ios_s17_wow_volume_serial( const char *config_dir, unsigned char letter );
/* C1: FileFsVolumeInformation for a 32-bit caller when the mount manager failed; buffer holds at
 * least sizeof(FILE_FS_VOLUME_INFORMATION). STATUS_NOT_IMPLEMENTED for a zero serial. */
extern NTSTATUS ios_s17_volume_info( void *buffer, ULONG serial, ULONG_PTR *information );
/* C2: log a failed NtCreateFile or NtOpenFile (api names it) within its budget */
extern void ios_s17_log_failed_open( const char *api, NTSTATUS status, ULONG disposition,
                                     ACCESS_MASK access, ULONG options, ULONG sharing,
                                     const char *unix_name, const UNICODE_STRING *nt_name );
/* C2 addendum: log a failed NtQueryAttributesFile or NtQueryFullAttributesFile likewise */
extern void ios_s17_log_failed_query( const char *api, NTSTATUS status, const char *unix_name,
                                      const UNICODE_STRING *nt_name );

/* ---- Part D ---- */

/* D3: the processor count Windows code sees, given the host's (decided once, logged once) */
extern long ios_s17_cpu_count( long host );
/* D4 with clarification 2: one futex queue table per pseudo-process, and the private information
 * class that hands the caller's out */
#define IOS_S17_FUTEX_INFO_CLASS 0xf00d
#define IOS_S17_FUTEX_QUEUES     256
/* the calling thread's pseudo-process's table, created zero-filled on first use; NULL for a
 * thread with no pseudo-process (ios_process.c) */
extern void *ios_get_process_futex_queues(void);
/* the table in *slot, created (zero-filled, published atomically) if it is still NULL; NULL only
 * when out of memory */
extern void *ios_s17_futex_table_get( void **slot );
/* frees the table in *slot (once no thread of its process can run any more) */
extern void ios_s17_futex_table_free( void **slot );
/* class 0xf00d: FALSE for a size other than a pointer's (upstream's handling); otherwise TRUE
 * with *status: STATUS_ACCESS_VIOLATION for a NULL buffer, STATUS_UNSUCCESSFUL when the caller
 * has no pseudo-process (or no memory), else the table's address and STATUS_SUCCESS */
extern BOOL ios_s17_query_futex_queues( void *info, ULONG size, ULONG *ret_size,
                                        NTSTATUS *status );
/* D5 */
extern const char *ntdll_syscall_name( UINT id );
/* D6: ntdll's unix calls 8-12 (64-bit layout), and their 32-bit answer */
extern NTSTATUS ios_unixcall_push_jit_aliases( void *args );
extern NTSTATUS ios_unixcall_register_hold_release( void *args );
extern NTSTATUS ios_unixcall_jit_alias_probe( void *args );
extern NTSTATUS ios_unixcall_mono_bridge_ptr( void *args );
extern NTSTATUS ios_unixcall_get_fex_arena( void *args );
extern NTSTATUS ios_unixcall_wow64_not_supported( void *args );

#endif /* MYIOSDECK_IOS_S17_H */
