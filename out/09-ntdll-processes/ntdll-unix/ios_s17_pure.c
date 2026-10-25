// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Parts B, C and D: the pure rules (see ios_s17_pure.h). Written from the spec; the
// FNV-1a constants are the published ones (offset basis 2166136261, prime 16777619).

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ios_s17_pure.h"

/* the Windows constants the rules need (winnt.h / winternl.h values) */
#define S17_FILE_WRITE_DATA   0x0002
#define S17_FILE_APPEND_DATA  0x0004
#define S17_GENERIC_WRITE     0x40000000u
#define S17_FILE_OPEN         1

/* ---------------------------------------------------------------- switches */

int ios_s17_switch_value( const char *value )
{
    return !(value && value[0] == '0' && !value[1]);
}

/* ---------------------------------------------------------------- Part B */

int ios_s17_is_wow64( int by_teb, int teb_has_wow_teb, int process_wide )
{
    if (by_teb && teb_has_wow_teb) return 1;
    return !!process_wide;
}

int ios_s17_in_wow64_call( int owner, unsigned long wow_base, int upstream )
{
    if (!owner) return !!upstream;
    return wow_base != 0;
}

int ios_s17_ec_bit( const uint64_t *map, uintptr_t ptr, unsigned long page_size )
{
    uintptr_t page;

    if (!map || !page_size) return 0;
    page = ptr / page_size;
    return (int)((map[page / 64] >> (page & 63)) & 1);
}

unsigned long ios_s17_section_limit( int per_process, unsigned long wow_base,
                                     unsigned long wow_limit )
{
    if (!per_process) return wow_limit;
    return wow_base ? wow_limit : 0;
}

/* ---------------------------------------------------------------- Part C1 */

uint32_t ios_s17_volume_serial( uint32_t stored, unsigned char letter )
{
    uint32_t hash = 2166136261u;
    unsigned char bytes[5];
    int i;

    if (!stored) return 0;
    bytes[0] = stored & 0xff;
    bytes[1] = (stored >> 8) & 0xff;
    bytes[2] = (stored >> 16) & 0xff;
    bytes[3] = (stored >> 24) & 0xff;
    bytes[4] = letter;
    for (i = 0; i < 5; i++)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    if (hash == 0 || hash == 0xffffffffu) return stored;
    return hash;
}

uint32_t ios_s17_parse_serial( const char *buf, size_t len )
{
    char text[16];
    unsigned long long value;
    char *end;

    if (!buf) return 0;
    if (len > 15) len = 15;
    memcpy( text, buf, len );
    text[len] = 0;
    errno = 0;
    value = strtoull( text, &end, 16 );
    if (end == text || errno || value > 0xffffffffull) return 0;
    return (uint32_t)value;
}

static uint32_t read_serial_file( const char *path, int *exists )
{
    char buf[15];
    ssize_t len;
    int fd;

    *exists = 0;
    if ((fd = open( path, O_RDONLY | O_CLOEXEC )) == -1) return 0;
    *exists = 1;
    do len = read( fd, buf, sizeof(buf) ); while (len == -1 && errno == EINTR);
    close( fd );
    if (len <= 0) return 0;
    return ios_s17_parse_serial( buf, (size_t)len );
}

/* Every racer of one app run is a thread of this process (pseudo-processes), so they take turns;
 * the file itself appears complete or not at all: the value is written to a private temporary
 * file, which is then linked to the final name (link fails with EEXIST if a winner exists, with
 * O_CREAT | O_EXCL's meaning) or, over a file holding no value, renamed onto it. A reader never
 * sees a created but still empty file. */
static pthread_mutex_t serial_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t load_serial_locked( const char *path, ios_s17_random_fn rnd )
{
    char tmp[4200], text[16];
    uint32_t value, winner;
    int fd, exists, n;

    if ((value = read_serial_file( path, &exists ))) return value;
    if (!rnd) return 0;
    do value = rnd(); while (value == 0 || value == 0xffffffffu);

    n = snprintf( tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)getpid() );
    if (n < 0 || (size_t)n >= sizeof(tmp)) return value;
    unlink( tmp );
    if ((fd = open( tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600 )) == -1)
        return value;   /* no file for this run: still a stable value */
    n = snprintf( text, sizeof(text), "%08x\n", value );
    if (write( fd, text, (size_t)n ) != n)
    {
        close( fd );
        unlink( tmp );   /* never leave a partial value */
        return value;
    }
    close( fd );

    if (!exists)
    {
        if (!link( tmp, path ))
        {
            unlink( tmp );
            return value;
        }
        if (errno != EEXIST)
        {
            unlink( tmp );
            return value;
        }
        /* another program created it first: its value wins */
        if ((winner = read_serial_file( path, &exists )))
        {
            unlink( tmp );
            return winner;
        }
    }
    /* the file holds 0 or nothing hex: replace it (QUESTIONS.md, spec 17 #1) */
    if (rename( tmp, path )) unlink( tmp );
    return value;
}

uint32_t ios_s17_load_serial( const char *config_dir, ios_s17_random_fn rnd )
{
    char path[4096];
    uint32_t value;
    int n;

    if (!config_dir || !config_dir[0]) return 0;
    n = snprintf( path, sizeof(path), "%s/%s", config_dir, IOS_S17_SERIAL_FILE );
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;

    pthread_mutex_lock( &serial_mutex );
    value = load_serial_locked( path, rnd );
    pthread_mutex_unlock( &serial_mutex );
    return value;
}

/* ---------------------------------------------------------------- Part C2 */

int ios_s17_open_is_write( uint32_t access, uint32_t disposition )
{
    if (access & (S17_FILE_WRITE_DATA | S17_FILE_APPEND_DATA | S17_GENERIC_WRITE)) return 1;
    return disposition != S17_FILE_OPEN;
}

int ios_s17_is_windows_path( const uint16_t *name, size_t len )
{
    static const char windows[] = "c:\\windows";
    size_t i, n = sizeof(windows) - 1;

    if (!name) return 0;
    if (len >= 4 && name[0] == '\\' && (name[1] == '?' || name[1] == '\\') && name[2] == '?' &&
        name[3] == '\\')
    {
        name += 4;
        len -= 4;
    }
    if (len < n) return 0;
    for (i = 0; i < n; i++)
    {
        uint16_t c = name[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)windows[i]) return 0;
    }
    return len == n || name[n] == '\\' || name[n] == '/';
}

int ios_s17_budget_kind( int is_write, const uint16_t *name, size_t len )
{
    if (is_write) return IOS_S17_BUDGET_WRITE;
    if (!name || !len || ios_s17_is_windows_path( name, len )) return IOS_S17_BUDGET_ANY;
    return IOS_S17_BUDGET_PROGRAM;
}

int ios_s17_budget_take( struct ios_s17_budgets *budgets, int kind )
{
    int *count, limit;

    switch (kind)
    {
    case IOS_S17_BUDGET_WRITE:   count = &budgets->write;   limit = IOS_S17_FAIL_BUDGET_WRITE;   break;
    case IOS_S17_BUDGET_PROGRAM: count = &budgets->program; limit = IOS_S17_FAIL_BUDGET_PROGRAM; break;
    default:                     count = &budgets->any;     limit = IOS_S17_FAIL_BUDGET_ANY;     break;
    }

    if (__atomic_load_n( count, __ATOMIC_RELAXED ) >= limit) return 0;
    return __atomic_fetch_add( count, 1, __ATOMIC_RELAXED ) < limit;
}

void ios_s17_ascii_w( char *dst, size_t cap, const uint16_t *src, size_t len )
{
    size_t i;

    if (!cap) return;
    for (i = 0; src && i < len && i + 1 < cap; i++)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : '?';
    dst[i] = 0;
}

void ios_s17_ascii_a( char *dst, size_t cap, const char *src )
{
    size_t i;

    if (!cap) return;
    for (i = 0; src && src[i] && i + 1 < cap; i++)
    {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    dst[i] = 0;
}

/* ---------------------------------------------------------------- Part D3 */

static int is_blank( char c )
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f';
}

int ios_s17_cfg_lookup( const char *text, size_t len, const char *key, char *out, size_t cap )
{
    size_t pos = 0, key_len = strlen( key );
    int found = 0;

    if (len >= 3 && (unsigned char)text[0] == 0xef && (unsigned char)text[1] == 0xbb &&
        (unsigned char)text[2] == 0xbf)
        pos = 3;   /* UTF-8 BOM */

    while (pos < len)
    {
        size_t start = pos, end, k0, k1, v0, v1, eq;

        while (pos < len && text[pos] != '\n') pos++;
        end = pos++;   /* [start, end) is the line */

        k0 = start;
        while (k0 < end && is_blank( text[k0] )) k0++;
        if (k0 == end || text[k0] == '#') continue;
        for (eq = k0; eq < end && text[eq] != '='; eq++) ;
        if (eq == end) continue;   /* no '=' */
        k1 = eq;
        while (k1 > k0 && is_blank( text[k1 - 1] )) k1--;
        if (k1 - k0 != key_len || memcmp( text + k0, key, key_len )) continue;
        v0 = eq + 1;
        while (v0 < end && is_blank( text[v0] )) v0++;
        v1 = end;
        while (v1 > v0 && is_blank( text[v1 - 1] )) v1--;
        if (cap)
        {
            size_t n = v1 - v0 < cap - 1 ? v1 - v0 : cap - 1;
            memcpy( out, text + v0, n );
            out[n] = 0;
        }
        found = 1;   /* keep going: the last occurrence wins */
    }
    return found;
}

int ios_s17_cfg_file_get( const char *docs_dir, const char *key, char *out, size_t cap )
{
    char path[4096], *text = NULL;
    size_t size = 0, used = 0;
    ssize_t n;
    int fd, found;

    if (!docs_dir || !docs_dir[0]) return 0;
    if (snprintf( path, sizeof(path), "%s/pocketgames.cfg", docs_dir ) >= (int)sizeof(path)) return 0;
    if ((fd = open( path, O_RDONLY | O_CLOEXEC )) == -1) return 0;
    for (;;)
    {
        if (used == size)
        {
            char *grown;
            if (size >= (1u << 20)) break;   /* a settings file is small; read at most 1 MB */
            size = size ? size * 2 : 4096;
            if (!(grown = realloc( text, size ))) break;
            text = grown;
        }
        n = read( fd, text + used, size - used );
        if (n == -1 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close( fd );
    found = text ? ios_s17_cfg_lookup( text, used, key, out, cap ) : 0;
    free( text );
    return found;
}

int ios_s17_parse_int( const char *value, long *out )
{
    char *end;
    long v;

    if (!value) return 0;
    while (is_blank( *value )) value++;
    if (!*value) return 0;
    errno = 0;
    v = strtol( value, &end, 10 );
    if (end == value || errno) return 0;
    while (is_blank( *end )) end++;
    if (*end) return 0;
    *out = v;
    return 1;
}

int ios_s17_cpu_override( const char *env_value, const char *cfg_value )
{
    long count = 0;

    if (!(ios_s17_parse_int( env_value, &count ) && count > 0))
    {
        count = 0;
        if (!ios_s17_parse_int( cfg_value, &count ) || count < 0) count = 0;
    }
    return (count >= 1 && count <= 63) ? (int)count : 0;
}

/* ---------------------------------------------------------------- Part D5 */

const char *ios_s17_syscall_name( const char *const *names, unsigned long limit, unsigned int num )
{
    if (!names || num >= limit) return NULL;
    return names[num];
}
