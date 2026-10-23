// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarification 9: winspool.drv's unix call tables on iOS, where there is no CUPS.
//
// winspool's DllMain calls unix_process_attach after __wine_init_unix_call; it succeeds. The
// other entries (unix_enum_printers, unix_get_default_page_size, unix_get_ppd) return
// STATUS_NOT_SUPPORTED: no printers, and the PE side's own fallbacks (its built-in PPD, the
// locale's page size) apply.

#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winuser.h"
#include "winerror.h"
#include "winreg.h"
#include "wingdi.h"
#include "winspool.h"
#include "ddk/winsplp.h"
#include "wine/unixlib.h"
#include "wspool.h"   /* Wine 11.4 dlls/winspool.drv/wspool.h: enum cups_funcs */

#define W32IOS_TABLE_FUNCS   winspool_unix_call_funcs
#define W32IOS_TABLE_WOW64   winspool_unix_call_wow64_funcs
#define W32IOS_TABLE_COUNT   unix_funcs_count
#define W32IOS_TABLE_ID      1
#define W32IOS_TABLE_HEAD    "[winspool] no printing on iOS"
#define W32IOS_TABLE_ENTRIES "winspool_ios_entries.h"
#include "unix_table_ios.inc"
