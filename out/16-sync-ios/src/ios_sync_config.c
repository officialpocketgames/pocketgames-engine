// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part A: which engine runs. The tuning values of Parts E and G are fixed (spec 00 Part I:
// the engine reads no settings file); the environment gives the fastsync switches, and
// PG_SYNC_INPROC with PG_SYNC_SESSION for in-process ntsync (clarification 1). Both are taken once
// per app run: objects and cells outlive a session (spec 08 clarification 1).

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ios_sync.h"

/* ---- the tuning values (spec 00 Part I: the defaults, always) ---- */

static const struct ios_sync_settings settings =
{
    .lock_spin      = 256,
    .spin_us        = 0,
    .yield_streak   = 256,
    .yield_sleep_us = 100,
    .eco            = 0,    /* the app's pg_set_eco switches it */
    .eco_qos        = IOS_ECO_QOS_UTILITY,
    .alert_spin_us  = 0,
};

const struct ios_sync_settings *ios_sync_settings(void)
{
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
    const char *mode = get( "PG_FASTSYNC" ), *peek = get( "PG_FS_POLLPEEK" );

    memset( c, 0, sizeof(*c) );
    if (env_true( mode )) c->mode = IOS_FS_ON;
    else if (mode && !strcmp( mode, "auto" )) c->mode = IOS_FS_AUTO;
    else if (mode && !strcmp( mode, "cells" )) c->mode = IOS_FS_CELLS;
    else c->mode = IOS_FS_OFF;

    c->sem = env_true( get( "PG_FASTSYNC_SEM" ));
    c->pollpeek = !(peek && (!strcmp( peek, "0" ) || !strcasecmp( peek, "off" ) || !strcasecmp( peek, "no" )));
    c->spin_us = env_uint( get( "PG_FASTSYNC_SPIN_US" ), 40, 0, 1000 );
    c->cap_us = env_uint( get( "PG_FASTSYNC_CAP_US" ), 2000, 50, 50000 );
    c->auto_reqs = env_uint( get( "PG_FS_AUTO_REQS" ), 20000, 1000, UINT32_MAX );
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

/* ---- A2: in-process ntsync (clarification 1: PG_SYNC_INPROC) ---- */

int ios_usync_engine_decide( const char *inproc, const char *session )
{
    if (!inproc || strcmp( inproc, "1" )) return 0;     /* exactly "1"; unset or anything else: off */
    if (session && !strcmp( session, "0" )) return 0;  /* an installer session (A2) wins */
    return 1;
}

/* one variable as the log names it: "NAME = value", "NAME = \"\"" (empty) or "NAME unset" */
static void describe_var( char *buf, size_t size, const char *name, const char *value )
{
    if (!value) snprintf( buf, size, "%s unset", name );
    else snprintf( buf, size, "%s = %s", name, *value ? value : "\"\"" );
}

int ios_usync_engine_describe( char *buf, size_t size, const char *inproc, const char *session )
{
    char inproc_text[96], session_text[96];
    int on = ios_usync_engine_decide( inproc, session );

    describe_var( inproc_text, sizeof(inproc_text), "PG_SYNC_INPROC", inproc );
    describe_var( session_text, sizeof(session_text), "PG_SYNC_SESSION", session );
    if (size) snprintf( buf, size, "%s (%s, %s)", on ? "on" : "off", inproc_text, session_text );
    return on;
}

static int engine;
static pthread_once_t engine_once = PTHREAD_ONCE_INIT;

/* read once, at the server's first get_inproc_device_fd, and kept for the app run */
static void decide_engine(void)
{
    char line[256];

    engine = ios_usync_engine_describe( line, sizeof(line), getenv( "PG_SYNC_INPROC" ),
                                        getenv( "PG_SYNC_SESSION" ));
    ios_sync_log( "in-process ntsync: %s", line );
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
    const char *inproc = getenv( "PG_SYNC_INPROC" ), *session = getenv( "PG_SYNC_SESSION" );
    char line[256];

    if (ios_usync_engine_decide( inproc, session ) == selected) return;
    if (__atomic_exchange_n( &logged, 1, __ATOMIC_RELAXED )) return;
    ios_usync_engine_describe( line, sizeof(line), inproc, session );
    ios_sync_log( "in-process ntsync: this session asks for %s; the app run keeps %s",
                  line, selected ? "on" : "off" );
}
