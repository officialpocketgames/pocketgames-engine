// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part J4: Part A's engine choice (PG_SYNC_INPROC x PG_SYNC_SESSION, clarification 1) and
// the fastsync switches; Part G's yield streak and eco generation.

#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ios_sync.h"
#include "check.h"

/* spec 00 Part I: the defaults, always */
static void test_settings(void)
{
    const struct ios_sync_settings *s = ios_sync_settings();

    CHECK_EQ( s->lock_spin, 256 );
    CHECK_EQ( s->spin_us, 0 );
    CHECK_EQ( s->yield_streak, 256 );
    CHECK_EQ( s->yield_sleep_us, 100 );
    CHECK_EQ( s->eco, 0 );
    CHECK_EQ( s->eco_qos, IOS_ECO_QOS_UTILITY );
    CHECK_EQ( s->alert_spin_us, 0 );
}

/* clarification 1: the environment pairs (PG_SYNC_INPROC, PG_SYNC_SESSION; NULL = unset) */
static const struct
{
    const char *inproc, *session;
    int         on;
    const char *line;
} engine_pairs[] =
{
    /* 1 x unset / 0 / 1 */
    { "1", NULL, 1, "on (PG_SYNC_INPROC = 1, PG_SYNC_SESSION unset)" },
    { "1", "0",  0, "off (PG_SYNC_INPROC = 1, PG_SYNC_SESSION = 0)" },
    { "1", "1",  1, "on (PG_SYNC_INPROC = 1, PG_SYNC_SESSION = 1)" },
    /* 0, yes, empty and unset x unset */
    { "0",  NULL, 0, "off (PG_SYNC_INPROC = 0, PG_SYNC_SESSION unset)" },
    { "yes", NULL, 0, "off (PG_SYNC_INPROC = yes, PG_SYNC_SESSION unset)" },
    { "",   NULL, 0, "off (PG_SYNC_INPROC = \"\", PG_SYNC_SESSION unset)" },
    { NULL, NULL, 0, "off (PG_SYNC_INPROC unset, PG_SYNC_SESSION unset)" },
};

static void test_engine_decide(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(engine_pairs) / sizeof(engine_pairs[0]); i++)
        CHECK_EQ( ios_usync_engine_decide( engine_pairs[i].inproc, engine_pairs[i].session ),
                  engine_pairs[i].on );

    /* exactly "1" selects it, exactly "0" in PG_SYNC_SESSION keeps it off */
    CHECK( !ios_usync_engine_decide( "on", NULL ));
    CHECK( !ios_usync_engine_decide( "YES", NULL ));
    CHECK( !ios_usync_engine_decide( "true", NULL ));
    CHECK( !ios_usync_engine_decide( "01", NULL ));
    CHECK( !ios_usync_engine_decide( "1 ", NULL ));
    CHECK( !ios_usync_engine_decide( " 1", NULL ));
    CHECK( !ios_usync_engine_decide( "2", NULL ));
    CHECK( ios_usync_engine_decide( "1", "" ));
    CHECK( ios_usync_engine_decide( "1", "00" ));
    CHECK( ios_usync_engine_decide( "1", "no" ));
    CHECK( !ios_usync_engine_decide( NULL, "0" ));
    CHECK( !ios_usync_engine_decide( NULL, "1" ));
    CHECK( !ios_usync_engine_decide( "0", "1" ));
}

/* A2's log line names both variables and the result */
static void test_engine_describe(void)
{
    char line[256], small[8];
    unsigned int i;

    for (i = 0; i < sizeof(engine_pairs) / sizeof(engine_pairs[0]); i++)
    {
        memset( line, 'x', sizeof(line) );
        CHECK_EQ( ios_usync_engine_describe( line, sizeof(line), engine_pairs[i].inproc,
                                             engine_pairs[i].session ), engine_pairs[i].on );
        CHECK( !strcmp( line, engine_pairs[i].line ));
        if (strcmp( line, engine_pairs[i].line )) fprintf( stderr, "  got \"%s\"\n", line );
    }
    /* a short buffer is cut and terminated; the decision is the same */
    CHECK_EQ( ios_usync_engine_describe( small, sizeof(small), "1", NULL ), 1 );
    CHECK( !strcmp( small, "on (PG_" ));
    CHECK_EQ( ios_usync_engine_describe( NULL, 0, "1", "0" ), 0 );
}

/* ---- one app run per child process: the choice is made once per process ---- */

static int capture_fd = -1, saved_stderr = -1;

/* the library logs to fd 2: send it to a file until capture_end */
static void capture_begin(void)
{
    char path[] = "/tmp/ios_sync_logXXXXXX";

    fflush( stderr );
    capture_fd = mkstemp( path );
    CHECK( capture_fd >= 0 );
    if (capture_fd < 0) return;
    unlink( path );
    saved_stderr = dup( 2 );
    dup2( capture_fd, 2 );
}

/* what was logged since capture_begin */
static const char *capture_end(void)
{
    static char text[1024];
    ssize_t n;

    text[0] = 0;
    if (capture_fd < 0) return text;
    dup2( saved_stderr, 2 );
    close( saved_stderr );
    n = pread( capture_fd, text, sizeof(text) - 1, 0 );
    close( capture_fd );
    capture_fd = -1;
    if (n > 0) text[n] = 0;
    return text;
}

static void check_log( const char *got, const char *expect )
{
    CHECK( !strcmp( got, expect ));
    if (strcmp( got, expect )) fprintf( stderr, "  logged \"%s\"\n  expected \"%s\"\n", got, expect );
}

static void set_or_unset( const char *name, const char *value )
{
    if (value) setenv( name, value, 1 );
    else unsetenv( name );
}

static void in_child( void (*run)(void) )
{
    pid_t pid;
    int status = 0;

    fflush( stdout );
    fflush( stderr );
    pid = fork();
    CHECK( pid >= 0 );
    if (pid < 0) return;
    if (!pid)
    {
        check_failures = 0;
        run();
        fflush( stderr );
        _exit( check_failures ? 1 : 0 );
    }
    CHECK_EQ( waitpid( pid, &status, 0 ), pid );
    CHECK( WIFEXITED( status ) && WEXITSTATUS( status ) == 0 );
}

/* the pair the child of test_engine_run uses */
static unsigned int run_pair;

/* the server's first get_inproc_device_fd decides and logs one line (A2) */
static void run_engine_pair(void)
{
    char expect[300];

    set_or_unset( "PG_SYNC_INPROC", engine_pairs[run_pair].inproc );
    set_or_unset( "PG_SYNC_SESSION", engine_pairs[run_pair].session );
    capture_begin();
    CHECK_EQ( ios_usync_engine_selected(), engine_pairs[run_pair].on );
    CHECK_EQ( ios_usync_engine_selected(), engine_pairs[run_pair].on );   /* asked again: no line */
    ios_usync_engine_recheck();   /* the same session: same answer, nothing logged */
    snprintf( expect, sizeof(expect), "sync: in-process ntsync: %s\n", engine_pairs[run_pair].line );
    check_log( capture_end(), expect );
}

static void test_engine_run(void)
{
    for (run_pair = 0; run_pair < sizeof(engine_pairs) / sizeof(engine_pairs[0]); run_pair++)
        in_child( run_engine_pair );
}

/* on for the app run: a later installer session (PG_SYNC_SESSION=0) is logged once, ignored */
static void run_kept_on(void)
{
    setenv( "PG_SYNC_INPROC", "1", 1 );
    unsetenv( "PG_SYNC_SESSION" );
    capture_begin();
    CHECK( ios_usync_engine_selected() );
    check_log( capture_end(), "sync: in-process ntsync: on (PG_SYNC_INPROC = 1, PG_SYNC_SESSION unset)\n" );

    /* the second session: PG_SYNC_SESSION = 1 gives the same answer */
    setenv( "PG_SYNC_SESSION", "1", 1 );
    capture_begin();
    ios_usync_engine_recheck();
    CHECK( ios_usync_engine_selected() );
    check_log( capture_end(), "" );

    /* the third: an installer session asks for off */
    setenv( "PG_SYNC_SESSION", "0", 1 );
    capture_begin();
    ios_usync_engine_recheck();
    CHECK( ios_usync_engine_selected() );
    check_log( capture_end(), "sync: in-process ntsync: this session asks for off (PG_SYNC_INPROC = 1, "
                              "PG_SYNC_SESSION = 0); the app run keeps on\n" );

    /* once per app run */
    unsetenv( "PG_SYNC_INPROC" );
    unsetenv( "PG_SYNC_SESSION" );
    capture_begin();
    ios_usync_engine_recheck();
    CHECK( ios_usync_engine_selected() );
    check_log( capture_end(), "" );
}

/* off for the app run (the store build: nothing sets PG_SYNC_INPROC); a settings file where
 * earlier builds looked for one changes nothing (spec 00 Part I); a later session's
 * PG_SYNC_INPROC=1 is logged once, ignored */
static void run_kept_off(void)
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
    unsetenv( "PG_SYNC_INPROC" );
    unsetenv( "PG_SYNC_SESSION" );

    capture_begin();
    CHECK_EQ( ios_sync_settings()->yield_streak, 256 );
    CHECK_EQ( ios_sync_settings()->eco_qos, IOS_ECO_QOS_UTILITY );
    CHECK( !ios_usync_engine_selected() );
    check_log( capture_end(), "sync: in-process ntsync: off (PG_SYNC_INPROC unset, PG_SYNC_SESSION unset)\n" );

    /* a later session asks for the same: nothing */
    setenv( "PG_SYNC_INPROC", "yes", 1 );
    capture_begin();
    ios_usync_engine_recheck();
    CHECK( !ios_usync_engine_selected() );
    check_log( capture_end(), "" );

    /* a later session asks for on: logged once, the app run keeps off */
    setenv( "PG_SYNC_INPROC", "1", 1 );
    capture_begin();
    ios_usync_engine_recheck();
    CHECK( !ios_usync_engine_selected() );
    ios_usync_engine_recheck();
    CHECK( !ios_usync_engine_selected() );
    check_log( capture_end(), "sync: in-process ntsync: this session asks for on (PG_SYNC_INPROC = 1, "
                              "PG_SYNC_SESSION unset); the app run keeps off\n" );
    remove( path );
    rmdir( dir );
}

static void test_engine_once(void)
{
    in_child( run_kept_on );
    in_child( run_kept_off );
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

    /* the thread's own state with the defaults (streak 256, 100 us) */
    for (i = 0; i < 257; i++) ios_yield_decide();
    CHECK_EQ( ios_yield_decide(), 100 );
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
    CHECK_EQ( ios_eco_last_class, IOS_QOS_UTILITY );   /* the default eco-qos */
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
    test_settings();
    test_engine_decide();
    test_engine_describe();
    test_engine_run();      /* before anything here decides: the children inherit the state */
    test_engine_once();
    test_fs_config();
    test_yield();
    test_eco();
    return check_done( "test_config_pace" );
}
