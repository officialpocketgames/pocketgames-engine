// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 10: the pure parts of memory on iOS. Only the C library is used, so
// tests/test_ios_vm_pure.c runs them on a host with fake bases. virtual.c and ios_dylib.c
// use them with the real address space.

#ifndef MYIOSDECK_IOS_VM_PURE_H
#define MYIOSDECK_IOS_VM_PURE_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#define IOS_4G              ((uint64_t)1 << 32)
#define IOS_2G              ((uint64_t)1 << 31)
#define IOS_GRANULARITY     ((uint64_t)0x10000)
#define IOS_LOWEST_ADDRESS  (IOS_4G + IOS_GRANULARITY)   /* Part A1 */

/* ---------- Part A2: the highest user address ---------- */

/* What 64-bit programs are told: the task's max_address rounded down to 64 KB, never more than
 * the Windows limit. A max_address of 0 (unknown) gives the Windows limit. */
uint64_t ios_vm_user_limit( uint64_t task_max_address, uint64_t windows_limit );

/* ---------- Part E4/E5: guest limits ---------- */

/* The ceiling reported to 32-bit code (exclusive): 4 GB, or 2 GB for an image without
 * IMAGE_FILE_LARGE_ADDRESS_AWARE when MADEIRA_LAA is "0". */
uint64_t ios_wow_ceiling( int large_address_aware, const char *env_laa );

/* Whether allocations try the low 2 GB first: an image without the flag that was given 4 GB,
 * unless MADEIRA_LAA_LOWFIRST is "0". */
int ios_wow_lowfirst( int large_address_aware, uint64_t ceiling, const char *env_lowfirst );

/* Translates an allocation's limits (inclusive high limit, 0: none). For a 32-bit process
 * (wow_base != 0) a high limit below 4 GB is a guest number: the range becomes
 * [wow_base + max(low, 64 KB), wow_base + min(high, ceiling - 1)]. For a 64-bit process a high limit
 * below the lowest address can't be met. Returns 0, or -1 when the request can't be met.
 * Limits that aren't guest numbers are left as they are. */
int ios_vm_translate_limits( uint64_t wow_base, uint64_t ceiling, uint64_t *low, uint64_t *high );

/* ---------- Part E2: guest window slots ---------- */

#define IOS_WIN_SLOTS 8

enum ios_win_state
{
    IOS_WIN_EMPTY,      /* no reservation */
    IOS_WIN_FREE,       /* reserved, unused (a torn-down window keeps its reservation) */
    IOS_WIN_RESERVED,   /* taken by a booting process, not bound to its PEB yet */
    IOS_WIN_BOUND,      /* bound to a live process's PEB */
    IOS_WIN_RELEASED,   /* its process ended; torn down when a slot is needed */
    IOS_WIN_ABANDONED,  /* never reused (a thread was left in it, or its process stopped being 32-bit) */
};

struct ios_win_slot
{
    int          state;     /* enum ios_win_state */
    uint64_t     base;      /* B; 0 while EMPTY */
    const void  *peb;       /* BOUND: the process's PEB */
    const void  *owner;     /* the process that took it (RESERVED, BOUND, RELEASED) */
};

struct ios_win_table
{
    struct ios_win_slot slot[IOS_WIN_SLOTS];
};

struct ios_win_ops
{
    void *ctx;
    /* any thread of the slot's old process left? (alive, suspended or parked) */
    int (*threads_left)( void *ctx, struct ios_win_slot *slot );
    /* alert those threads and wait up to 0.5 s; returns what threads_left says afterwards */
    int (*alert_and_wait)( void *ctx, struct ios_win_slot *slot );
    /* remove every view inside the window and put the reservation back; 0 on success */
    int (*teardown)( void *ctx, struct ios_win_slot *slot );
    /* reserve a new window, set slot->base; 0 on success */
    int (*reserve)( void *ctx, struct ios_win_slot *slot );
    /* a line for the log */
    void (*log)( void *ctx, const char *what, const struct ios_win_slot *slot );
};

/* Takes a slot for a booting 32-bit process: a FREE one, else a RELEASED one that can be torn
 * down (one with threads left is abandoned), else a new reservation. Returns the slot index,
 * or -1 if none can be had. The slot is RESERVED for owner. */
int ios_win_take( struct ios_win_table *t, const struct ios_win_ops *ops, const void *owner );

/* Binds the slot owner reserved to its PEB. Returns the index or -1. */
int ios_win_bind( struct ios_win_table *t, const void *owner, const void *peb );

/* The process ended: its slot becomes RELEASED (stops resolving for new bindings). */
int ios_win_release( struct ios_win_table *t, const void *owner );

/* The process stopped being 32-bit (Part E8): its slot is abandoned. */
int ios_win_abandon_owner( struct ios_win_table *t, const void *owner );

/* The bound window of a PEB, or 0. */
uint64_t ios_win_base_for_peb( const struct ios_win_table *t, const void *peb );

/* ---------- Part D: the fixed exe window ---------- */

struct ios_exewin
{
    uint64_t     base, size;      /* the window the app reserved; size 0: none */
    const void  *owner;           /* the occupant's process, or NULL */
    uint64_t     claim_base;      /* the occupant's image base */
    uint64_t     claim_size;      /* its size in host-page units (bytes, rounded) */
};

/* "<hex base>:<hex size>" (no 0x). Returns 0 and fills the window, or -1. */
int ios_exewin_parse( const char *env, struct ios_exewin *w );

/* Whether an image preferring base with this size lies inside the window. */
int ios_exewin_fits( const struct ios_exewin *w, uint64_t base, uint64_t size );

/* Claims the window for owner (0), or -1 when occupied, outside or the window doesn't exist. */
int ios_exewin_claim( struct ios_exewin *w, const void *owner, uint64_t base, uint64_t size,
                      uint64_t host_page );

/* Whether owner's claim is exactly this image (same host-page-rounded size). */
int ios_exewin_confirm( const struct ios_exewin *w, const void *owner, uint64_t base, uint64_t size,
                        uint64_t host_page );

/* Ends owner's claim: 1 (it was the occupant; *base, *size: what to put back), 0 otherwise. */
int ios_exewin_retire( struct ios_exewin *w, const void *owner, uint64_t *base, uint64_t *size );

/* ---------- Part C: dylib images ---------- */

/* The folder of a machine's dylibs: AMD64 (ARM64EC) from pe_dir, ARM64 from pe_dir_a64, else
 * (i386 and the rest) none. An empty variable counts as unset. */
const char *ios_dylib_dir( unsigned short machine, const char *pe_dir, const char *pe_dir_a64 );

/* "<dir>/lib<name>.dylib" with name the lower-case file name of the DLL path (Windows or unix
 * separators). Returns 0, or -1 if it doesn't fit or the name is empty. */
int ios_dylib_path( const char *dir, const char *dll_path, char *buf, size_t size );

/* Clarification 4 (spec 18 Part B1): the address of the export `name` of the PE image mapped at
 * image (size bytes, headers included), or NULL. Everything is bounds-checked; forwarders give
 * NULL. Used for ntdll's `ios_teb_tsd_offset`. */
void *ios_pe_find_export( const void *image, size_t size, const char *name );

/* ---------- Part B: executable requests ---------- */

enum ios_exec_place
{
    IOS_PLACE_OTHER,        /* anywhere else */
    IOS_PLACE_IMAGE_CODE,   /* a signed image's headers and code */
    IOS_PLACE_IMAGE_DATA,   /* a signed image's data */
    IOS_PLACE_GUEST,        /* inside a 32-bit guest window */
};

/* The unix protection to apply for a request (PROT_* bits: 1 read, 2 write, 4 exec), or -1 for
 * "change nothing, report success" (signed code). *strip is set when PROT_EXEC was dropped. */
int ios_vm_exec_prot( int place, int unix_prot, int *strip );

/* ---------- clarifications 9 and 9a: which images their signed dylib replaces ----------
 * 9a first: an image with native ARM code (machine 0xaa64, or is_hybrid: ARM64X, or an x64
 * image with ARM64EC metadata, which is what the server's is_hybrid says) can only run from its
 * dylib on iOS, so it takes the dylib whatever its marker or path (the CPU modules xtajit.dll
 * and xtajit64.dll carry no Wine marker). Then 9, for images without native ARM code:
 * wine_builtin_marker (the server found Wine's "Wine builtin DLL" signature) or
 * loaded_as_builtin (the unix loader loads it as a builtin from the bundle's farms); anything
 * else with a builtin's name (a game's own D3DCompiler_47.dll) is mapped from its own file.
 * Sizes never decide. Returns 1 when Part C's dylib may replace the image (the dylib lookup then
 * decides whether there is one). */
int ios_image_takes_dylib( int wine_builtin_marker, int loaded_as_builtin, unsigned short machine,
                           int is_hybrid );

/* ---------- clarification 8: a DLL image's 64-bit machine against its process ----------
 * main_machine: the process's main image (0x8664 x64, 0xaa64 plain ARM64, 0x014c WoW64, 0 not
 * known yet); image_machine and is_hybrid (ARM64X/CHPE metadata) from the image's information.
 * A plain ARM64 process takes only 0xaa64 images; an x64 process 0x8664 images (the ARM64EC
 * builds) and 0xaa64 images that are hybrid. Anything else (a WoW64 process, an unknown main
 * image, an image that isn't 64-bit) is left to upstream. Returns 1 when the image may be
 * mapped, 0 to refuse it with STATUS_INVALID_IMAGE_FORMAT. */
int ios_image_fits_process( unsigned short main_machine, unsigned short image_machine, int is_hybrid );

/* ---------- clarification 10: no second instance in the same process ----------
 * A DLL that takes its dylib is the same DLL as a loaded one when both give the same dylib path
 * (ios_dylib_path: the file name, case-insensitive, in the machine's directory), whatever the
 * directory or spelling it was asked by. A view found at one of that dylib's instances is the
 * calling process's loaded copy when the view starts at the instance's image, is a dylib image
 * view (SEC_IMAGE and VPROT_SYSTEM) and belongs to the calling process (owner keys are never 0).
 * Returns 1 when that view is handed back instead of mapping a new instance. */
int ios_dylib_view_reusable( int starts_at_image, int is_dylib_view, const void *view_owner,
                             const void *owner );

/* ---------- Part E4: what a 32-bit process's own window holds ----------
 * Only memory the guest asked for. A guest request reaches map_view with its range translated
 * into the window, [B + low, B + high]; a unix-side request with no guest limit (the shared data
 * page, kernel stacks, the unix side's own buffers) spans the whole address space and must not
 * land in the window: guest code would see it at a low address (the shared data page at B is
 * the program's NULL page) and could overwrite it. Returns whether a general allocation whose
 * requested range is [range_low, range_high) may be placed in the window [base, base + 4 GB). */
int ios_win_request_in_window( uint64_t base, uint64_t range_low, uint64_t range_high );

/* ---------- Part E4: modules named by guest address ---------- */

/* Whether a builtin registered at host address `registered` is the module a caller names as
 * `query`: the same address, or, for a 32-bit process (wow_base != 0), a guest address below
 * 4 GB whose host address in that process's window (wow_base + query) is the registered one.
 * WoW64 passes a 32-bit DLL's base as its guest address. */
int ios_module_match( uint64_t registered, uint64_t query, uint64_t wow_base );

/* ---------- clarification 7: shared image sections ---------- */

/* Whether a writable shared section [rva, rva + map_size) of an image may keep upstream's
 * shared mapping: only when it starts and ends on host pages, so it fills whole host pages of
 * its own (images have 4 KB section alignment, the host 16 KB pages). Otherwise it is mapped
 * like an ordinary writable section. */
int ios_shared_section_shareable( uint64_t rva, uint64_t map_size, uint64_t host_page );

/* ---------- clarification 1: low windows ---------- */

#define IOS_LOWWIN_SLOTS 64

/* Registers low base..low+size as an alias of real (owner NULL: every process). Lock-free
 * readers. Returns 0, or -1 when full. */
int ios_lowwin_add( uint64_t low, uint64_t size, uint64_t real, const void *owner );

/* Forgets every window of owner. */
void ios_lowwin_remove_owner( const void *owner );

/* The real address for addr: a window of peb or an owner-NULL one; with peb NULL any match.
 * 0 when none covers it. */
uint64_t ios_lowwin_lookup( uint64_t addr, const void *peb );

/* The same, returning the window's slot (or -1) and the real address in *real. */
int ios_lowwin_find( uint64_t addr, const void *peb, uint64_t *real );

/* Counts one access serviced through a window (spec 11 clarification 1); returns the count. */
unsigned int ios_lowwin_note_service( int slot );

/* Forgets everything (tests). */
void ios_lowwin_reset(void);

#endif /* MYIOSDECK_IOS_VM_PURE_H */
