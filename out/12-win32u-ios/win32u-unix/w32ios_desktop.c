// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part B4/B5: get_desktop_window (winstation.c calls this instead of its own body).
//
//  - The server is asked with force set, as a service process asks, so it never waits for an
//    explorer.exe; none is ever started. Without a desktop window the answer is 0 and the next
//    call asks again.
//  - Only the session's first process ran init_user (window station, desktop, desktop and
//    message window classes). A thread of another process that gets no desktop window: once per
//    process, register the desktop and message classes for its process and ask again; if the
//    thread has no desktop at all, connect it the way winstation_init connects the session
//    (WinSta0 and the default desktop, unless its process parameters name others) and ask once
//    more. PG_CHILD_DESKTOP=0 turns this off.

#include "ntstatus.h"
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/server.h"
#include "wine/debug.h"
#include "w32ios.h"

static BOOL query_desktop_window( struct ntuser_thread_info *thread_info )
{
    SERVER_START_REQ( get_desktop_window )
    {
        req->force = 1;
        if (!wine_server_call( req ))
        {
            thread_info->top_window = reply->top_window;
            thread_info->msg_window = reply->msg_window;
        }
    }
    SERVER_END_REQ;
    return thread_info->top_window != 0;
}

static BOOL child_desktop_enabled( void )
{
    static int enabled = -1;
    if (enabled < 0) enabled = w32ios_env_bool( "PG_CHILD_DESKTOP", TRUE );
    return enabled;
}

/* the session's first process isn't the caller's, and the caller's process hasn't tried yet */
static BOOL try_child_desktop( void )
{
    void *session = w32ios_session_peb();
    struct w32ios_proc *proc;

    if (!child_desktop_enabled() || !session || session == ios_current_peb()) return FALSE;
    proc = w32ios_proc_current();
    return !InterlockedExchange( &proc->child_desktop_tried, 1 );
}

HWND w32ios_get_desktop_window( void )
{
    struct ntuser_thread_info *thread_info = NtUserGetThreadInfo();
    BOOL found;

    if (thread_info->top_window) return UlongToHandle( thread_info->top_window );

    found = query_desktop_window( thread_info );
    if (!found && try_child_desktop())
    {
        w32ios_log( "process %04x: no desktop window for thread %04x, registering the desktop classes",
                    (unsigned int)GetCurrentProcessId(), (unsigned int)GetCurrentThreadId() );
        register_desktop_class();
        found = query_desktop_window( thread_info );
        if (!found && !NtUserGetThreadDesktop( GetCurrentThreadId() ))
        {
            w32ios_log( "process %04x: thread %04x has no desktop, connecting it",
                        (unsigned int)GetCurrentProcessId(), (unsigned int)GetCurrentThreadId() );
            winstation_init();
            found = query_desktop_window( thread_info );
        }
        w32ios_log( "process %04x: desktop window %s", (unsigned int)GetCurrentProcessId(),
                    found ? "found" : "still missing" );
    }

    if (!found)
    {
        static LONG count;
        LONG n = InterlockedIncrement( &count );
        if (w32ios_log_event( n - 1 ))
            w32ios_log( "no desktop window for thread %04x yet (%d)", (unsigned int)GetCurrentThreadId(), (int)n );
        return 0;
    }

    user_driver->pSetDesktopWindow( UlongToHandle( thread_info->top_window ));
    register_builtin_classes();
    return UlongToHandle( thread_info->top_window );
}
