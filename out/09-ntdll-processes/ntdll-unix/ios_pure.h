// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09: the pieces of the iOS process model that are pure functions or plain data structures.
// They use only the C library (no Wine headers), so tests/test_ios_pure.c can run them on a
// host. Everything that touches Wine's state is in ios_process.c.

#ifndef MYIOSDECK_IOS_PURE_H
#define MYIOSDECK_IOS_PURE_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* ---------- Part A: unix libraries by name ---------- */

/* Builds the name of the table symbol for a DLL: "X.dll" -> "x_unix_call_funcs", or
 * "x_unix_call_wow64_funcs" when wow64 is non-zero. Any directory part and the extension are
 * dropped, letters are lower-cased and characters that can't appear in a C identifier become '_'.
 * "winemetal" maps to "dxmt_winemetal" (spec 05). Returns 0, or -1 if the name is empty or the
 * result doesn't fit in size bytes. */
int ios_unixlib_symbol( const char *dll_name, int wow64, char *buf, size_t size );

/* The same for the library's optional init entry: "x_unix_lib_init". */
int ios_unixlib_init_symbol( const char *dll_name, char *buf, size_t size );

/* ---------- Part C5 / C6: names of child programs ---------- */

/* The file name part of a Windows or unix path (after the last '\\' or '/'). */
const char *ios_file_part( const char *path );

/* 1 if the file name of path contains "crash", "crs-handler", "handler", "report" or "helper",
 * ignoring case: a helper, not counted as a live game child. */
int ios_is_helper_name( const char *path );

/* The refused-spawn token that path contains (ignoring case), or NULL. */
const char *ios_refused_spawn_match( const char *path );

/* 1 if the file name of path is conhost.exe (ignoring case). */
int ios_is_conhost( const char *path );

/* ---------- Part B2: the per-PEB registry ---------- */

#define IOS_REGISTRY_SLOTS 64

enum ios_slot_state { IOS_SLOT_FREE = 0, IOS_SLOT_BUSY = 1, IOS_SLOT_LIVE = 2 };

struct ios_registry_slot
{
    _Atomic int    state;   /* enum ios_slot_state */
    void *_Atomic  key;     /* the PEB */
    void *_Atomic  value;   /* the pseudo-process's state */
};

struct ios_registry
{
    struct ios_registry_slot slot[IOS_REGISTRY_SLOTS];
};

/* Publishes key -> value in a free slot. The slot is written completely before it becomes
 * visible. Returns 0, or -1 if key is NULL, the key is already published or no slot is free. */
int ios_registry_publish( struct ios_registry *reg, void *key, void *value );

/* Lock-free (safe in fault handlers): the value published for key, or NULL. */
void *ios_registry_lookup( struct ios_registry *reg, const void *key );

/* Removes key's slot so it can be reused. Returns 0, or -1 if the key isn't published. */
int ios_registry_remove( struct ios_registry *reg, const void *key );

/* Number of live slots (diagnostics, tests). */
int ios_registry_count( struct ios_registry *reg );

/* ---------- Part C5: children that keep the session alive ---------- */

#define IOS_MAX_CHILDREN 64
#define IOS_CHILD_NAME_MAX 64

/* Starts tracking a child. now is a monotonic time in seconds. Returns a non-zero id, or 0 if
 * the table is full (the child then isn't counted). */
unsigned int ios_children_add( const char *path, double now );

/* The child became a Wine system process (services.exe and the like): no longer counted. */
void ios_children_set_system( unsigned int id );

/* The child ended. */
void ios_children_remove( unsigned int id );

/* How many tracked children are alive, not helpers, not system processes, and started at most
 * max_age seconds before now (max_age < 0: any age). Their names go to buf, comma-separated
 * and NUL-terminated, truncated to len bytes (nothing is written when buf is NULL or len <= 0). */
int ios_children_live( char *buf, int len, double max_age, double now );

/* Forget every child (tests). */
void ios_children_reset(void);

/* ---------- Part F2: hardware identity ---------- */

/* A plausible desktop clock in MHz derived from the counter frequency CNTFRQ_EL0 (in Hz):
 * the counter's MHz doubled until it is at least 2000 (24 MHz -> 3072). 0 Hz gives 3000. */
unsigned int ios_plausible_mhz( uint64_t counter_hz );

/* ---------- Part B: which farm a process loads its images from ----------
 * The bundle has separate farms (spec 19 B4/B6), not upstream's ARM64X images:
 * "/arm64ec-windows" (ARM64EC, every 64-bit image of an x64 program), "/aarch64-windows" (plain
 * ARM64: Wine's own programs and the 64-bit half of a 32-bit program) and "/i386-windows".
 * main_machine is the process's main image's machine (0 before it is known), image_machine the
 * machine of the image being looked for (upstream passes the host's, ARM64, for ntdll). */
#define IOS_MACHINE_I386   0x014c
#define IOS_MACHINE_ARMNT  0x01c4
#define IOS_MACHINE_AMD64  0x8664
#define IOS_MACHINE_ARM64  0xaa64
const char *ios_pe_farm_dir( unsigned short main_machine, unsigned short image_machine );

/* ---------- Part C4: the name a child is known by ----------
 * The NT name NtCreateUserProcess resolved, when it has a buffer. A builtin with no file in the
 * prefix yet (wineboot.exe at the first boot) is started anyway and has none: its NT name's
 * buffer is NULL and its length undefined, never to be read. Then the image path the caller
 * gave. Lengths in bytes; returns NULL (*len_ret 0) when neither has a buffer. */
const void *ios_child_image_name( const void *nt_buffer, unsigned int nt_length,
                                  const void *path_buffer, unsigned int path_length,
                                  unsigned int *len_ret );

/* ---------- Parts A and C2: how a failing or ending thread leaves ---------- */

enum ios_exit_route
{
    IOS_EXIT_APP_JMP,   /* longjmp to the app's wine_ios_exit_jmpbuf (contract Section B) */
    IOS_EXIT_OWN_JMP,   /* longjmp to __wine_main's own buffer: __wine_main returns */
    IOS_EXIT_THREAD,    /* pthread_exit: a thread the app didn't hand to __wine_main */
};

/* app_ready: the app's buffer is set for the calling thread (wine_ios_exit_initialized == 1 and
 * it is wine_ios_main_thread); own_ready: the calling thread is inside __wine_main, whose own
 * buffer is set. The thread that called __wine_main never takes IOS_EXIT_THREAD, which would
 * end an app thread (the main thread: the whole app) without a trace. */
int ios_exit_route( int app_ready, int own_ready );

/* ---------- once-per-app set-up that may end by jumping out ----------
 * pthread_once can't be left by longjmp (its control stays "running" and the next caller
 * waits forever). These states don't block: a set-up left half done is reported as failed. */

enum ios_init_state { IOS_INIT_NONE, IOS_INIT_RUNNING, IOS_INIT_DONE };
enum ios_init_step  { IOS_INIT_RUN, IOS_INIT_SKIP, IOS_INIT_FAILED_BEFORE };

/* NONE -> RUNNING: run it (then call ios_init_end); DONE: skip; RUNNING: an earlier run
 * didn't finish (callers are one at a time: the session's __wine_main, then its children). */
int ios_init_begin( int *state );
void ios_init_end( int *state );

#endif /* MYIOSDECK_IOS_PURE_H */
