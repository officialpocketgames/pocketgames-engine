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
 * IMAGE_FILE_LARGE_ADDRESS_AWARE when PG_LAA is "0". */
uint64_t ios_wow_ceiling( int large_address_aware, const char *env_laa );

/* Whether allocations try the low 2 GB first: an image without the flag that was given 4 GB,
 * unless PG_LAA_LOWFIRST is "0". */
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

/* ---------- clarification 12: the window may be several kernel regions ----------
 * The kernel splits a large reservation into entries of at most 128 MB, so the window the app
 * reserved with one call is checked as a walk: the first region at or below the base, each next
 * one starting exactly where the previous ended, each with current protection VM_PROT_NONE (0),
 * until they reach base + size. A failed query, a gap, another protection or more than
 * IOS_EXEWIN_MAX_REGIONS regions refuse the window. */

#define IOS_EXEWIN_MAX_REGIONS 64

/* Like vm_region_64: the region holding *addr, else the next one above it; on success (0)
 * *addr is its start, *size its size, *protection its current protection. Nonzero: the
 * kern_return_t (no region at or above *addr, for example). */
typedef int (*ios_region_query)( void *ctx, uint64_t *addr, uint64_t *size, int *protection );

enum ios_exewin_walk_result
{
    IOS_EXEWIN_WALK_OK,
    IOS_EXEWIN_WALK_QUERY_FAILED,   /* the query at `at` failed with kr */
    IOS_EXEWIN_WALK_GAP,            /* nothing at `at`: the next region is [next, next_end) */
    IOS_EXEWIN_WALK_PROTECTION,     /* region [next, next_end) isn't inaccessible */
    IOS_EXEWIN_WALK_TOO_MANY,       /* more than IOS_EXEWIN_MAX_REGIONS regions */
};

struct ios_exewin_walk
{
    int       result;       /* enum ios_exewin_walk_result */
    int       kr;           /* the failed query's kern_return_t, else 0 */
    unsigned  regions;      /* adjacent inaccessible regions found from the base */
    uint64_t  start, end;   /* what they cover (start = end = base: none) */
    int       protection;   /* the last region reported's protection; -1 when the last query failed */
    uint64_t  at;           /* where the walk stopped: the address asked last */
    uint64_t  next, next_end;   /* the last region reported (GAP, PROTECTION) */
};

/* Walks [base, base + size). Returns 0 when the window is accepted, -1 when refused; *walk
 * says how far the walk got either way. */
int ios_exewin_walk( uint64_t base, uint64_t size, ios_region_query query, void *ctx,
                     struct ios_exewin_walk *walk );

/* The boot line: "exe window check: kr 0, <n> regions 0x<start>-0x<end>, protection 0", and
 * for a refused window "; refused: <why>" after it. */
void ios_exewin_walk_describe( const struct ios_exewin_walk *walk, char *buf, size_t size );

/* ---------- Part C: dylib images ---------- */

/* The folder of a machine's dylibs: AMD64 (ARM64EC) from pe_dir, ARM64 from pe_dir_a64, else
 * (i386 and the rest) none. An empty variable counts as unset. */
const char *ios_dylib_dir( unsigned short machine, const char *pe_dir, const char *pe_dir_a64 );

/* "<dir>/lib<name>.dylib" with name the lower-case file name of the DLL path (Windows or unix
 * separators). Returns 0, or -1 if it doesn't fit or the name is empty. */
int ios_dylib_path( const char *dir, const char *dll_path, char *buf, size_t size );

/* Clarification 17: whether an image starts on Windows' 64 KB allocation granularity (a
 * program finding a DLL's base by stepping down 64 KB at a time to `MZ` relies on it) */
int ios_image_granular( const void *image );

/* Clarification 17: which image a mapping of a dylib's DLL registers. first_has_view: dyld's
 * mapping already has a view (C4: another process maps the DLL); first_no_own_view: dyld's
 * mapping isn't 64 KB aligned, so it never gets a view (set at first use). Returns
 * IOS_DYLIB_TAKE_FIRST (register dyld's mapping) or IOS_DYLIB_TAKE_INSTANCE (a fresh instance
 * from the app, which places it 64 KB aligned). */
#define IOS_DYLIB_TAKE_FIRST    0
#define IOS_DYLIB_TAKE_INSTANCE 1
int ios_dylib_take( int first_has_view, int first_no_own_view );

/* Clarification 18: whether a 64-bit main image whose preferred base lies in Part D's window
 * takes the window. characteristics: the file header's; dll_characteristics: the optional
 * header's; reloc_dir_size: the size of the relocation data directory (0: none). Returns 0 for
 * an image Windows would move anyway (relocations not stripped, a relocation directory, and both
 * DYNAMIC_BASE and HIGH_ENTROPY_VA): it goes the normal path away from its preferred base and the
 * window stays free. Returns 1 for every other image. */
int ios_image_wants_window( unsigned short characteristics, unsigned short dll_characteristics,
                            uint32_t reloc_dir_size );

/* Clarification 18: the size of the relocation data directory of the PE headers in hdr (len
 * bytes, from the start of the file). 0 when the headers are cut short or malformed, when the
 * optional header has no relocation entry, or when the entry is empty. Everything is
 * bounds-checked. */
uint32_t ios_pe_reloc_dir_size( const void *hdr, size_t len );

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
 * builds) and 0xaa64 images that are hybrid. A WoW64 process refuses 0x8664 images that are
 * hybrid (ARM64EC, clarification 11) and leaves the rest to upstream. Anything else (an unknown
 * main image, an image that isn't 64-bit) is left to upstream. Returns 1 when the image may be
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

/* ---------- clarification 13: allocation lines ----------
 * Which line a NtAllocateVirtualMemory(Ex) call of the process's own gets: FAILED for any
 * failure (status not 0); FIXED for MEM_RESERVE (0x2000) at a non-zero base, whatever the
 * result; NONE otherwise (a successful commit inside a reservation, a reservation anywhere). */
#define IOS_ALLOC_LOG_NONE   0
#define IOS_ALLOC_LOG_FAILED 1
#define IOS_ALLOC_LOG_FIXED  2
int ios_alloc_log_kind( uint32_t type, int base_given, uint32_t status );

/* Clarification 14 B: one allocation call as the log sees it (what the caller asked for; the
 * range only for the Ex call's address requirements) */
struct ios_alloc_event
{
    uint64_t base, size;
    uint32_t type, prot, status;
    uint64_t zero_bits;
    int      has_range;
    uint64_t range_low, range_high;
    uint64_t got, got_size;      /* what the caller has after the call */
};

/* A process's allocation log: the run of failures not written yet, and the lines written. A run
 * is consecutive failures with the same size, type, protection and status whose bases step by
 * the same amount (count 1: just `first`). Lines are counted when written: 512, then the 513th
 * is the suppression line, then nothing. The caller serialises calls on one log. */
#define IOS_ALLOC_LOG_CAP      512
#define IOS_ALLOC_RUN_MAX      4096
struct ios_alloc_log
{
    unsigned int           lines;
    unsigned int           count;   /* events in the run, 0: none */
    struct ios_alloc_event first;
    uint64_t               last;    /* the run's last base */
    int64_t                step;    /* base step, known once count >= 2 */
};

/* writes one line (no newline) */
typedef void (*ios_alloc_line_fn)( void *ctx, const char *line );

/* One allocation call of the process's own, of the kind ios_alloc_log_kind gave (every call,
 * NONE included: any call that doesn't continue the run ends it). A failure joins or starts the
 * run (written at IOS_ALLOC_RUN_MAX events); FIXED writes the run, then its `alloc at` line. */
void ios_alloc_log_event( struct ios_alloc_log *log, int kind, const struct ios_alloc_event *ev,
                          ios_alloc_line_fn out, void *ctx );

/* Writes the pending run, if any: clarification 13's line for a run of one, else
 * `alloc failed x<n>: bases <first>..<last> step <signed step> size .. type .. prot .. status ..` */
void ios_alloc_log_flush( struct ios_alloc_log *log, ios_alloc_line_fn out, void *ctx );

/* ---------- clarification 14 A: memory outside Wine's views ----------
 * For a 64-bit process, the answer to a query at base (page-aligned) that no view contains;
 * gap_end is where upstream's free answer ends (the next view, or the address-space limit).
 * wine_reserved: base lies in a range Wine reserved itself (its reserved areas, the exe window):
 * upstream's answer. Otherwise the host's region (vm_region_64 at base: the region holding it or
 * the next one above; host_found 0 when there is none):
 * - a host region holds base: reserved, from its start rounded down to 64 KB to its end rounded
 *   up, clipped to gap_end;
 * - else free up to the next host region's start rounded down to 64 KB, or gap_end if lower;
 *   when that leaves less than one 64 KB block from base rounded down, that block is in use:
 *   reserved from base rounded down to the host region's end rounded up (or to gap_end without
 *   one), clipped to gap_end.
 * Returns IOS_FREE_UPSTREAM, IOS_FREE_FREE or IOS_FREE_RESERVED, with alloc_base (reserved only)
 * and region_end. */
#define IOS_FREE_UPSTREAM 0
#define IOS_FREE_FREE     1
#define IOS_FREE_RESERVED 2
int ios_free_region_answer( uint64_t base, uint64_t gap_end, int wine_reserved, int host_found,
                            uint64_t host_start, uint64_t host_end, uint64_t *alloc_base,
                            uint64_t *region_end );

/* ---------- clarification 15: large memory events ----------
 * Whether a successful call of the process's own gets a `mem:` line. op: IOS_MEM_ALLOC (a
 * MEM_RESERVE with a base of 0), IOS_MEM_FREE (a MEM_RELEASE), IOS_MEM_MAP (a view of a section
 * that isn't an image), IOS_MEM_IMAGE (an image view mapped from its file), IOS_MEM_UNMAP (any
 * view); size is the range the call gives back. Images always, the others from 1 MB. */
#define IOS_MEM_ALLOC 1
#define IOS_MEM_FREE  2
#define IOS_MEM_MAP   3
#define IOS_MEM_IMAGE 4
#define IOS_MEM_UNMAP 5
#define IOS_MEM_EVENT_MIN 0x100000
int ios_mem_event_wanted( int op, uint64_t size );

/* The per-process line counter (zero at the start): IOS_MEM_LINE_WRITE for the first 256 lines,
 * IOS_MEM_LINE_SUPPRESS once (write `mem: further lines suppressed` instead), then
 * IOS_MEM_LINE_NONE. The caller serialises calls on one counter. */
#define IOS_MEM_EVENT_CAP     256
#define IOS_MEM_LINE_NONE     0
#define IOS_MEM_LINE_WRITE    1
#define IOS_MEM_LINE_SUPPRESS 2
int ios_mem_event_take( unsigned int *lines );

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

/* ---------- clarification 16: the address an image view's caller asked for ----------
 * ios_image_asked_base: the address NtMapViewOfSection of an image tries first, the caller's
 * rounded down to 64 KB (0: none asked). ios_image_asked_note: what the `mem: image` line adds for
 * a view not at its preferred base (", asked any" or ", asked 0x...", then ", limit 0x..." when
 * the call limited the address); an empty string for a view at its preferred base. Returns buf. */
uint64_t ios_image_asked_base( uint64_t asked );
char *ios_image_asked_note( char *buf, size_t len, uint64_t asked, uint64_t limit, uint64_t got,
                            uint64_t preferred );

#endif /* MYIOSDECK_IOS_VM_PURE_H */
