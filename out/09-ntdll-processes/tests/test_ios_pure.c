// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part G: host tests of the pure parts (ntdll-unix/ios_pure.c): the helper-name rule,
// the refused-spawn match, the per-PEB registry (also under concurrent use), the unix library
// symbol names, the live-children count and the clock adjustment.
//
// Build and run on any host with a C11 compiler and pthreads (run-host-tests.sh does this):
//   cc -std=c11 -Wall -pthread -I../ntdll-unix ../ntdll-unix/ios_pure.c test_ios_pure.c -o t && ./t

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ios_pure.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while (0)

#define CHECK_STR(a, b) do { checks++; if (strcmp( (a), (b) )) { failures++; \
    printf( "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b) ); } } while (0)

/* ---------- Part C5: helpers ---------- */

static void test_helper_names(void)
{
    CHECK( ios_is_helper_name( "C:\\Games\\X\\CrashReporter.exe" ) );
    CHECK( ios_is_helper_name( "crs-handler.exe" ) );
    CHECK( ios_is_helper_name( "UnityCrashHandler64.exe" ) );
    CHECK( ios_is_helper_name( "C:\\x\\BugREPORT.exe" ) );
    CHECK( ios_is_helper_name( "updatehelper.exe" ) );
    CHECK( ios_is_helper_name( "EventHandler.exe" ) );
    CHECK( !ios_is_helper_name( "C:\\Games\\Game\\Game.exe" ) );
    CHECK( !ios_is_helper_name( "C:\\Games\\Launcher.exe" ) );
    /* only the file name counts, not the folders */
    CHECK( !ios_is_helper_name( "C:\\CrashTests\\Game.exe" ) );
    CHECK( !ios_is_helper_name( "/report/helper/game.exe" ) );
    CHECK( !ios_is_helper_name( "" ) );
    CHECK( !ios_is_helper_name( NULL ) );
}

/* ---------- Part C6: refused spawns ---------- */

static void test_refused(void)
{
    CHECK_STR( ios_refused_spawn_match( "\\??\\C:\\Steam\\SteamErrorReporter.exe" ), "steamerrorreporter" );
    CHECK_STR( ios_refused_spawn_match( "C:\\x\\GLDRIVERQUERY.exe" ), "gldriverquery" );
    CHECK_STR( ios_refused_spawn_match( "C:\\x\\vulkandriverquery64.exe" ), "vulkandriverquery" );
    CHECK_STR( ios_refused_spawn_match( "steamsysinfo.exe" ), "steamsysinfo" );
    CHECK_STR( ios_refused_spawn_match( "C:\\HardwareUpdater\\run.exe" ), "hardwareupdater" );
    CHECK_STR( ios_refused_spawn_match( "UnityCrashHandler64.exe" ), "unitycrashhandler64" );
    CHECK( !ios_refused_spawn_match( "UnityCrashHandler32.exe" ) );
    CHECK( !ios_refused_spawn_match( "C:\\Games\\Game.exe" ) );
    CHECK( !ios_refused_spawn_match( NULL ) );

    CHECK( ios_is_conhost( "C:\\windows\\system32\\conhost.exe" ) );
    CHECK( ios_is_conhost( "CONHOST.EXE" ) );
    CHECK( !ios_is_conhost( "C:\\windows\\system32\\myconhost.exe" ) );
    CHECK( !ios_is_conhost( "conhost.exe.bak" ) );
}

/* ---------- Part A: unix library names ---------- */

static void test_unixlib_symbol(void)
{
    char buf[128];

    CHECK( !ios_unixlib_symbol( "winemetal.dll", 0, buf, sizeof(buf) ) );
    CHECK_STR( buf, "dxmt_winemetal_unix_call_funcs" );
    CHECK( !ios_unixlib_symbol( "WINEMETAL.DLL", 1, buf, sizeof(buf) ) );
    CHECK_STR( buf, "dxmt_winemetal_unix_call_wow64_funcs" );
    CHECK( !ios_unixlib_symbol( "C:\\windows\\system32\\win32u.dll", 0, buf, sizeof(buf) ) );
    CHECK_STR( buf, "win32u_unix_call_funcs" );
    CHECK( !ios_unixlib_symbol( "ws2_32", 1, buf, sizeof(buf) ) );
    CHECK_STR( buf, "ws2_32_unix_call_wow64_funcs" );
    CHECK( !ios_unixlib_symbol( "/x/my-lib.so", 0, buf, sizeof(buf) ) );
    CHECK_STR( buf, "my_lib_unix_call_funcs" );
    CHECK( ios_unixlib_symbol( "", 0, buf, sizeof(buf) ) );
    CHECK( ios_unixlib_symbol( ".dll", 0, buf, sizeof(buf) ) == 0 );  /* a leading dot is part of the name */
    CHECK( ios_unixlib_symbol( "winemetal.dll", 0, buf, 10 ) );      /* doesn't fit */

    CHECK( !ios_unixlib_init_symbol( "win32u.dll", buf, sizeof(buf) ) );
    CHECK_STR( buf, "win32u_unix_lib_init" );
    CHECK( !ios_unixlib_init_symbol( "winemetal.dll", buf, sizeof(buf) ) );
    CHECK_STR( buf, "dxmt_winemetal_unix_lib_init" );
}

/* ---------- Part B2: registry ---------- */

static void test_registry(void)
{
    static struct ios_registry reg;
    /* only their addresses are used: keys and values */
    static int keys[IOS_REGISTRY_SLOTS + 1], values[IOS_REGISTRY_SLOTS + 1];
    int i;

    memset( &reg, 0, sizeof(reg) );
    CHECK( ios_registry_lookup( &reg, &keys[0] ) == NULL );
    CHECK( ios_registry_publish( &reg, NULL, &values[0] ) == -1 );
    CHECK( ios_registry_lookup( &reg, NULL ) == NULL );

    CHECK( !ios_registry_publish( &reg, &keys[0], &values[0] ) );
    CHECK( ios_registry_lookup( &reg, &keys[0] ) == &values[0] );
    CHECK( ios_registry_publish( &reg, &keys[0], &values[1] ) == -1 );  /* already there */
    CHECK( ios_registry_count( &reg ) == 1 );

    /* remove, then the same key (a PEB at the same address) starts clean */
    CHECK( !ios_registry_remove( &reg, &keys[0] ) );
    CHECK( ios_registry_lookup( &reg, &keys[0] ) == NULL );
    CHECK( ios_registry_remove( &reg, &keys[0] ) == -1 );
    CHECK( !ios_registry_publish( &reg, &keys[0], &values[2] ) );
    CHECK( ios_registry_lookup( &reg, &keys[0] ) == &values[2] );
    CHECK( !ios_registry_remove( &reg, &keys[0] ) );

    /* fill every slot, one more fails, a removed slot is reused */
    for (i = 0; i < IOS_REGISTRY_SLOTS; i++) CHECK( !ios_registry_publish( &reg, &keys[i], &values[i] ) );
    CHECK( ios_registry_count( &reg ) == IOS_REGISTRY_SLOTS );
    CHECK( ios_registry_publish( &reg, &keys[IOS_REGISTRY_SLOTS], &values[0] ) == -1 );
    for (i = 0; i < IOS_REGISTRY_SLOTS; i++) CHECK( ios_registry_lookup( &reg, &keys[i] ) == &values[i] );
    CHECK( !ios_registry_remove( &reg, &keys[17] ) );
    CHECK( !ios_registry_publish( &reg, &keys[IOS_REGISTRY_SLOTS], &values[IOS_REGISTRY_SLOTS] ) );
    CHECK( ios_registry_lookup( &reg, &keys[IOS_REGISTRY_SLOTS] ) == &values[IOS_REGISTRY_SLOTS] );
    CHECK( ios_registry_lookup( &reg, &keys[17] ) == NULL );
    for (i = 0; i <= IOS_REGISTRY_SLOTS; i++) ios_registry_remove( &reg, &keys[i] );
    CHECK( ios_registry_count( &reg ) == 0 );
}

/* concurrent users: each thread publishes, looks up and removes its own keys many times while
 * a reader checks that a lookup never returns another key's value */
#define THREADS 8
#define ROUNDS  20000

static struct ios_registry shared_reg;
static int thread_keys[THREADS][4];
static int bad_lookups;

static void *registry_worker( void *arg )
{
    int t = (int)(intptr_t)arg, r, k;

    for (r = 0; r < ROUNDS; r++)
    {
        for (k = 0; k < 4; k++) ios_registry_publish( &shared_reg, &thread_keys[t][k], &thread_keys[t][k] );
        for (k = 0; k < 4; k++)
        {
            void *v = ios_registry_lookup( &shared_reg, &thread_keys[t][k] );
            if (v != &thread_keys[t][k]) __atomic_add_fetch( &bad_lookups, 1, __ATOMIC_RELAXED );
        }
        for (k = 0; k < 4; k++) ios_registry_remove( &shared_reg, &thread_keys[t][k] );
    }
    return NULL;
}

static void test_registry_threads(void)
{
    pthread_t threads[THREADS];
    int t;

    memset( &shared_reg, 0, sizeof(shared_reg) );
    for (t = 0; t < THREADS; t++) pthread_create( &threads[t], NULL, registry_worker, (void *)(intptr_t)t );
    for (t = 0; t < THREADS; t++) pthread_join( threads[t], NULL );
    CHECK( bad_lookups == 0 );
    CHECK( ios_registry_count( &shared_reg ) == 0 );
}

/* ---------- Part C5: live children ---------- */

static void test_children(void)
{
    char buf[256];
    unsigned int game, crash, launcher, svc, ids[IOS_MAX_CHILDREN];
    int i, n;

    ios_children_reset();
    CHECK( ios_children_live( buf, sizeof(buf), -1, 100.0 ) == 0 );
    CHECK_STR( buf, "" );

    game = ios_children_add( "C:\\Games\\Game\\Game.exe", 100.0 );
    crash = ios_children_add( "C:\\Games\\Game\\CrashHandler.exe", 101.0 );
    launcher = ios_children_add( "C:\\Games\\Game\\Updater.exe", 10.0 );
    svc = ios_children_add( "C:\\windows\\system32\\services.exe", 102.0 );
    CHECK( game && crash && launcher && svc );
    CHECK( game != crash && crash != launcher );

    /* helpers never count; system processes don't count once marked */
    CHECK( ios_children_live( buf, sizeof(buf), -1, 110.0 ) == 3 );
    ios_children_set_system( svc );
    n = ios_children_live( buf, sizeof(buf), -1, 110.0 );
    CHECK( n == 2 );
    CHECK( strstr( buf, "Game.exe" ) && strstr( buf, "Updater.exe" ) && !strstr( buf, "CrashHandler" ) );
    CHECK( strchr( buf, ',' ) != NULL );

    /* max_age: only children started at most max_age seconds ago */
    CHECK( ios_children_live( buf, sizeof(buf), 60.0, 110.0 ) == 1 );
    CHECK_STR( buf, "Game.exe" );
    CHECK( ios_children_live( buf, sizeof(buf), 5.0, 110.0 ) == 0 );
    CHECK_STR( buf, "" );

    /* small buffers are truncated, the count is still right */
    CHECK( ios_children_live( buf, 4, -1, 110.0 ) == 2 );
    CHECK( strlen( buf ) <= 3 );
    CHECK( ios_children_live( NULL, 0, -1, 110.0 ) == 2 );

    ios_children_remove( game );
    ios_children_remove( launcher );
    CHECK( ios_children_live( buf, sizeof(buf), -1, 110.0 ) == 0 );
    ios_children_remove( 0 );  /* untracked: nothing */

    /* the table is bounded; a full table gives 0 (not counted) */
    ios_children_reset();
    for (i = 0; i < IOS_MAX_CHILDREN; i++) CHECK( (ids[i] = ios_children_add( "a.exe", 1.0 )) != 0 );
    CHECK( ios_children_add( "b.exe", 1.0 ) == 0 );
    ios_children_remove( ids[5] );
    CHECK( ios_children_add( "b.exe", 1.0 ) != 0 );
    ios_children_reset();
}

/* ---------- Part F2 ---------- */

static void test_mhz(void)
{
    CHECK( ios_plausible_mhz( 24000000 ) == 3072 );
    CHECK( ios_plausible_mhz( 1000000000 ) == 2000 );
    CHECK( ios_plausible_mhz( 100000000 ) == 3200 );
    CHECK( ios_plausible_mhz( 0 ) == 3000 );
    CHECK( ios_plausible_mhz( 9000000000ull ) == 5000 );
}

static void test_file_part(void)
{
    CHECK_STR( ios_file_part( "C:\\a\\b.exe" ), "b.exe" );
    CHECK_STR( ios_file_part( "/a/b/c" ), "c" );
    CHECK_STR( ios_file_part( "plain" ), "plain" );
    CHECK_STR( ios_file_part( "dir\\" ), "" );
    CHECK_STR( ios_file_part( NULL ), "" );
}

/* Part C4, the third device run: wineboot.exe at the first boot is a builtin with no file in
 * system32 yet; NtCreateUserProcess starts it anyway with an NT name whose buffer is NULL and
 * whose length is whatever the stack held. Its name must come from the image path then. */
static void test_child_image_name(void)
{
    static const unsigned short nt[] = { '\\','?','?','\\','C',':','\\','a','.','e','x','e' };
    static const unsigned short path[] = { 'C',':','\\','w','b','.','e','x','e' };
    unsigned int len = 12345;
    const void *name;

    /* the bug: NULL buffer, garbage length -> the path, never the NULL buffer */
    name = ios_child_image_name( NULL, 0xbeef, path, sizeof(path), &len );
    CHECK( name == path && len == sizeof(path) );

    /* a resolved NT name wins */
    name = ios_child_image_name( nt, sizeof(nt), path, sizeof(path), &len );
    CHECK( name == nt && len == sizeof(nt) );

    /* an empty but present NT name is still the NT name */
    name = ios_child_image_name( nt, 0, path, sizeof(path), &len );
    CHECK( name == nt && len == 0 );

    /* neither: NULL and 0, so the caller converts nothing */
    len = 777;
    name = ios_child_image_name( NULL, 0xbeef, NULL, 0x55, &len );
    CHECK( name == NULL && len == 0 );
}

/* Part B, spec 19 B4 (the fifth device run's x64 session loaded aarch64-windows/ntdll.dll for an
 * x64 program: 4000000e). The loader looks ntdll up with the host's machine (ARM64). */
static void test_pe_farm_dir(void)
{
    /* an x64 (ARM64EC) program: ntdll and its other 64-bit images are the ARM64EC build */
    CHECK_STR( ios_pe_farm_dir( IOS_MACHINE_AMD64, IOS_MACHINE_ARM64 ), "/arm64ec-windows" );
    CHECK_STR( ios_pe_farm_dir( IOS_MACHINE_AMD64, IOS_MACHINE_AMD64 ), "/arm64ec-windows" );
    /* a plain ARM64 program (Wine's programs: wineboot.exe, services.exe, also in an x64
     * session, where the ARM64EC farm has no programs) */
    CHECK_STR( ios_pe_farm_dir( IOS_MACHINE_ARM64, IOS_MACHINE_ARM64 ), "/aarch64-windows" );
    /* a 32-bit program: its 64-bit ntdll is ARM64, its own images i386 */
    CHECK_STR( ios_pe_farm_dir( IOS_MACHINE_I386, IOS_MACHINE_ARM64 ), "/aarch64-windows" );
    CHECK_STR( ios_pe_farm_dir( IOS_MACHINE_I386, IOS_MACHINE_I386 ), "/i386-windows" );
    /* before the main image is known (its own lookup): by the image's machine */
    CHECK_STR( ios_pe_farm_dir( 0, IOS_MACHINE_ARM64 ), "/aarch64-windows" );
    CHECK_STR( ios_pe_farm_dir( 0, IOS_MACHINE_AMD64 ), "/arm64ec-windows" );
    CHECK_STR( ios_pe_farm_dir( 0, 0x1234 ), "" );
}

int main(void)
{
    test_pe_farm_dir();
    test_child_image_name();
    test_helper_names();
    test_refused();
    test_unixlib_symbol();
    test_registry();
    test_registry_threads();
    test_children();
    test_mhz();
    test_file_part();
    printf( "%d checks, %d failed\n", checks, failures );
    return failures ? 1 : 0;
}
