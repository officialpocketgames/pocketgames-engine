// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part J4: Part A's engine choice (settings file x environment) and the fastsync
// switches; Part G's yield streak and eco generation.

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ios_sync.h"
#include "check.h"

static void test_cfg_find(void)
{
    static const char text[] =
        "\xef\xbb\xbf# comment = no\n"
        "  inproc-sync =  yes  \r\n"
        "no equals here\n"
        "eco=1\n"
        "env.X = a=b\n"
        "eco = 0\n"
        "   # inproc-sync = 0\n"
        "empty =\n";
    char v[32];

    CHECK( ios_sync_cfg_find( text, strlen( text ), "inproc-sync", v, sizeof(v) ));
    CHECK( !strcmp( v, "yes" ));
    CHECK( ios_sync_cfg_find( text, strlen( text ), "eco", v, sizeof(v) ));
    CHECK( !strcmp( v, "0" ));   /* the last occurrence wins */
    CHECK( ios_sync_cfg_find( text, strlen( text ), "env.X", v, sizeof(v) ));
    CHECK( !strcmp( v, "a=b" ));
    CHECK( ios_sync_cfg_find( text, strlen( text ), "empty", v, sizeof(v) ));
    CHECK( !strcmp( v, "" ));
    CHECK( !ios_sync_cfg_find( text, strlen( text ), "# comment", v, sizeof(v) ));
    CHECK( !ios_sync_cfg_find( text, strlen( text ), "no equals here", v, sizeof(v) ));
    CHECK( !ios_sync_cfg_find( NULL, 0, "eco", v, sizeof(v) ));
    /* truncation */
    CHECK( ios_sync_cfg_find( text, strlen( text ), "inproc-sync", v, 3 ));
    CHECK( !strcmp( v, "ye" ));

    CHECK( ios_sync_cfg_true( "1" ) && ios_sync_cfg_true( "on" ) && ios_sync_cfg_true( "TRUE" ) &&
           ios_sync_cfg_true( "Yes" ));
    CHECK( !ios_sync_cfg_true( "0" ) && !ios_sync_cfg_true( "off" ) && !ios_sync_cfg_true( "" ) &&
           !ios_sync_cfg_true( "2" ));
}

static void parse_str( struct ios_sync_settings *s, const char *text )
{
    ios_sync_settings_parse( s, text, strlen( text ));
}

static void test_settings(void)
{
    static const char text[] =
        "madsync-lock-spin = 999999\n"
        "madsync-spin-us = 50\n"
        "yield-streak = 10\n"
        "yield-sleep-us = 0\n"
        "alert-spin-us = 300\n"
        "eco = on\n"
        "eco-qos = background\n";
    struct ios_sync_settings s;

    ios_sync_settings_parse( &s, NULL, 0 );
    CHECK( !s.file_found );
    CHECK_EQ( s.inproc_sync, -1 );
    CHECK_EQ( s.lock_spin, 256 );
    CHECK_EQ( s.spin_us, 0 );
    CHECK_EQ( s.yield_streak, 256 );
    CHECK_EQ( s.yield_sleep_us, 100 );
    CHECK_EQ( s.eco, 0 );
    CHECK_EQ( s.eco_qos, IOS_ECO_QOS_UTILITY );
    CHECK_EQ( s.alert_spin_us, 0 );

    ios_sync_settings_parse( &s, text, strlen( text ));
    CHECK( s.file_found );
    CHECK_EQ( s.inproc_sync, -1 );
    CHECK_EQ( s.lock_spin, 100000 );   /* clamped */
    CHECK_EQ( s.spin_us, 50 );
    CHECK_EQ( s.yield_streak, 10 );
    CHECK_EQ( s.yield_sleep_us, 0 );
    CHECK_EQ( s.alert_spin_us, 200 );
    CHECK_EQ( s.eco, 1 );
    CHECK_EQ( s.eco_qos, IOS_ECO_QOS_BACKGROUND );

    parse_str( &s, "eco-qos = initiated\ninproc-sync = 0\nmadsync-lock-spin = x\n" );
    CHECK_EQ( s.eco_qos, IOS_ECO_QOS_INITIATED );
    CHECK_EQ( s.inproc_sync, 0 );
    CHECK_EQ( s.lock_spin, 256 );   /* not a number: the default */
    parse_str( &s, "eco-qos = whatever\n" );
    CHECK_EQ( s.eco_qos, IOS_ECO_QOS_UTILITY );
}

static void test_engine_decide(void)
{
    /* settings x environment */
    CHECK( !ios_usync_engine_decide( -1, NULL ));
    CHECK( !ios_usync_engine_decide( 0, NULL ));
    CHECK( ios_usync_engine_decide( 1, NULL ));
    CHECK( ios_usync_engine_decide( 1, "1" ));
    CHECK( ios_usync_engine_decide( 1, "" ));
    CHECK( ios_usync_engine_decide( 1, "00" ));   /* only exactly "0" */
    CHECK( !ios_usync_engine_decide( 1, "0" ));
    CHECK( !ios_usync_engine_decide( 0, "1" ));
    CHECK( !ios_usync_engine_decide( -1, "0" ));
}

static const char *fake_env[16][2];

static const char *fake_getenv( const char *name )
{
    int i;
    for (i = 0; fake_env[i][0]; i++) if (!strcmp( fake_env[i][0], name )) return fake_env[i][1];
    return NULL;
}

static void set_env( int i, const char *name, const char *value )
{
    fake_env[i][0] = name;
    fake_env[i][1] = value;
    fake_env[i + 1][0] = NULL;
}

static void test_fs_config(void)
{
    static const struct { const char *value; int mode; } modes[] =
    {
        { NULL, IOS_FS_OFF }, { "1", IOS_FS_ON }, { "on", IOS_FS_ON }, { "yes", IOS_FS_ON },
        { "auto", IOS_FS_AUTO }, { "cells", IOS_FS_CELLS }, { "0", IOS_FS_OFF }, { "2", IOS_FS_OFF },
        { "AUTO", IOS_FS_OFF }, { "", IOS_FS_OFF },
    };
    struct ios_fs_config c;
    unsigned int i;

    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
    {
        fake_env[0][0] = NULL;
        if (modes[i].value) set_env( 0, "PG_FASTSYNC", modes[i].value );
        ios_fs_config_parse( &c, fake_getenv );
        CHECK_EQ( c.mode, modes[i].mode );
    }

    fake_env[0][0] = NULL;
    ios_fs_config_parse( &c, fake_getenv );
    CHECK_EQ( c.sem, 0 );
    CHECK_EQ( c.pollpeek, 1 );
    CHECK_EQ( c.spin_us, 40 );
    CHECK_EQ( c.cap_us, 2000 );
    CHECK_EQ( c.auto_reqs, 20000 );

    set_env( 0, "PG_FASTSYNC_SEM", "on" );
    set_env( 1, "PG_FS_POLLPEEK", "off" );
    set_env( 2, "PG_FASTSYNC_SPIN_US", "5000" );
    set_env( 3, "PG_FASTSYNC_CAP_US", "10" );
    set_env( 4, "PG_FS_AUTO_REQS", "5" );
    ios_fs_config_parse( &c, fake_getenv );
    CHECK_EQ( c.sem, 1 );
    CHECK_EQ( c.pollpeek, 0 );
    CHECK_EQ( c.spin_us, 1000 );
    CHECK_EQ( c.cap_us, 50 );
    CHECK_EQ( c.auto_reqs, 1000 );

    set_env( 0, "PG_FASTSYNC_SPIN_US", "0" );
    set_env( 1, "PG_FASTSYNC_CAP_US", "99999" );
    set_env( 2, "PG_FS_POLLPEEK", "no" );
    ios_fs_config_parse( &c, fake_getenv );
    CHECK_EQ( c.spin_us, 0 );
    CHECK_EQ( c.cap_us, 50000 );
    CHECK_EQ( c.pollpeek, 0 );
    set_env( 0, "PG_FS_POLLPEEK", "1" );
    ios_fs_config_parse( &c, fake_getenv );
    CHECK_EQ( c.pollpeek, 1 );
}

/* the real file through $PG_DOCS_DIR, read once */
static void test_settings_file(void)
{
    char dir[] = "/tmp/ios_sync_cfgXXXXXX", path[64];
    FILE *f;

    CHECK( mkdtemp( dir ) != NULL );
    snprintf( path, sizeof(path), "%s/pocketgames.cfg", dir );
    CHECK( (f = fopen( path, "w" )) != NULL );
    if (!f) return;
    fputs( "inproc-sync = yes\neco-qos = background\nyield-streak = 3\nyield-sleep-us = 50\n", f );
    fclose( f );
    setenv( "PG_DOCS_DIR", dir, 1 );
    unsetenv( "PG_MADSYNC_SESSION" );

    CHECK( ios_sync_settings()->file_found );
    CHECK_EQ( ios_sync_settings()->inproc_sync, 1 );
    CHECK( ios_usync_engine_selected() );
    /* once per app run: a later change is not seen */
    setenv( "PG_MADSYNC_SESSION", "0", 1 );
    CHECK( ios_usync_engine_selected() );
    ios_usync_engine_recheck();   /* logged once ... */
    ios_usync_engine_recheck();   /* ... not twice */
    CHECK( ios_usync_engine_selected() );
    remove( path );
    rmdir( dir );
}

static void test_yield(void)
{
    struct ios_yield_state st = { 0, 0 };
    uint64_t t = 1000000000ull;
    unsigned int i;

    /* 3 quick yields are fine, the 4th one past a streak of 3 sleeps */
    CHECK_EQ( ios_yield_step( &st, t, 3, 100 ), 0 );               /* streak 0 */
    for (i = 1; i <= 3; i++) CHECK_EQ( ios_yield_step( &st, t += 1000, 3, 100 ), 0 );
    CHECK_EQ( st.streak, 3 );
    CHECK_EQ( ios_yield_step( &st, t += 1999999, 3, 100 ), 100 );  /* just under 2 ms */
    CHECK_EQ( ios_yield_step( &st, t += 100000, 3, 100 ), 100 );
    /* a gap of 2 ms ends the streak */
    CHECK_EQ( ios_yield_step( &st, t += 2000000, 3, 100 ), 0 );
    CHECK_EQ( st.streak, 0 );
    /* sleep 0 = never */
    for (i = 0; i < 10; i++) CHECK_EQ( ios_yield_step( &st, t += 10, 3, 0 ), 0 );

    /* the thread's own state through the settings of the file above (streak 3, 50 us) */
    for (i = 0; i < 4; i++) ios_yield_decide();
    CHECK_EQ( ios_yield_decide(), 50 );
}

static void test_eco(void)
{
    unsigned int applied = 0, gen;

    CHECK_EQ( ios_eco_qos_class( 0, IOS_ECO_QOS_BACKGROUND ), IOS_QOS_USER_INTERACTIVE );
    CHECK_EQ( ios_eco_qos_class( 1, IOS_ECO_QOS_UTILITY ), IOS_QOS_UTILITY );
    CHECK_EQ( ios_eco_qos_class( 1, IOS_ECO_QOS_BACKGROUND ), IOS_QOS_BACKGROUND );
    CHECK_EQ( ios_eco_qos_class( 1, IOS_ECO_QOS_INITIATED ), IOS_QOS_USER_INITIATED );

    /* a new thread with eco off has nothing to do; with eco on it applies */
    CHECK( !ios_eco_must_apply( &applied, 1, 0 ));
    CHECK_EQ( applied, 1 );
    CHECK( !ios_eco_must_apply( &applied, 1, 0 ));
    CHECK( ios_eco_must_apply( &applied, 2, 1 ));
    CHECK( !ios_eco_must_apply( &applied, 2, 1 ));
    CHECK( ios_eco_must_apply( &applied, 3, 0 ));   /* back to interactive */
    applied = 0;
    CHECK( ios_eco_must_apply( &applied, 5, 1 ));

    /* the generation starts at 1 and moves on every switch */
    CHECK( !pg_get_eco() );
    gen = ios_eco_generation();
    CHECK_EQ( gen, 1 );
    ios_eco_apply();
    CHECK_EQ( ios_eco_last_class, -1 );   /* eco never on: no call */
    pg_set_eco( 1 );
    CHECK( pg_get_eco() );
    CHECK_EQ( ios_eco_generation(), gen + 1 );
    ios_eco_apply();
    CHECK_EQ( ios_eco_last_class, IOS_QOS_BACKGROUND );   /* eco-qos of the file */
    ios_eco_last_class = -1;
    ios_eco_apply();
    CHECK_EQ( ios_eco_last_class, -1 );   /* same generation: nothing */
    pg_set_eco( 0 );
    CHECK_EQ( ios_eco_generation(), gen + 2 );
    ios_eco_apply();
    CHECK_EQ( ios_eco_last_class, IOS_QOS_USER_INTERACTIVE );
}

int main(void)
{
    test_cfg_find();
    test_settings();
    test_engine_decide();
    test_fs_config();
    test_settings_file();
    test_yield();
    test_eco();
    return check_done( "test_config_pace" );
}
