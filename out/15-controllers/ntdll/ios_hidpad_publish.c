// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part C: the HID pad's registry entries, written by ntdll's unix side.
//
// setupapi lists HID devices from the registry, and on iOS no device manager writes them.
// ios_hidpad_publish() is called by spec 09 (clarification 1) in the session's first program,
// after the drive links and before the program's code runs. It publishes only the identity
// whose \??\<link> symlink exists: the server read MADEIRA_HIDPAD when it started, and the
// registry must name exactly the device that exists.
//
// Compiled into ntdll's unix library (spec 09) with Wine's headers.

#include "config.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "winternl.h"

#include "ios_hidpad.h"
#include "ios_hidpad_regplan.h"

#ifndef SYMBOLIC_LINK_QUERY
#define SYMBOLIC_LINK_QUERY 0x0001
#endif

void ios_hidpad_publish(void);

/* spec 09's log (ntdll's log file); stderr when it isn't linked */
#ifdef __APPLE__
extern void ios_log( const char *fmt, ... ) __attribute__((weak_import, format(printf,1,2)));
#else
extern void ios_log( const char *fmt, ... ) __attribute__((weak, format(printf,1,2)));
#endif

#define PUBLISH_LOG(...) do { \
    if (ios_log) ios_log( __VA_ARGS__ ); \
    else fprintf( stderr, __VA_ARGS__ ); \
} while (0)

#define MAX_NAME 512

/* ASCII to a counted UTF-16 string in `buffer` (NUL-terminated too). */
static int to_unicode( const char *ascii, size_t len, WCHAR *buffer, size_t count, UNICODE_STRING *str )
{
    size_t i;

    if (len >= count || len * sizeof(WCHAR) > 0xfffc) return 0;
    for (i = 0; i < len; i++) buffer[i] = (WCHAR)(unsigned char)ascii[i];
    buffer[len] = 0;
    str->Buffer = buffer;
    str->Length = (USHORT)(len * sizeof(WCHAR));
    str->MaximumLength = (USHORT)((len + 1) * sizeof(WCHAR));
    return 1;
}

static int link_exists( const struct ios_hidpad_identity *id )
{
    char path[MAX_NAME];
    WCHAR pathW[MAX_NAME];
    UNICODE_STRING str;
    OBJECT_ATTRIBUTES attr;
    HANDLE handle;
    size_t len;

    if (!(len = ios_hidpad_symlink_path( id, path, sizeof(path) ))) return 0;
    if (!to_unicode( path, len, pathW, MAX_NAME, &str )) return 0;
    InitializeObjectAttributes( &attr, &str, OBJ_CASE_INSENSITIVE, 0, NULL );
    if (NtOpenSymbolicLinkObject( &handle, SYMBOLIC_LINK_QUERY, &attr )) return 0;
    NtClose( handle );
    return 1;
}

/* Open or create one level `name[0..len-1]` below `root` (0: `name` is a full NT path). */
static HANDLE create_level( HANDLE root, const char *name, size_t len, ULONG options )
{
    WCHAR nameW[MAX_NAME];
    UNICODE_STRING str;
    OBJECT_ATTRIBUTES attr;
    HANDLE key = 0;

    if (!to_unicode( name, len, nameW, MAX_NAME, &str )) return 0;
    InitializeObjectAttributes( &attr, &str, OBJ_CASE_INSENSITIVE, root, NULL );
    if (NtCreateKey( &key, KEY_ALL_ACCESS, &attr, 0, NULL, options, NULL )) return 0;
    return key;
}

/* Walk `path` (levels separated by '\') below `root`, creating what is missing; `root` is
 * closed. Returns the last level's handle, or 0. */
static HANDLE create_path( HANDLE root, const char *path, ULONG options )
{
    const char *p = path, *end;
    HANDLE key;

    while (root && *p)
    {
        if (!(end = strchr( p, '\\' ))) end = p + strlen( p );
        key = end > p ? create_level( root, p, (size_t)(end - p), options ) : 0;
        NtClose( root );
        root = key;
        p = *end ? end + 1 : end;
    }
    return root;
}

static int set_value( HANDLE key, const struct ios_hidpad_reg_value *v )
{
    WCHAR nameW[64], dataW[IOS_HIDPAD_REG_TEXT];
    UNICODE_STRING name;
    size_t i;

    if (!to_unicode( v->name, strlen( v->name ), nameW, 64, &name )) return 0;
    if (v->type == IOS_HIDPAD_REG_DWORD)
    {
        DWORD value = v->dword;
        return !NtSetValueKey( key, &name, 0, REG_DWORD, &value, sizeof(value) );
    }
    for (i = 0; i < v->text_len; i++) dataW[i] = (WCHAR)(unsigned char)v->text[i];
    return !NtSetValueKey( key, &name, 0, v->type == IOS_HIDPAD_REG_MULTI_SZ ? REG_MULTI_SZ : REG_SZ,
                           dataW, (ULONG)(v->text_len * sizeof(WCHAR)) );
}

static int write_key( const struct ios_hidpad_reg_key *k )
{
    static const char machine[] = "\\Registry\\Machine";
    HANDLE key;
    unsigned int i;
    int ok = 1;

    key = create_level( 0, machine, sizeof(machine) - 1, 0 );
    key = create_path( key, k->base, 0 );
    key = create_path( key, k->tail, REG_OPTION_VOLATILE );
    if (!key) return 0;
    for (i = 0; i < k->value_count; i++) ok &= set_value( key, &k->values[i] );
    NtClose( key );
    return ok;
}

void ios_hidpad_publish(void)
{
    static const enum ios_hidpad_kind kinds[] = { IOS_HIDPAD_DUALSENSE, IOS_HIDPAD_GENERIC };
    static struct ios_hidpad_reg_key keys[IOS_HIDPAD_REG_KEYS];
    struct ios_hidpad_identity id;
    const char *env = getenv( "MADEIRA_HIDPAD" );
    unsigned int i, count, written = 0;
    int found = 0;

    /* No HID pad: nothing at all. Publishing again (a later session in the same app run) is
     * harmless: the keys are opened and the values rewritten. */
    if (!env || !*env) return;

    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]) && !found; i++)
    {
        ios_hidpad_identity_get( kinds[i], getenv( "MADEIRA_HIDPAD_NAME" ), &id );
        found = link_exists( &id );
    }
    if (!found)
    {
        PUBLISH_LOG( "[hidpad] MADEIRA_HIDPAD=%s but no HID pad link exists in the server: no registry entries\n", env );
        return;
    }

    count = ios_hidpad_registry_plan( &id, keys );
    for (i = 0; i < count; i++) written += write_key( &keys[i] );
    PUBLISH_LOG( "[hidpad] %s: %u of %u registry keys written for %s\\%s\n", id.name, written,
                 (unsigned int)IOS_HIDPAD_REG_KEYS, id.device_id, id.instance );
}
