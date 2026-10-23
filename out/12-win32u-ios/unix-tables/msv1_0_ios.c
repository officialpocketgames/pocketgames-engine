// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarification 9: msv1_0's unix call tables on iOS, where there is no ntlm_auth (and no
// process may be spawned, spec 00).
//
// msv1_0 has no setup entry: its DllMain only calls __wine_init_unix_call. All three entries
// (unix_chat, unix_cleanup, unix_fork) return STATUS_NOT_SUPPORTED; a failed unix_fork makes NTLM
// unavailable, as on a machine without Samba's ntlm_auth.

#include <stdarg.h>
#include <sys/types.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "sspi.h"
#include "wine/unixlib.h"
#include "unixlib.h"   /* Wine 11.4 dlls/msv1_0/unixlib.h: enum ntlm_funcs */

#define W32IOS_TABLE_FUNCS   msv1_0_unix_call_funcs
#define W32IOS_TABLE_WOW64   msv1_0_unix_call_wow64_funcs
#define W32IOS_TABLE_COUNT   unix_funcs_count
#define W32IOS_TABLE_ID      4
#define W32IOS_TABLE_HEAD    "[msv1_0] no NTLM on iOS"
#define W32IOS_TABLE_ENTRIES "msv1_0_ios_entries.h"
#include "unix_table_ios.inc"
