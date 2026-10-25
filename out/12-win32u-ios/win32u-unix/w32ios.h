// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Internal declarations of the iOS win32u (spec 12) for files that include Wine's headers.
// Include after win32u_private.h / ntuser_private.h.

#ifndef W32IOS_H
#define W32IOS_H

#include <stdarg.h>
#include <pthread.h>
#include "wine/list.h"
#include "w32ios_pure.h"
#include "w32ios_s17_pure.h"

/* ---------------------------------------------------------------- the other Wine parts */

extern void *ios_current_peb( void );                 /* spec 09 Part B1 */
extern unsigned long ios_wow_base( void );            /* spec 06 / spec 10 Part E3 */
extern unsigned long long ios_get_real_usd( void );   /* spec 10 clarification 1 */
extern void ios_wow_fixup_peb64_ptrs( void );         /* spec 10 clarification 2 */
extern int ios_wow_session_armed( void ) __attribute__((weak));   /* spec 10 clarification 3 */

/* ---------------------------------------------------------------- logging (w32ios_init.c) */

/* One line to the unix side's log, with a "[win32u] " prefix (no newline needed). */
void w32ios_log( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));
/* The same, unless PG_QUIET=1. */
void w32ios_log_verbose( const char *fmt, ... ) __attribute__((format(printf, 1, 2)));
/* An environment switch: unset or empty gives def, then w32ios_parse_bool. */
BOOL w32ios_env_bool( const char *name, BOOL def );

/* ---------------------------------------------------------------- Part B: per pseudo-process */

#define W32IOS_PARAM_KEYS 16   /* >= sysparams.c's NB_PARAM_KEYS (checked there) */

/* Everything upstream keeps per process in a global or static of dlls/win32u (CHANGES.md,
 * Part B audit, class "process"). One per pseudo-process, found through its PEB. In the patched
 * files the upstream name of each global becomes a macro for its field here. The fields that
 * stand for an upstream global are named pp_<upstream name>, never the upstream name itself:
 * a macro named like a field would rewrite every access to that field after it. */
struct w32ios_proc
{
    /* B2: user32's built-in procedures and module of this process */
    struct w32ios_client_procs client_procs;
    HINSTANCE        user32;          /* its user32.dll */
    /* B3: built-in classes */
    pthread_mutex_t  builtins_lock;
    LONG             builtins_done;
    LONG             builtins_owner;          /* thread id registering them now */
    /* B4: one try at a desktop window for a child process */
    LONG             child_desktop_tried;
    /* the GDI handle table is attached to this process's PEB (w32ios_mem.c) */
    LONG             gdi_attached;
    /* C6: the registry GPU was written by this process */
    BOOL             gpu_written;
    /* E6: when the foreground window was last fixed for this process */
    DWORD            foreground_fix_time;
    BOOL             foreground_fix_done;
    /* class.c */
    struct list      pp_class_list;
    /* dce.c */
    struct list      pp_dce_list;
    struct list      pp_window_surfaces;
    DWORD            pp_last_idle;
    /* dc.c */
    struct list      pp_dc_attr_buckets;
    /* cursoricon.c */
    struct list      pp_icon_cache;
    /* window.c */
    volatile unsigned int pp_startup_info_flags;
    unsigned int     pp_startup_show_window;
    struct list      pp_client_surfaces;
    /* font.c: hkcu_key, wine_fonts_key, wine_fonts_cache_key */
    HKEY             font_keys[3];
    /* sysparams.c */
    HKEY             pp_video_key, pp_enum_key, pp_control_key, pp_config_key, pp_volatile_base_key;
    HKEY             base_keys[W32IOS_PARAM_KEYS], volatile_keys[W32IOS_PARAM_KEYS];
    LONG             pp_dpi_context;
    DWORD            pp_process_layout;
    HDC              pp_display_dc;
    DWORD            pp_exiting_thread_id;
    UINT64           pp_thunk_lock_callback;
    /* input.c */
    LONG             pp_enable_mouse_in_pointer;
    struct { HBITMAP bitmap; unsigned int timeout; } pp_caret;
    /* rawinput.c */
    RAWINPUTDEVICE  *pp_registered_devices;
    unsigned int     pp_registered_device_count;
    /* palette.c */
    HPALETTE         pp_hPrimaryPalette, pp_hLastRealizedPalette;
    /* defwnd.c */
    short            pp_f10_key, pp_menu_sys_key;
    /* menu.c */
    HWND             pp_top_popup;
    HMENU            pp_top_popup_hmenu;
    BOOL             pp_exit_menu;
    /* scroll.c */
    struct SCROLL_TRACKING_INFO pp_g_tracking_info;
    BOOL             pp_scroll_moving_thumb, pp_scroll_timer_running;
    POINT            pp_prev_pt;
    UINT             pp_track_thumb_pos;
    int              pp_last_click_pos, pp_last_mouse_pos;
    /* message.c */
    MSG              pp_clk_msg;
    /* imm.c */
    BOOL             pp_disable_ime;
};

/* The calling thread's pseudo-process (created on first use). Never NULL. */
struct w32ios_proc *w32ios_proc_current( void );
/* A session starts (w32ios_proc.c calls it): resets of the session-wide state of every Part. */
void w32ios_session_start( void );
/* Set when the session's first process finished init_user (B4); its PEB. */
void w32ios_set_session_process( void *peb );
void *w32ios_session_peb( void );
BOOL w32ios_session_initialized( void );

/* B1: the allocation ceiling of the calling thread (cached per thread). */
ULONG_PTR w32ios_zero_bits( void );

/* B4/B5: get_desktop_window (w32ios_desktop.c) */
HWND w32ios_get_desktop_window( void );

/* B9: KUSER_SHARED_DATA through spec 10's real page (a zeroed page before it exists) */
const void *w32ios_usd( void );

/* GDI handle table and session views outliving the process that mapped them (w32ios_mem.c) */
void *w32ios_gdi_table( SIZE_T *size );            /* app-wide, allocated once */
void  w32ios_gdi_attach_proc( struct w32ios_proc *proc );   /* publish it in the caller's PEB */

/* Spec 17 F2: whether 32-bit code runs in this session (the rules of F4-F6 apply) */
BOOL w32ios_gdi_session_32bit( void );
/* Spec 17 F4: before any handle-table operation for a thread, the table's address must be in
 * its pseudo-process's PEB; the thread remembers the PEB it published for, so the steady state
 * is one load and one compare */
extern __thread const void *w32ios_gdi_published_peb;
void w32ios_gdi_publish_slow( void );
static inline void w32ios_gdi_publish( void )
{
    TEB *teb = NtCurrentTeb();

    if (teb && w32ios_s17_publish_needed( w32ios_gdi_published_peb, teb->Peb ))
        w32ios_gdi_publish_slow();
}
/* Spec 17 F5/F6: the calling thread as the owner of a DC attribute bucket or a cache DCE */
void w32ios_s17_caller( struct w32ios_s17_owner *owner );
void  w32ios_detach_view( void **base, SIZE_T size );
/* NLS code page data copied out of the calling process's views (font.c) */
void  w32ios_init_cptable( const USHORT *data, CPTABLEINFO *info );
const USHORT *w32ios_copy_nls( const USHORT *data, SIZE_T size );
/* a heap copy of a whole view, which is unmapped (the view on failure) */
void *w32ios_copy_view( void *view, SIZE_T size );

/* font.c: per-process registry keys (0 hkcu, 1 Software\Wine\Fonts, 2 its Cache) */
HKEY *w32ios_font_key( int which );

/* ---------------------------------------------------------------- Part C: the virtual monitor */

/* w32ios_display.c. Reading a size takes no lock (callers may hold the display lock). */
void w32ios_display_session_start( void );          /* a program with no parent starts (C2, clar. 6/7/10) */
/* w32ios_proc.c, clarification 10: another user program of the session runs (TRUE when unknown) */
BOOL w32ios_other_user_program_running( void );
/* sysparams_ios.inc, clarification 10: the size changed for a new program (no lock held): the
 * work area is recomputed, and the server gets the monitor on the next display lock */
void w32ios_monitor_resized( void );
struct w32ios_size w32ios_display_default( void );
struct w32ios_size w32ios_display_current( void );
void w32ios_display_set_current( struct w32ios_size size );
BOOL w32ios_display_extended_modes( void );          /* PG_EXTENDED_MODES=1 */
BOOL w32ios_display_mode_set_enabled( void );        /* PG_VIRTUAL_MODE_SET != 0 */
/* the session default goes to the server once per session (C5): returns TRUE the first time */
BOOL w32ios_display_take_report( void );
/* fill a DEVMODEW's display fields (32 bpp, 60 Hz, at 0,0) */
void w32ios_display_fill_devmode( DEVMODEW *devmode, struct w32ios_size size );

/* C6: the registry GPU of this launch, written once per process (sysparams_ios.inc) */
struct w32ios_gpu
{
    BOOL      valid;
    UINT16    vendor, device;
    char      name[64];
    char      path[MAX_PATH];   /* under Enum: PCI\VEN_xxxx&DEV_xxxx&SUBSYS_00000000&REV_00\00000000 */
    char      guid[39];         /* its VideoID, "{...}" */
    LUID      luid;
    ULONGLONG memory;           /* dedicated memory in bytes */
};
/* the identity this launch uses: Apple 106B:0001, or NVIDIA 10DE:2544 with DXMT_ENABLE_NVEXT=1 */
void w32ios_gpu_identity( UINT16 *vendor, UINT16 *device, const char **name );
/* the app's pgios_display_mode_changed, if it provides one (w32ios_display.c) */
void w32ios_app_display_mode_changed( int w, int h );

/* ---------------------------------------------------------------- Part D: D3DKMT adapter */

/* w32ios_kmt.c */
BOOL w32ios_kmt_enabled( void );                 /* PG_KMT_ADAPTER=1/on/true/yes */
BOOL w32ios_kmt_luid( LUID *luid );              /* with the adapter on: Metal's LUID */
ULONGLONG w32ios_kmt_memory( void );             /* the registry GPU's dedicated memory */

/* ---------------------------------------------------------------- Part E: the iOS user driver */

/* w32ios_driver.c */
void w32ios_driver_install( void );              /* __wine_set_user_driver, once */
void w32ios_driver_session_start( void );
BOOL w32ios_desktop_mode( void );                /* PG_DESKTOP=1 */
/* E2: a pure poll may skip its set_queue_mask request (TRUE) unless a second passed */
BOOL w32ios_skip_queue_mask( void );
/* E2: NtWaitForMultipleObjects for a message wait, in 16 ms slices when the thread slices */
NTSTATUS w32ios_wait_objects( DWORD count, const HANDLE *handles, WAIT_TYPE type, BOOLEAN alertable,
                              LARGE_INTEGER *abs );
/* E6: a process asks for controller state: make its own window the foreground one if needed */
void w32ios_foreground_fix( void );

/* ---------------------------------------------------------------- Part F: XInput */

/* NtUserCallTwoParam_GetGamepadState (w32ios_gamepad.c) */
ULONG_PTR w32ios_gamepad_call( ULONG_PTR arg1, ULONG_PTR arg2 );

#endif /* W32IOS_H */
