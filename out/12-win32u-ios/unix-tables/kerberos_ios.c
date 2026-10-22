// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarification 9: kerberos.dll's unix call tables on iOS, where there is no Kerberos.
//
// kerberos calls unix_process_attach after __wine_init_unix_call; it succeeds. The other entries
// (credentials, contexts, signatures, sealing, the ticket cache) return STATUS_NOT_SUPPORTED: no
// tickets.

#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "rpc.h"
#include "sspi.h"
#include "ntsecapi.h"
#include "ntsecpkg.h"
#include "wine/unixlib.h"
#include "unixlib.h"   /* Wine 11.4 dlls/kerberos/unixlib.h: enum unix_funcs */

#define W32IOS_TABLE_FUNCS   kerberos_unix_call_funcs
#define W32IOS_TABLE_WOW64   kerberos_unix_call_wow64_funcs
#define W32IOS_TABLE_COUNT   unix_funcs_count
#define W32IOS_TABLE_ID      3
#define W32IOS_TABLE_HEAD    "[kerberos] no Kerberos on iOS"
#define W32IOS_TABLE_ENTRIES "kerberos_ios_entries.h"
#include "unix_table_ios.inc"
