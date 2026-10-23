// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 22 Part T (Part E): the client-object ownership rule (full handle and owner) over a fake
// user handle table shared by pseudo-processes, the way patches/S22-E-window.patch uses
// w32ios_client_object_ok in get_user_handle_ptr and next_thread_user_object.

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "w32ios_pure.h"

static int checks, failures;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf( "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond ); } } while (0)

#define SLOTS 16
#define FIRST_HANDLE 0x0020
#define TO_INDEX(h) ((((h) & 0xffff) - FIRST_HANDLE) >> 1)
#define FROM_INDEX(i, gen) ((uint32_t)(((i) << 1) + FIRST_HANDLE + ((uint32_t)(gen) << 16)))
#define OTHER_PROCESS ((void *)1)

enum { OBJ_WINDOW = 1, OBJ_MENU = 2 };

struct fake_wnd { uint32_t handle; };                 /* WND: its full handle first */
struct entry { uint32_t pid, tid; uint16_t gen, type; }; /* the server's shared entry */

static struct entry entries[SLOTS];
static void *objects[SLOTS];       /* client_objects */
static uint32_t owners[SLOTS];     /* client_owners */

/* alloc_user_handle / set_user_handle_ptr */
static uint32_t server_alloc( int index, uint32_t pid, uint16_t type )
{
    entries[index].pid = pid;
    entries[index].type = type;
    entries[index].gen++;
    return FROM_INDEX( index, entries[index].gen );
}
static void set_object( uint32_t handle, void *ptr, uint32_t caller )
{
    owners[TO_INDEX( handle )] = ptr ? caller : 0;
    objects[TO_INDEX( handle )] = ptr;
}

/* get_user_handle_ptr as patched */
static void *lookup( uint32_t handle, uint16_t type, uint32_t caller )
{
    int index = TO_INDEX( handle );
    uint32_t full;
    void *ptr;

    if (index < 0 || index >= SLOTS || entries[index].type != type) return NULL;
    /* a short handle (high word 0 or 0xffff) is completed from the entry, a full one must match */
    if ((handle >> 16) && (handle >> 16) != 0xffff && (handle >> 16) != entries[index].gen) return NULL;
    full = FROM_INDEX( index, entries[index].gen );
    if (entries[index].pid != caller) return OTHER_PROCESS;
    if (!(ptr = objects[index])) return NULL;
    if (owners[index] != caller) return NULL;   /* never read another process's object */
    if (!w32ios_client_object_ok( owners[index], caller, type == OBJ_WINDOW,
                                  type == OBJ_WINDOW ? ((struct fake_wnd *)ptr)->handle : 0, full ))
        return NULL;
    return ptr;
}

/* next_thread_user_object as patched: skips what doesn't count */
static void *next_object( uint32_t caller, int *index, uint16_t type )
{
    for (; *index < SLOTS; (*index)++)
    {
        void *ptr;
        if (entries[*index].type != type || entries[*index].pid != caller) continue;
        ptr = lookup( FROM_INDEX( *index, entries[*index].gen ), type, caller );
        if (ptr && ptr != OTHER_PROCESS) { (*index)++; return ptr; }
    }
    return NULL;
}

static void test_rule(void)
{
    CHECK( w32ios_client_object_ok( 7, 7, 1, 0x10022, 0x10022 ) );
    CHECK( !w32ios_client_object_ok( 7, 8, 1, 0x10022, 0x10022 ) );    /* another process's */
    CHECK( !w32ios_client_object_ok( 0, 0, 1, 0x10022, 0x10022 ) );    /* no owner recorded */
    CHECK( !w32ios_client_object_ok( 7, 7, 1, 0x10022, 0x20022 ) );    /* an older window's object */
    CHECK( w32ios_client_object_ok( 7, 7, 0, 0x1234, 0x20022 ) );      /* not a window: owner only */
    CHECK( !w32ios_client_object_ok( 6, 7, 0, 0, 0 ) );
}

static void test_table(void)
{
    struct fake_wnd game1_main, game1_child, game2_main;
    uint32_t h1, h2, desk2, h3;
    int idx;
    void *p;

    memset( entries, 0, sizeof(entries) );
    memset( objects, 0, sizeof(objects) );
    memset( owners, 0, sizeof(owners) );

    /* game 1 (pid 0x40) makes two windows */
    h1 = server_alloc( 5, 0x40, OBJ_WINDOW );
    game1_main.handle = h1;
    set_object( h1, &game1_main, 0x40 );
    h2 = server_alloc( 6, 0x40, OBJ_WINDOW );
    game1_child.handle = h2;
    set_object( h2, &game1_child, 0x40 );
    CHECK( lookup( h1, OBJ_WINDOW, 0x40 ) == &game1_main );
    CHECK( lookup( h1 & 0xffff, OBJ_WINDOW, 0x40 ) == &game1_main );        /* short handle */
    CHECK( lookup( h1, OBJ_WINDOW, 0x50 ) == OTHER_PROCESS );

    /* game 1 ends: its objects stay in the slots (nothing of it runs to free them). The server
     * reuses slot 5 for game 2's (pid 0x50) desktop window, which has no client object. */
    desk2 = server_alloc( 5, 0x50, OBJ_WINDOW );
    CHECK( lookup( desk2, OBJ_WINDOW, 0x50 ) == NULL );                     /* not game 1's freed window */
    CHECK( lookup( desk2 & 0xffff, OBJ_WINDOW, 0x50 ) == NULL );
    CHECK( lookup( h1, OBJ_WINDOW, 0x40 ) == NULL );                        /* the old full handle is gone */

    /* game 2 makes a window in slot 6 (game 1's old child slot) */
    h3 = server_alloc( 6, 0x50, OBJ_WINDOW );
    game2_main.handle = h3;
    set_object( h3, &game2_main, 0x50 );
    CHECK( lookup( h3, OBJ_WINDOW, 0x50 ) == &game2_main );

    /* an own window whose object wasn't cleared and whose slot came back with a newer
     * generation: the handle check drops it */
    game2_main.handle = h3;
    server_alloc( 6, 0x50, OBJ_WINDOW );
    CHECK( lookup( FROM_INDEX( 6, entries[6].gen ), OBJ_WINDOW, 0x50 ) == NULL );
    entries[6].gen--;   /* back to h3's generation */

    /* enumeration of game 2's windows: only its own object, never game 1's */
    idx = 0;
    p = next_object( 0x50, &idx, OBJ_WINDOW );
    CHECK( p == &game2_main );
    CHECK( next_object( 0x50, &idx, OBJ_WINDOW ) == NULL );

    /* a menu (not a window): the owner decides */
    server_alloc( 9, 0x50, OBJ_MENU );
    set_object( FROM_INDEX( 9, entries[9].gen ), &game1_main, 0x40 );       /* an ended process's */
    CHECK( lookup( FROM_INDEX( 9, entries[9].gen ), OBJ_MENU, 0x50 ) == NULL );
    set_object( FROM_INDEX( 9, entries[9].gen ), &game2_main, 0x50 );
    CHECK( lookup( FROM_INDEX( 9, entries[9].gen ), OBJ_MENU, 0x50 ) == &game2_main );
}

int main( void )
{
    test_rule();
    test_table();
    printf( "spec 22 Part E: %d checks, %d failures\n", checks, failures );
    return failures ? 1 : 0;
}
