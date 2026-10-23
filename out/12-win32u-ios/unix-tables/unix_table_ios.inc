// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarifications 8 and 9: the two stand-in tables of one library. Included by <name>_ios.c
// after the library's upstream header (its call enum), with these defined:
//   W32IOS_TABLE_FUNCS    the 64-bit table's name   (<name>_unix_call_funcs, spec 09's lookup)
//   W32IOS_TABLE_WOW64    the WoW64 table's name    (<name>_unix_call_wow64_funcs)
//   W32IOS_TABLE_COUNT    the enum's count entry
//   W32IOS_TABLE_ID       the library's id, below W32IOS_UNIX_LIBS
//   W32IOS_TABLE_HEAD     the log line's head
//   W32IOS_TABLE_ENTRIES  "<name>_ios_entries.h", written by gen-entries.sh
//
// The setup entries return STATUS_SUCCESS and do nothing. Every other entry has its own small
// function, so the log line can give its index; none reads or writes its parameter block, so the
// same functions serve the WoW64 table (whose blocks hold 32-bit layouts).

#include "unix_tables_ios.h"

static const struct w32ios_unix_report table_report = { W32IOS_TABLE_ID, W32IOS_TABLE_HEAD };

static NTSTATUS __attribute__((unused)) table_setup( void *args )
{
    (void)args;
    return STATUS_SUCCESS;
}

#define W32IOS_UNIX_SETUP( name )
#define W32IOS_UNIX_UNSUPPORTED( name ) \
    static NTSTATUS unsupported_##name( void *args ) { (void)args; return w32ios_unix_unsupported( &table_report, name ); }
#include W32IOS_TABLE_ENTRIES
#undef W32IOS_UNIX_SETUP
#undef W32IOS_UNIX_UNSUPPORTED

#define W32IOS_UNIX_SETUP( name )       [name] = table_setup,
#define W32IOS_UNIX_UNSUPPORTED( name ) [name] = unsupported_##name,

const unixlib_entry_t W32IOS_TABLE_FUNCS[] =
{
#include W32IOS_TABLE_ENTRIES
};

const unixlib_entry_t W32IOS_TABLE_WOW64[] =
{
#include W32IOS_TABLE_ENTRIES
};

#undef W32IOS_UNIX_SETUP
#undef W32IOS_UNIX_UNSUPPORTED

/* every enum entry has its line (no NULL holes), and the tables have the enum's size */
C_ASSERT( W32IOS_UNIX_ENTRY_COUNT == W32IOS_TABLE_COUNT );
C_ASSERT( ARRAYSIZE(W32IOS_TABLE_FUNCS) == W32IOS_TABLE_COUNT );
C_ASSERT( ARRAYSIZE(W32IOS_TABLE_WOW64) == W32IOS_TABLE_COUNT );
C_ASSERT( W32IOS_TABLE_ID < W32IOS_UNIX_LIBS );
