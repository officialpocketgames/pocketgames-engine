// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Parts F and G: the rules of win32u's smaller changes as plain functions (no Wine
// headers), so the host tests can check them: the 32-bit session decision (F2), publishing the
// GDI handle table (F4), the owners of DC attribute buckets and cache DCEs (F5, F6), and the
// GA_ROOT walk when the parent list can't be read (G2).

#ifndef W32IOS_S17_PURE_H
#define W32IOS_S17_PURE_H

#include <stddef.h>
#include <stdint.h>

/* ---- F2: is this a 32-bit session? ---- */

/* PG_GDI_SHARED_SECTION set and not "0": yes; "0": no; otherwise (unset or empty) the
 * session's arming (spec 10 clarification 3; armed < 0: the function isn't linked, no) */
int w32ios_s17_session_32bit( const char *switch_value, int armed );

/* ---- F4: publishing the handle table in the caller's PEB ---- */

/* the thread's marker says which PEB it last published for; anything else needs a publish */
static inline int w32ios_s17_publish_needed( const void *marker, const void *peb )
{
    return marker != peb;
}

enum w32ios_s17_publish
{
    W32IOS_S17_MASTER,   /* the master view's address */
    W32IOS_S17_VIEW,     /* a second view inside the process's guest window */
};

/* a 64-bit pseudo-process gets the master; a 32-bit one a view, in a 32-bit session */
enum w32ios_s17_publish w32ios_s17_publish_kind( int process_is_32bit, int session_32bit );

/* the views of 32-bit processes, remembered by (process id, PEB); the oldest is overwritten */
#define W32IOS_S17_VIEWS 8

struct w32ios_s17_view
{
    uint32_t    pid;
    const void *peb;
    void       *view;      /* NULL: no view could be mapped (the master was published) */
    uint64_t    stamp;     /* 0: empty slot */
};

struct w32ios_s17_views
{
    struct w32ios_s17_view slot[W32IOS_S17_VIEWS];
    uint64_t               clock;
};

/* index of (pid, peb), or -1 */
int w32ios_s17_views_find( const struct w32ios_s17_views *views, uint32_t pid, const void *peb );
/* remembers (pid, peb) → view in an empty slot or over the oldest; returns the slot */
int w32ios_s17_views_put( struct w32ios_s17_views *views, uint32_t pid, const void *peb, void *view );

/* ---- F5, F6: who may use a DC attribute bucket or a cache DCE ---- */

struct w32ios_s17_owner
{
    uint32_t    pid;
    const void *peb;
    int         is_32bit;
};

/* both 64-bit: shared as upstream; otherwise only the same (process id, PEB) */
int w32ios_s17_owner_ok( const struct w32ios_s17_owner *caller, const struct w32ios_s17_owner *owner );

/* ---- G2: GA_ROOT when the parent list can't be read ---- */

/* walks up from hwnd with get_parent while the current window has WS_CHILD; returns the window
 * where the walk stops (at most max_steps steps, a guard against a cycle). get_parent returns 0
 * at the top; is_child says whether a window has WS_CHILD. */
typedef uint64_t (*w32ios_s17_parent_fn)( void *ctx, uint64_t hwnd );
typedef int (*w32ios_s17_is_child_fn)( void *ctx, uint64_t hwnd );
uint64_t w32ios_s17_root_walk( uint64_t hwnd, w32ios_s17_parent_fn get_parent,
                               w32ios_s17_is_child_fn is_child, void *ctx, unsigned int max_steps );

#endif /* W32IOS_S17_PURE_H */
