// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarification 9: netapi32's unix call tables on iOS, where there is no Samba.
//
// netapi32 calls unix_netapi_init once before its first remote call; it succeeds. The other
// entries (server, share and workstation information, password change) return
// STATUS_NOT_SUPPORTED: no domain, no remote machines.

#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/unixlib.h"
#include "unixlib.h"   /* Wine 11.4 dlls/netapi32/unixlib.h: enum samba_funcs */

#define W32IOS_TABLE_FUNCS   netapi32_unix_call_funcs
#define W32IOS_TABLE_WOW64   netapi32_unix_call_wow64_funcs
#define W32IOS_TABLE_COUNT   unix_funcs_count
#define W32IOS_TABLE_ID      2
#define W32IOS_TABLE_HEAD    "[netapi32] no Samba on iOS"
#define W32IOS_TABLE_ENTRIES "netapi32_ios_entries.h"
#include "unix_table_ios.inc"
