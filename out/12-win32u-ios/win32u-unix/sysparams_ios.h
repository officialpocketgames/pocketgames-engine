// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Included by sysparams.c (patches/B-12-sysparams.patch) after its statics, under WINE_IOS.
// The code is in sysparams_ios.inc, included at the end of sysparams.c.

#include "w32ios.h"
#include "w32ios_kmt.h"

_Static_assert( NB_PARAM_KEYS <= W32IOS_PARAM_KEYS, "room for the parameter keys" );

/* ---------------------------------------------------------------- Part B */

/* registry handles of this pseudo-process; config_key and volatile_base_key are opened by
 * sysparams_init in the session's first process only, so other processes open them on use */
static HKEY *w32ios_config_key( void );
static HKEY *w32ios_volatile_base_key( void );

#define hkcu_key            (*w32ios_font_key( 0 ))
#define video_key           (w32ios_proc_current()->pp_video_key)
#define enum_key            (w32ios_proc_current()->pp_enum_key)
#define control_key         (w32ios_proc_current()->pp_control_key)
#define config_key          (*w32ios_config_key())
#define volatile_base_key   (*w32ios_volatile_base_key())
/* other per-process state of upstream */
#define process_layout      (w32ios_proc_current()->pp_process_layout)
#define display_dc          (w32ios_proc_current()->pp_display_dc)
#define thunk_lock_callback (w32ios_proc_current()->pp_thunk_lock_callback)
/* dpi_context is also a member name (ntuser_thread_info), so its three uses are patched */
#define W32IOS_PROCESS_DPI_CONTEXT (w32ios_proc_current()->pp_dpi_context)

/* ---------------------------------------------------------------- Part C */

static BOOL w32ios_lock_display_devices( BOOL force );
static void w32ios_display_lock_released( void );
static RECT w32ios_virtual_monitor_rect( void );
static RECT w32ios_get_display_rect( const WCHAR *display );
static NTSTATUS w32ios_enum_display_devices( UNICODE_STRING *device, DWORD index, DISPLAY_DEVICEW *info,
                                             DWORD flags );
static BOOL w32ios_enum_display_settings( UNICODE_STRING *device, DWORD index, DEVMODEW *devmode,
                                          DWORD flags );
static LONG w32ios_change_display_settings( UNICODE_STRING *devname, DEVMODEW *devmode, HWND hwnd,
                                            DWORD flags, void *lparam );
static NTSTATUS w32ios_open_adapter_from_gdi_name( D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME *desc );
static void w32ios_fix_nonclient_metrics( NONCLIENTMETRICSW *ncm );
static void w32ios_screen_metric_queried( void );
