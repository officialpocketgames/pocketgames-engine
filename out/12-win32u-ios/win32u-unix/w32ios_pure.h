// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Pure logic of the iOS win32u (spec 12): no Wine headers, no system calls, so the host tests in
// ../tests can compile it on any machine. Everything that touches Wine or the app calls into
// these functions with plain values.

#ifndef W32IOS_PURE_H
#define W32IOS_PURE_H

#include <stddef.h>
#include <stdint.h>

/* ===================== Part B: one win32u for every pseudo-process ===================== */

/* The per pseudo-process table. A slot is keyed by (PEB, process id). Readers never lock:
 * they read a slot's sequence number, its state and key, and the sequence number again, and
 * trust the slot only if nothing changed (a seqlock). Writers are serialised by the caller (one
 * mutex), write the whole slot, and publish it with a release store. */
#define W32IOS_PP_SLOTS 64

enum { W32IOS_SLOT_FREE = 0, W32IOS_SLOT_LIVE = 1 };

struct w32ios_pp_slot
{
    uint32_t seq;     /* odd while a writer changes the slot */
    uint32_t state;   /* W32IOS_SLOT_* */
    void    *peb;
    uint32_t pid;
    uint32_t stamp;   /* when it was published, for choosing a victim */
};

struct w32ios_pp_table
{
    struct w32ios_pp_slot slot[W32IOS_PP_SLOTS];
    uint32_t clock;
};

/* Lock-free lookup: the index of the live slot for (peb, pid), or -1. */
int w32ios_pp_find( const struct w32ios_pp_table *table, const void *peb, uint32_t pid );

/* Writer side (caller holds the table's mutex). Chooses the slot for (peb, pid): the live slot
 * already holding that key, else a free slot, else a live slot with the same PEB and another
 * process id (that process is dead: two live processes can't share a PEB address), else a live
 * slot for which is_dead() says its process ended. Returns -1 when every slot holds a live
 * process. *reuse is set to 1 when the slot held the same key already. */
int w32ios_pp_pick( const struct w32ios_pp_table *table, const void *peb, uint32_t pid,
                    int (*is_dead)( void *peb, uint32_t pid, void *ctx ), void *ctx, int *reuse );

/* Writer side: mark a slot as being rewritten (readers skip it), then publish it with its key. */
void w32ios_pp_begin( struct w32ios_pp_table *table, int index );
void w32ios_pp_publish( struct w32ios_pp_table *table, int index, void *peb, uint32_t pid );

/* C2 / clarification 6: a new session starts with a process that has no parent (its
 * ProcessBasicInformation.InheritedFromUniqueProcessId is 0). queried = 0 when that query failed:
 * then it is not taken for a new session. */
int w32ios_is_session_root( int queried, uint64_t parent_pid );

/* B4 / clarification 11: whether a thread that found no desktop window should register the
 * desktop classes (and connect) for its process: B4 is on (PG_CHILD_DESKTOP), init_user has run
 * in some process, the caller's own slot isn't marked as the one that ran it, and its process
 * hasn't tried yet. */
int w32ios_child_desktop_due( int enabled, int initialized, int slot_ran_init_user, int slot_tried );

/* B1: the allocation ceiling of a thread. A WoW64 thread gets the guest ceiling OR 0x7fffffff;
 * every other thread 0. */
uint64_t w32ios_ceiling( int wow64, uint64_t highest_user_address );

/* B2: user32's built-in window procedures for one process. Entry i is the procedure pair (ANSI,
 * Unicode) whose handle is WINPROC_HANDLE << 16 | i; a pair has the layout of class.c's
 * WINDOWPROC (two pointers). */
typedef void *w32ios_proc_pair[2];
#define W32IOS_MAX_CLIENT_PROCS 64

struct w32ios_client_procs
{
    int              set;     /* filled by NtUserInitializeClientPfnArrays */
    w32ios_proc_pair procs[W32IOS_MAX_CLIENT_PROCS];
};

/* The table that answers for a caller: its own when it was filled, else the global fallback. */
w32ios_proc_pair *w32ios_procs_table( struct w32ios_client_procs *own, w32ios_proc_pair *global );
/* The index of a built-in procedure equal to func (either flavour), or -1. count = how many
 * built-ins exist (NTUSER_NB_PROCS). */
int w32ios_procs_match( const void *table, int count, const void *func );
/* The index of an entry of table (count entries), or -1 if entry isn't inside it. */
int w32ios_procs_index( const void *table, int count, const void *entry );

/* B3: what a thread does when it needs the built-in classes of its process.
 * done: the process registered them; owner: thread id registering them now (0 if none). */
enum w32ios_builtin_action { W32IOS_BUILTIN_SKIP, W32IOS_BUILTIN_REGISTER, W32IOS_BUILTIN_WAIT };
enum w32ios_builtin_action w32ios_builtin_action( int done, uint32_t owner, uint32_t self );

/* B7: a window procedure value as WoW64 thunks hand it over. A 32-bit handle 0xffffNNNN that a
 * thunk converted as a guest address arrives as wow_base + 0xffffNNNN. Returns the handle in that
 * case and the value unchanged otherwise (also when disabled or for a 64-bit caller). */
uint64_t w32ios_winproc_handle( uint64_t value, uint64_t wow_base, int enabled );

/* B8: the walk of the class list stops after this many entries. */
#define W32IOS_CLASS_WALK_MAX 8192

/* ===================== Part C: the virtual monitor ===================== */

struct w32ios_size { int w, h; };

#define W32IOS_STD_MODE_COUNT 37
extern const struct w32ios_size w32ios_std_modes[W32IOS_STD_MODE_COUNT];

/* The session default from the environment values (strings or NULL): positive width and height,
 * else 1024 x 768. */
struct w32ios_size w32ios_session_default( const char *w, const char *h );

/* Spec 12 clarification 10: what a top-level program (no parent) does to the monitor when it
 * starts. first: no program has set a size yet in this app run. want: its launch's size
 * (w32ios_session_default of its environment). cur: the current size. others: another user
 * program of the session is still running. */
enum w32ios_launch_size
{
    W32IOS_LAUNCH_FIRST,    /* the first program: default and current become want */
    W32IOS_LAUNCH_SAME,     /* want is the current size: the default becomes want, nothing else */
    W32IOS_LAUNCH_CHANGE,   /* default and current become want, the server and caches follow */
    W32IOS_LAUNCH_STAYS,    /* another program runs: nothing changes */
};
enum w32ios_launch_size w32ios_launch_size( int first, struct w32ios_size cur, struct w32ios_size want,
                                            int others );

/* Whether a running process counts as another user program for clarification 10. path: its
 * image's Windows path (len characters, 0 when it couldn't be read); sysroot: the Windows
 * directory (KUSER_SHARED_DATA.NtSystemRoot, "C:\windows"); name: its file name. Not counted:
 * images inside the Windows directory (services.exe, explorer.exe, winedevice.exe, ...) and
 * helpers by name (ignoring case, the file name contains "crash", "handler", "report" or
 * "helper", as spec 09's live-children count). An unreadable path counts unless the name is a
 * helper's. */
int w32ios_is_user_program( const uint16_t *path, size_t len, const uint16_t *sysroot, size_t root_len,
                            const uint16_t *name, size_t name_len );

/* EnumDisplaySettings' list: [0] the current size, then the standard modes in order whose pixel
 * count is at most the default's (four times with extended), the current size never twice.
 * Returns the count written (at most max). */
int w32ios_build_modes( struct w32ios_size cur, struct w32ios_size def, int extended,
                        struct w32ios_size *out, int max );

enum w32ios_cds { W32IOS_CDS_RESTORE, W32IOS_CDS_ACCEPT, W32IOS_CDS_BADMODE };
/* ChangeDisplaySettingsEx's decision for our device. has_w/has_h: DM_PELSWIDTH/DM_PELSHEIGHT in
 * dmFields (a missing one of the two keeps the current value); none (or no devmode): restore.
 * *out receives the size to apply for ACCEPT and RESTORE. */
enum w32ios_cds w32ios_mode_decision( int has_devmode, int has_w, int has_h, int w, int h,
                                      struct w32ios_size cur, struct w32ios_size def, int extended,
                                      struct w32ios_size *out );

/* Device names: 0 unknown, 1 the adapter \\.\DISPLAY1 (also NULL or empty), 2 its monitor
 * \\.\DISPLAY1\Monitor0. Case-insensitive; len in UTF-16 units (the string needs no NUL). */
enum { W32IOS_DEV_NONE = 0, W32IOS_DEV_ADAPTER = 1, W32IOS_DEV_MONITOR = 2 };
int w32ios_match_device( const uint16_t *name, size_t len );

/* C6: the driver version string per PCI vendor, and its QWORD a<<48 | b<<32 | c<<16 | d. */
const char *w32ios_driver_version( uint16_t vendor );
uint64_t w32ios_pack_version( const char *version );

/* ===================== Part D: the D3DKMT adapter ===================== */

/* D1: the adapter LUID as a 64-bit value (LowPart = low 32 bits): bswap64(registryID) */
uint64_t w32ios_luid_from_registry_id( uint64_t id );

/* ===================== Part E: the iOS user driver ===================== */

/* E3: cursor images to straight-alpha BGRA (w * h * 4 bytes out). Rows are top-down.
 * color: 32-bit BGRA pixels; and_mask: 1 bit per pixel, rows padded to 32 bits. */
int  w32ios_cursor_has_alpha( const uint8_t *color, int w, int h );
void w32ios_cursor_color( const uint8_t *color, const uint8_t *and_mask, int w, int h, uint8_t *out );
/* Monochrome: mask holds 2h rows, the AND mask above the XOR mask. */
void w32ios_cursor_mono( const uint8_t *mask, int w, int h, uint8_t *out );
#define W32IOS_CURSOR_MAX 256

/* E1: KEYEVENTF_EXTENDEDKEY for a key: its MAPVK_VK_TO_VSC_EX scan code has the 0xe0 or 0xe1
 * prefix, or (nav_e0) it is a dedicated navigation key. */
int w32ios_key_extended( unsigned int vk, unsigned int scan_ex, int nav_e0 );
/* E1: log the first few events and every 256th. */
int w32ios_log_event( unsigned int count );

/* Spec 15 clarification 9b (raw input diagnostics): the first few events of a kind are logged in
 * full, then one count per 5 s window while they keep coming. w32ios_diag_event counts one event
 * at now_ms and returns its number (1-based); w32ios_diag_due returns the window's count when it
 * has lasted W32IOS_DIAG_WINDOW_MS and had events (and starts the next window), else 0. */
struct w32ios_diag
{
    unsigned int total;
    unsigned int window;
    uint64_t window_start;
};
#define W32IOS_DIAG_WINDOW_MS 5000
unsigned int w32ios_diag_event( struct w32ios_diag *d, uint64_t now_ms );
unsigned int w32ios_diag_due( struct w32ios_diag *d, uint64_t now_ms );
/* The XInput slot (0-3) whose HID device spec 15 clarification 8 serves under raw input handle
 * 0x101 + slot with VID/PID 045E:028E, or -1 for any other device. */
int w32ios_rawinput_slot( unsigned int handle, unsigned int vendor, unsigned int product );

/* E2: the deadline of the next slice of a message wait. Times in 100 ns units. has_deadline = 0
 * for a wait without timeout. *final is 1 when the slice ends at the caller's own deadline, so a
 * timeout there is the caller's timeout. */
int64_t w32ios_slice_deadline( int64_t now, int has_deadline, int64_t deadline, int64_t slice,
                               int *final );
#define W32IOS_SLICE (16 * 10000)   /* 16 ms */

/* E2: skip the set_queue_mask request of a pure poll (nothing delivered, no wake or changed mask)
 * unless a second passed since this thread's last one. */
int w32ios_poll_skip_request( int pure, uint32_t now_ms, uint32_t last_ms );

/* E4/E5: rectangle tests (left, top, right, bottom). */
int w32ios_covers_screen( int l, int t, int r, int b, int screen_w, int screen_h );
int w32ios_dialog_candidate( int l, int t, int r, int b, int screen_w, int screen_h );

/* ===================== Part F: XInput ===================== */

struct w32ios_xinput_gamepad
{
    uint16_t wButtons;
    uint8_t  bLeftTrigger;
    uint8_t  bRightTrigger;
    int16_t  sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};
struct w32ios_xinput_state
{
    uint32_t dwPacketNumber;
    struct w32ios_xinput_gamepad Gamepad;
};
struct w32ios_xinput_vibration
{
    uint16_t wLeftMotorSpeed;
    uint16_t wRightMotorSpeed;
};
struct w32ios_xinput_capabilities
{
    uint8_t  Type;
    uint8_t  SubType;
    uint16_t Flags;
    struct w32ios_xinput_gamepad   Gamepad;
    struct w32ios_xinput_vibration Vibration;
};

_Static_assert( sizeof(struct w32ios_xinput_gamepad) == 12, "XINPUT_GAMEPAD is 12 bytes" );
_Static_assert( sizeof(struct w32ios_xinput_state) == 16, "XINPUT_STATE is 16 bytes" );
_Static_assert( sizeof(struct w32ios_xinput_vibration) == 4, "XINPUT_VIBRATION is 4 bytes" );
_Static_assert( sizeof(struct w32ios_xinput_capabilities) == 20, "XINPUT_CAPABILITIES is 20 bytes" );

/* The operations of NtUserCallTwoParam_GetGamepadState (arg1 bits 8-15). */
enum { W32IOS_PAD_STATE = 0, W32IOS_PAD_CAPS = 1, W32IOS_PAD_VIBRATION = 2 };

/* The contract's snapshot, read through these so this file needs no Wine types. */
struct w32ios_pad_source
{
    int  (*get_state)( int index, void *pgios_gamepad );   /* pgios_gamepad_get_state */
    void (*set_vibration)( int index, uint16_t left, uint16_t right );
    int  (*rumble_caps)( void );
};
/* The whole answer: arg1 = index | op << 8, buffer = the caller's structure. Returns 1 or 0. */
uint32_t w32ios_gamepad_answer( const struct w32ios_pad_source *src, uint32_t arg1, void *buffer );

/* ===================== spec 22 Part E: client objects in one shared table ===================== */

/* Whether the client object in a user handle slot counts for the caller: the slot's object was
 * put there by the calling pseudo-process (owner_pid, 0 for none), and, for a window
 * (is_window), the object's own full handle equals the window's full handle. Window enumeration
 * and message dispatch skip every other object (an ended process's may still sit in the slot). */
int w32ios_client_object_ok( uint32_t owner_pid, uint32_t caller_pid, int is_window,
                             uint32_t object_handle, uint32_t full_handle );

/* ===================== small helpers ===================== */

/* "1", "on", "true", "yes" (any case) are true; "0", "off", "false", "no" false; others def. */
int w32ios_parse_bool( const char *value, int def );

#endif /* W32IOS_PURE_H */
