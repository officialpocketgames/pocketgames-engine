// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Part J, Parts F and G2: host tests of win32u-unix/w32ios_s17_pure.c: the 32-bit
// session decision, publishing the GDI handle table (the per-thread marker, master or view, the
// remembered views), the owner rules of DC attribute buckets and cache DCEs, and the GA_ROOT
// walk over a fake window tree.

#include <stdint.h>
#include <stdio.h>

#include "w32ios_s17_pure.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond ); } } while (0)

#define CHECK_EQ(a, b) do { uint64_t a_ = (uint64_t)(a), b_ = (uint64_t)(b); checks++; \
    if (a_ != b_) { failures++; printf( "FAIL %s:%d: %s == %s (0x%llx != 0x%llx)\n", \
        __FILE__, __LINE__, #a, #b, (unsigned long long)a_, (unsigned long long)b_ ); } } while (0)

/* ---------------------------------------------------------------- F2 */

static void test_session(void)
{
    /* the switch decides when it is set */
    CHECK( w32ios_s17_session_32bit( "1", 0 ) );
    CHECK( w32ios_s17_session_32bit( "yes", -1 ) );
    CHECK( !w32ios_s17_session_32bit( "0", 1 ) );
    /* otherwise the arming; a missing function is no */
    CHECK( w32ios_s17_session_32bit( NULL, 1 ) );
    CHECK( !w32ios_s17_session_32bit( NULL, 0 ) );
    CHECK( !w32ios_s17_session_32bit( NULL, -1 ) );
    CHECK( w32ios_s17_session_32bit( "", 1 ) );     /* empty counts as unset */
    CHECK( !w32ios_s17_session_32bit( "", 0 ) );
}

/* ---------------------------------------------------------------- F4 */

static void test_publish(void)
{
    int peb_a = 0, peb_b = 0;   /* only their addresses are used, as two PEBs */
    struct w32ios_s17_views views = { 0 };
    int i, a, b;
    char v[16];

    /* the marker: publish once per PEB, again when the thread's PEB changes */
    CHECK( w32ios_s17_publish_needed( NULL, &peb_a ) );
    CHECK( !w32ios_s17_publish_needed( &peb_a, &peb_a ) );
    CHECK( w32ios_s17_publish_needed( &peb_a, &peb_b ) );

    /* 64-bit: the master in every session; 32-bit: a view only in a 32-bit session */
    CHECK_EQ( w32ios_s17_publish_kind( 0, 0 ), W32IOS_S17_MASTER );
    CHECK_EQ( w32ios_s17_publish_kind( 0, 1 ), W32IOS_S17_MASTER );
    CHECK_EQ( w32ios_s17_publish_kind( 1, 1 ), W32IOS_S17_VIEW );
    CHECK_EQ( w32ios_s17_publish_kind( 1, 0 ), W32IOS_S17_MASTER );   /* switch off */

    /* views by (pid, PEB): found again, a failed one (NULL) remembered too */
    CHECK_EQ( w32ios_s17_views_find( &views, 1, &peb_a ), -1 );
    a = w32ios_s17_views_put( &views, 1, &peb_a, &v[0] );
    CHECK_EQ( w32ios_s17_views_find( &views, 1, &peb_a ), a );
    CHECK( views.slot[a].view == &v[0] );
    CHECK_EQ( w32ios_s17_views_find( &views, 2, &peb_a ), -1 );   /* same PEB, other process */
    CHECK_EQ( w32ios_s17_views_find( &views, 1, &peb_b ), -1 );
    b = w32ios_s17_views_put( &views, 2, &peb_a, NULL );
    CHECK( b != a );
    CHECK( views.slot[b].view == NULL );
    /* putting the same key again updates its slot */
    CHECK_EQ( w32ios_s17_views_put( &views, 1, &peb_a, &v[1] ), a );
    CHECK( views.slot[a].view == &v[1] );

    /* eight slots; the ninth overwrites the oldest */
    for (i = 3; i <= 8; i++) w32ios_s17_views_put( &views, i, &peb_b, &v[i] );
    for (i = 3; i <= 8; i++) CHECK( w32ios_s17_views_find( &views, i, &peb_b ) >= 0 );
    w32ios_s17_views_put( &views, 9, &peb_b, &v[9] );
    CHECK_EQ( w32ios_s17_views_find( &views, 2, &peb_a ), -1 );   /* the oldest (pid 2) */
    CHECK( w32ios_s17_views_find( &views, 1, &peb_a ) >= 0 );      /* refreshed later */
    CHECK( w32ios_s17_views_find( &views, 9, &peb_b ) >= 0 );
    w32ios_s17_views_put( &views, 10, &peb_b, &v[10] );
    CHECK_EQ( w32ios_s17_views_find( &views, 1, &peb_a ), -1 );   /* next oldest */
}

/* ---------------------------------------------------------------- F5 / F6 */

static void test_owners(void)
{
    int peb1 = 0, peb2 = 0;     /* only their addresses are used */
    struct w32ios_s17_owner p64a = { 10, &peb1, 0 }, p64b = { 11, &peb2, 0 };
    struct w32ios_s17_owner p32a = { 20, &peb1, 1 }, p32b = { 21, &peb2, 1 };
    struct w32ios_s17_owner p32a_again = { 20, &peb1, 1 };
    struct w32ios_s17_owner later_same_peb = { 22, &peb1, 1 };   /* a new process at a dead one's PEB */

    /* 64-bit processes share, as upstream */
    CHECK( w32ios_s17_owner_ok( &p64a, &p64b ) );
    CHECK( w32ios_s17_owner_ok( &p64b, &p64a ) );
    /* a 32-bit caller or owner: only its own process */
    CHECK( !w32ios_s17_owner_ok( &p64a, &p32a ) );
    CHECK( !w32ios_s17_owner_ok( &p32a, &p64a ) );
    CHECK( !w32ios_s17_owner_ok( &p32a, &p32b ) );
    CHECK( w32ios_s17_owner_ok( &p32a, &p32a_again ) );
    CHECK( !w32ios_s17_owner_ok( &later_same_peb, &p32a ) );   /* keyed on the process id too */
    CHECK( !w32ios_s17_owner_ok( &p64a, &later_same_peb ) );
}

/* ---------------------------------------------------------------- G2 */

struct fake_window { uint64_t parent; int child; };
static struct fake_window tree[16];

static uint64_t fake_parent( void *ctx, uint64_t hwnd ) { return hwnd < 16 ? tree[hwnd].parent : 0; }
static int fake_is_child( void *ctx, uint64_t hwnd ) { return hwnd < 16 && tree[hwnd].child; }

static void test_root_walk(void)
{
    /* 1: a dialog (top-level, parent 0 = the desktop); 2: a group box in it; 3: a button in 2;
     * 4: a top-level popup; 5: a WS_CHILD window whose parent can't be read (0);
     * 6, 7: a cycle of child windows */
    tree[1] = (struct fake_window){ 0, 0 };
    tree[2] = (struct fake_window){ 1, 1 };
    tree[3] = (struct fake_window){ 2, 1 };
    tree[4] = (struct fake_window){ 0, 0 };
    tree[5] = (struct fake_window){ 0, 1 };
    tree[6] = (struct fake_window){ 7, 1 };
    tree[7] = (struct fake_window){ 6, 1 };

    CHECK_EQ( w32ios_s17_root_walk( 3, fake_parent, fake_is_child, NULL, 64 ), 1 );
    CHECK_EQ( w32ios_s17_root_walk( 2, fake_parent, fake_is_child, NULL, 64 ), 1 );
    CHECK_EQ( w32ios_s17_root_walk( 1, fake_parent, fake_is_child, NULL, 64 ), 1 );   /* itself */
    CHECK_EQ( w32ios_s17_root_walk( 4, fake_parent, fake_is_child, NULL, 64 ), 4 );
    CHECK_EQ( w32ios_s17_root_walk( 5, fake_parent, fake_is_child, NULL, 64 ), 5 );   /* stops */
    /* a cycle ends after the step limit */
    {
        uint64_t r = w32ios_s17_root_walk( 6, fake_parent, fake_is_child, NULL, 64 );
        CHECK( r == 6 || r == 7 );
    }
    /* a parent that is not WS_CHILD ends the walk there, even if it has a parent */
    tree[2] = (struct fake_window){ 1, 0 };
    CHECK_EQ( w32ios_s17_root_walk( 3, fake_parent, fake_is_child, NULL, 64 ), 2 );
}

int main( void )
{
    test_session();
    test_publish();
    test_owners();
    test_root_walk();
    printf( "%d checks, %d failures\n", checks, failures );
    return failures ? 1 : 0;
}
