// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part A: which engine runs. The settings file (spec 02 Part G's format, read from
// $MADEIRA_DOCS_DIR/madeira.cfg) gives inproc-sync and the tuning keys of Parts E and G; the
// environment gives the fastsync switches. Both are read once per app run: objects and cells
// outlive a session (spec 08 clarification 1).

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ios_sync.h"

/* ---- the settings file ---- */

static int is_space( char c )
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

int ios_sync_cfg_find( const char *text, size_t len, const char *key, char *out, size_t cap )
{
    size_t pos = 0, key_len = strlen( key );
    int found = 0;

    if (!text) return 0;
    if (len >= 3 && !memcmp( text, "\xef\xbb\xbf", 3 )) pos = 3;   /* UTF-8 BOM */

    while (pos < len)
    {
        size_t start = pos, end, eq, k0, k1, v0, v1;

        while (pos < len && text[pos] != '\n') pos++;
        end = pos++;   /* the line is [start, end) */

        k0 = start;
        while (k0 < end && is_space( text[k0] )) k0++;
        if (k0 == end || text[k0] == '#') continue;
        for (eq = k0; eq < end && text[eq] != '='; eq++) ;
        if (eq == end) continue;   /* no '=' */

        k1 = eq;
        while (k1 > k0 && is_space( text[k1 - 1] )) k1--;
        if (k1 - k0 != key_len || memcmp( text + k0, key, key_len )) continue;

        v0 = eq + 1;
        while (v0 < end && is_space( text[v0] )) v0++;
        v1 = end;
        while (v1 > v0 && is_space( text[v1 - 1] )) v1--;

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

int ios_sync_cfg_true( const char *value )
{
    static const char *const yes[] = { "1", "on", "true", "yes" };
    unsigned int i;

    for (i = 0; i < sizeof(yes) / sizeof(yes[0]); i++)
        if (!strcasecmp( value, yes[i] )) return 1;
    return 0;
}

/* a decimal value clamped to [min, max]; anything that isn't a number keeps the default */
static unsigned int cfg_uint( const char *text, size_t len, const char *key, unsigned int def,
                              unsigned int min, unsigned int max )
{
    char value[32], *end;
    unsigned long long v;

    if (!ios_sync_cfg_find( text, len, key, value, sizeof(value) )) return def;
    if (!isdigit( (unsigned char)value[0] )) return def;
    v = strtoull( value, &end, 10 );
    if (*end) return def;
    if (v < min) return min;
    if (v > max) return max;
    return (unsigned int)v;
}

void ios_sync_settings_parse( struct ios_sync_settings *s, const char *text, size_t len )
{
    char value[32];

    memset( s, 0, sizeof(*s) );
    s->file_found = text != NULL;
    s->inproc_sync = -1;
    if (ios_sync_cfg_find( text, len, "inproc-sync", s->inproc_raw, sizeof(s->inproc_raw) ))
        s->inproc_sync = ios_sync_cfg_true( s->inproc_raw );

    s->lock_spin      = cfg_uint( text, len, "madsync-lock-spin", 256, 0, 100000 );
    s->spin_us        = cfg_uint( text, len, "madsync-spin-us", 0, 0, 200 );
    s->yield_streak   = cfg_uint( text, len, "yield-streak", 256, 0, 1000000 );
    s->yield_sleep_us = cfg_uint( text, len, "yield-sleep-us", 100, 0, 100000 );
    s->alert_spin_us  = cfg_uint( text, len, "alert-spin-us", 0, 0, 200 );

    s->eco = ios_sync_cfg_find( text, len, "eco", value, sizeof(value) ) && ios_sync_cfg_true( value );
    s->eco_qos = IOS_ECO_QOS_UTILITY;
    if (ios_sync_cfg_find( text, len, "eco-qos", value, sizeof(value) ))
    {
        if (!strcmp( value, "background" )) s->eco_qos = IOS_ECO_QOS_BACKGROUND;
        else if (!strcmp( value, "initiated" )) s->eco_qos = IOS_ECO_QOS_INITIATED;
    }
}

static struct ios_sync_settings settings;
static pthread_once_t settings_once = PTHREAD_ONCE_INIT;

static void read_settings(void)
{
    const char *docs = getenv( "MADEIRA_DOCS_DIR" );
    char path[1024], *text = NULL;
    long size = 0;
    FILE *file = NULL;

    if (docs && *docs && snprintf( path, sizeof(path), "%s/madeira.cfg", docs ) < (int)sizeof(path))
        file = fopen( path, "rb" );
    if (file)
    {
        if (!fseek( file, 0, SEEK_END ) && (size = ftell( file )) >= 0 && size <= (1 << 20) &&
            !fseek( file, 0, SEEK_SET ) && (text = malloc( size + 1 )))
        {
            size = fread( text, 1, size, file );
            text[size] = 0;
        }
        else size = 0;
        fclose( file );
        if (!text) text = calloc( 1, 1 );   /* found, but empty or unreadable */
    }
    ios_sync_settings_parse( &settings, text, text ? size : 0 );
    free( text );
}

const struct ios_sync_settings *ios_sync_settings(void)
{
    pthread_once( &settings_once, read_settings );
    return &settings;
}

/* ---- the fastsync switches ---- */

static int env_true( const char *v )
{
    return v && (!strcmp( v, "1" ) || !strcasecmp( v, "on" ) || !strcasecmp( v, "yes" ));
}

static unsigned int env_uint( const char *v, unsigned int def, unsigned int min, unsigned int max )
{
    unsigned long long n;
    char *end;

    if (!v || !isdigit( (unsigned char)*v )) return def;
    n = strtoull( v, &end, 10 );
    if (*end) return def;
    if (n < min) return min;
    if (n > max) return max;
    return (unsigned int)n;
}

void ios_fs_config_parse( struct ios_fs_config *c, ios_getenv_fn get )
{
    const char *mode = get( "MADEIRA_FASTSYNC" ), *peek = get( "MADEIRA_FS_POLLPEEK" );

    memset( c, 0, sizeof(*c) );
    if (env_true( mode )) c->mode = IOS_FS_ON;
    else if (mode && !strcmp( mode, "auto" )) c->mode = IOS_FS_AUTO;
    else if (mode && !strcmp( mode, "cells" )) c->mode = IOS_FS_CELLS;
    else c->mode = IOS_FS_OFF;

    c->sem = env_true( get( "MADEIRA_FASTSYNC_SEM" ));
    c->pollpeek = !(peek && (!strcmp( peek, "0" ) || !strcasecmp( peek, "off" ) || !strcasecmp( peek, "no" )));
    c->spin_us = env_uint( get( "MADEIRA_FASTSYNC_SPIN_US" ), 40, 0, 1000 );
    c->cap_us = env_uint( get( "MADEIRA_FASTSYNC_CAP_US" ), 2000, 50, 50000 );
    c->auto_reqs = env_uint( get( "MADEIRA_FS_AUTO_REQS" ), 20000, 1000, UINT32_MAX );
}

static const char *libc_getenv( const char *name )
{
    return getenv( name );
}

static struct ios_fs_config fs_config;
static pthread_once_t fs_once = PTHREAD_ONCE_INIT;

static void read_fs_config(void)
{
    ios_fs_config_parse( &fs_config, libc_getenv );
}

const struct ios_fs_config *ios_fs_config(void)
{
    pthread_once( &fs_once, read_fs_config );
    return &fs_config;
}

void ios_fs_config_recheck(void)
{
    static int logged;
    struct ios_fs_config now;
    const struct ios_fs_config *first = ios_fs_config();

    ios_fs_config_parse( &now, libc_getenv );
    if ((now.mode != first->mode || now.sem != first->sem) &&
        !__atomic_exchange_n( &logged, 1, __ATOMIC_RELAXED ))
        ios_sync_log( "fastsync: this session asks for mode %d, semaphores %d; the app run keeps "
                      "mode %d, semaphores %d", now.mode, now.sem, first->mode, first->sem );
}

/* ---- A2: in-process ntsync ---- */

int ios_usync_engine_decide( int inproc_sync, const char *madsync_session )
{
    if (inproc_sync != 1) return 0;
    if (madsync_session && !strcmp( madsync_session, "0" )) return 0;
    return 1;
}

static int engine;
static pthread_once_t engine_once = PTHREAD_ONCE_INIT;

static void decide_engine(void)
{
    const struct ios_sync_settings *s = ios_sync_settings();
    const char *session = getenv( "MADEIRA_MADSYNC_SESSION" );

    engine = ios_usync_engine_decide( s->inproc_sync, session );
    ios_sync_log( "in-process ntsync: inproc-sync = %s (settings file %s), MADEIRA_MADSYNC_SESSION %s%s -> %s",
                  s->inproc_sync >= 0 ? s->inproc_raw : "unset", s->file_found ? "found" : "not found",
                  session ? "= " : "unset", session ? session : "", engine ? "on" : "off" );
}

int ios_usync_engine_selected(void)
{
    pthread_once( &engine_once, decide_engine );
    return engine;
}

void ios_usync_engine_recheck(void)
{
    static int logged;
    int selected = ios_usync_engine_selected();
    int now = ios_usync_engine_decide( ios_sync_settings()->inproc_sync, getenv( "MADEIRA_MADSYNC_SESSION" ));

    if (now != selected && !__atomic_exchange_n( &logged, 1, __ATOMIC_RELAXED ))
        ios_sync_log( "in-process ntsync: this session asks for %s; the app run keeps %s",
                      now ? "on" : "off", selected ? "on" : "off" );
}
