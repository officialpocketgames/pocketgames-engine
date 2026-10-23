// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/*
 * Spec 18 Part H5 and H6: DLL name rules of the loader, pure code shared by loader.c and the
 * host tests. Names are UTF-16 (WCHAR) strings with an explicit length.
 *   H5  is a relative name one of the XInput DLLs that must come from the system directory?
 *   H6  the path in the other 64-bit farm: \??\C:\windows\sysx64\<base name>[.dll]
 *   clarification 1: the same for the aarch64 build, \??\C:\windows\sysaa64\<base name>[.dll],
 *       logged once per DLL name
 */

#ifndef MYIOSDECK_NTDLL_NAMES_H
#define MYIOSDECK_NTDLL_NAMES_H

#include <stddef.h>

#define NAMES_MAX_PATH 260

static inline unsigned short names_lower( unsigned short c )
{
    return (c >= 'A' && c <= 'Z') ? (unsigned short)(c + 'a' - 'A') : c;
}

/* case-insensitive (ASCII) compare of a counted name with a NUL-terminated ASCII string */
static inline int names_equal_ascii( const unsigned short *name, size_t len, const char *ascii )
{
    size_t i;

    for (i = 0; i < len; i++)
        if (!ascii[i] || names_lower( name[i] ) != (unsigned short)ascii[i]) return 0;
    return !ascii[len];
}

/* H5: the name must be relative (no path separator) and equal one of the XInput names */
static inline int names_is_builtin_xinput( const unsigned short *name, size_t len )
{
    static const char * const xinput[] =
    {
        "xinput1_1.dll", "xinput1_2.dll", "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll", "xinputuap.dll",
    };
    size_t i;

    for (i = 0; i < len; i++)
        if (name[i] == '\\' || name[i] == '/' || name[i] == ':') return 0;
    for (i = 0; i < sizeof(xinput) / sizeof(xinput[0]); i++)
        if (names_equal_ascii( name, len, xinput[i] )) return 1;
    return 0;
}

/* H3: is a failed load of this name always logged, past the per-process cap? Its base name
 * starts with one of the subsystem prefixes or contains ".drv" (ASCII, no case) */
static inline int names_always_log_failure( const unsigned short *name, size_t len )
{
    static const char * const prefixes[] =
    {
        "xinput", "steam_api", "d3d11", "dxgi", "winemetal", "d3d10", "opengl32", "d3dcompiler",
    };
    size_t i, j, start = 0;

    for (i = 0; i < len; i++)
        if (name[i] == '\\' || name[i] == '/') start = i + 1;
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
    {
        const char *p = prefixes[i];
        for (j = 0; p[j] && start + j < len; j++)
            if (names_lower( name[start + j] ) != (unsigned short)p[j]) break;
        if (!p[j]) return 1;
    }
    for (i = start; i + 4 <= len; i++)
        if (name[i] == '.' && names_lower( name[i + 1] ) == 'd' && names_lower( name[i + 2] ) == 'r' &&
            names_lower( name[i + 3] ) == 'v')
            return 1;
    return 0;
}

/* H6 and clarification 1: build \??\C:\windows\<farm>\<base name>[.dll] for name into out
 * (out_cap characters including the terminator); farm is "sysx64" (ARM64EC build) or "sysaa64"
 * (aarch64 build). Returns the length without terminator, or 0 when it doesn't fit MAX_PATH or
 * out, or the name has no base name */
static inline size_t names_farm_path( const char *farm, const unsigned short *name, size_t len,
                                      unsigned short *out, size_t out_cap )
{
    static const char prefix[] = "\\??\\C:\\windows\\";
    static const char ext[] = ".dll";
    size_t i, start = 0, pos = 0, total, farm_len = 0;
    int has_dot = 0;

    while (farm[farm_len]) farm_len++;
    for (i = 0; i < len; i++)
        if (name[i] == '\\' || name[i] == '/') start = i + 1;
    if (start >= len) return 0;
    for (i = start; i < len; i++)
        if (name[i] == '.') has_dot = 1;

    total = (sizeof(prefix) - 1) + farm_len + 1 + (len - start) + (has_dot ? 0 : sizeof(ext) - 1);
    if (total + 1 > NAMES_MAX_PATH || total + 1 > out_cap) return 0;

    for (i = 0; prefix[i]; i++) out[pos++] = (unsigned char)prefix[i];
    for (i = 0; i < farm_len; i++) out[pos++] = (unsigned char)farm[i];
    out[pos++] = '\\';
    for (i = start; i < len; i++) out[pos++] = name[i];
    if (!has_dot) for (i = 0; ext[i]; i++) out[pos++] = (unsigned char)ext[i];
    out[pos] = 0;
    return pos;
}

static inline size_t names_sysx64_path( const unsigned short *name, size_t len, unsigned short *out, size_t out_cap )
{
    return names_farm_path( "sysx64", name, len, out, out_cap );
}

/* H6 and clarification 1: the other farm is tried only after the normal search (system32 and
 * the load path) ended in one of these two statuses */
#define NAMES_STATUS_INVALID_IMAGE_FORMAT 0xc000007bu
#define NAMES_STATUS_DLL_NOT_FOUND        0xc0000135u

static inline int names_farm_fallback_due( unsigned int status )
{
    return status == NAMES_STATUS_INVALID_IMAGE_FORMAT || status == NAMES_STATUS_DLL_NOT_FOUND;
}

/* clarification 1: log a substitution once per DLL name (base name, no case); a full table
 * logs nothing more. The caller holds the loader lock. */
#define NAMES_LOGGED_MAX 128

struct names_logged
{
    unsigned int hashes[NAMES_LOGGED_MAX];
    unsigned int count;
};

static inline int names_log_once( struct names_logged *set, const unsigned short *name, size_t len )
{
    unsigned int hash = 2166136261u, i;
    size_t start = 0, j;

    for (j = 0; j < len; j++)
        if (name[j] == '\\' || name[j] == '/') start = j + 1;
    for (j = start; j < len; j++) hash = (hash ^ names_lower( name[j] )) * 16777619u;
    for (i = 0; i < set->count; i++) if (set->hashes[i] == hash) return 0;
    if (set->count == NAMES_LOGGED_MAX) return 0;
    set->hashes[set->count++] = hash;
    return 1;
}

#endif /* MYIOSDECK_NTDLL_NAMES_H */
