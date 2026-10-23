// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Included by class.c (patches/B-02-class.patch) right after its statics, under WINE_IOS.
// Spec 12 Part B2, B3, B7, B8. The code is in class_ios.inc, included at the end of class.c.

#include "w32ios.h"

_Static_assert( sizeof(WINDOWPROC) == sizeof(w32ios_proc_pair), "WINDOWPROC is a pair of pointers" );
_Static_assert( NTUSER_NB_PROCS <= W32IOS_MAX_CLIENT_PROCS, "room for every built-in procedure" );

static WINDOWPROC *w32ios_find_winproc( WNDPROC func, BOOL ansi );
static WINDOWPROC *w32ios_get_winproc_ptr( WNDPROC handle );
static WNDPROC w32ios_proc_to_handle( WINDOWPROC *proc );
static void w32ios_initialize_client_procs( const ntuser_client_func_ptr *client_procsA,
                                            const ntuser_client_func_ptr *client_procsW,
                                            HINSTANCE user_module, pthread_once_t *once );
static void w32ios_register_builtin_classes( void );
static HINSTANCE w32ios_caller_user32_module( void );

/* B8: stop a walk of the class list that went on too long (a corrupt list) */
static inline BOOL w32ios_class_walk_stop( UINT walked )
{
    static LONG logged;

    if (walked <= W32IOS_CLASS_WALK_MAX) return FALSE;
    if (!InterlockedExchange( &logged, 1 ))
        w32ios_log( "class list of process %04x longer than %u entries: treated as not found",
                    (unsigned int)GetCurrentProcessId(), W32IOS_CLASS_WALK_MAX );
    return TRUE;
}

/* the global stays the fallback for a process that never loads user32 (B2) */
static HINSTANCE *const w32ios_global_user32_module = &user32_module;

/* per pseudo-process from here on */
#define class_list     (w32ios_proc_current()->pp_class_list)
#define user32_module  (w32ios_caller_user32_module())
