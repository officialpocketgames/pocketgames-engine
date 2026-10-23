// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/* Host tests: spec 18 Part D1 (arm64ec_redirect_ptr's decoding) and D3 (the x64 export IAT test). */

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#include "check.h"
#include "../pe/dlls/ntdll/arm64ec_x64_export_iat.c"
#include "../pe/dlls/ntdll/arm64ec_redirect.h"

static void put32( unsigned char *p, uint32_t v )
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void put64( unsigned char *p, uint64_t v )
{
    memcpy( p, &v, 8 );
}

static void put_ff25( unsigned char *image, uint32_t rva, uint32_t slot_rva )
{
    image[rva] = 0xff;
    image[rva + 1] = 0x25;
    put32( image + rva + 2, slot_rva - (rva + 6) );
}

/* D3 */

#define IMG_SIZE   0x2000
#define EXP_RVA    0x100
#define FUNCS_RVA  0x200
#define MAP_RVA    0x300
#define STUB_RVA   0x400
#define SLOT_RVA   0x800

static unsigned char img[IMG_SIZE];

static void build_image( uint32_t map_start, uint32_t map_len )
{
    memset( img, 0, sizeof(img) );
    put32( img + EXP_RVA + 20, 2 );          /* NumberOfFunctions */
    put32( img + EXP_RVA + 28, FUNCS_RVA );  /* AddressOfFunctions */
    put32( img + FUNCS_RVA, 0x1000 );        /* plain function, not a stub */
    put32( img + FUNCS_RVA + 4, STUB_RVA );
    put_ff25( img, STUB_RVA, SLOT_RVA );
    put32( img + MAP_RVA, map_start );
    put32( img + MAP_RVA + 4, map_len );
}

static int iat( uint32_t slot, uint32_t exp_rva, uint32_t exp_size, uint32_t map_count )
{
    return arm64ec_iat_slot_is_x64_export( img, IMG_SIZE, slot, exp_rva, exp_size, MAP_RVA, map_count );
}

static void test_x64_export_iat(void)
{
    /* a match */
    build_image( STUB_RVA | 2, 0x100 );
    CHECK( iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );
    CHECK( !iat( SLOT_RVA + 8, EXP_RVA, 0x40, 1 ) );  /* another slot */
    CHECK( !arm64ec_iat_slot_is_x64_export( NULL, IMG_SIZE, SLOT_RVA, EXP_RVA, 0x40, MAP_RVA, 1 ) );

    /* the stub outside every x64 code range */
    build_image( 0x500 | 2, 0x100 );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );

    /* the stub in an ARM64EC range (type 1) and an ARM64 range (type 0) */
    build_image( STUB_RVA | 1, 0x100 );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );
    build_image( STUB_RVA, 0x100 );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );

    /* a second map entry that is x64 */
    build_image( 0x500 | 1, 0x100 );
    put32( img + MAP_RVA + 8, (STUB_RVA - 0x10) | 2 );
    put32( img + MAP_RVA + 12, 0x20 );
    CHECK( iat( SLOT_RVA, EXP_RVA, 0x40, 2 ) );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );

    /* a forwarder string entry: its RVA is inside the export directory, even with stub bytes */
    build_image( (EXP_RVA + 0x20) | 2, 0x100 );
    put32( img + FUNCS_RVA + 4, EXP_RVA + 0x30 );
    put_ff25( img, EXP_RVA + 0x30, SLOT_RVA );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );
    CHECK( iat( SLOT_RVA, EXP_RVA, 0x30, 1 ) );  /* smaller directory: no longer a forwarder */

    /* slot misaligned */
    build_image( STUB_RVA | 2, 0x100 );
    put_ff25( img, STUB_RVA, SLOT_RVA + 4 );
    CHECK( !iat( SLOT_RVA + 4, EXP_RVA, 0x40, 1 ) );

    /* slot past the end */
    build_image( STUB_RVA | 2, 0x100 );
    CHECK( !iat( IMG_SIZE, EXP_RVA, 0x40, 1 ) );
    CHECK( !iat( 0xfffffff8, EXP_RVA, 0x40, 1 ) );

    /* export directory too small or past the end */
    build_image( STUB_RVA | 2, 0x100 );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 39, 1 ) );
    CHECK( iat( SLOT_RVA, EXP_RVA, 40, 1 ) );
    CHECK( !iat( SLOT_RVA, IMG_SIZE - 20, 40, 1 ) );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0xffffffff, 1 ) );

    /* address table past the end */
    build_image( STUB_RVA | 2, 0x100 );
    put32( img + EXP_RVA + 28, IMG_SIZE - 4 );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );
    build_image( STUB_RVA | 2, 0x100 );
    put32( img + EXP_RVA + 20, 0x40000000 );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );

    /* empty code map, code map past the end */
    build_image( STUB_RVA | 2, 0x100 );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 0 ) );
    CHECK( !arm64ec_iat_slot_is_x64_export( img, IMG_SIZE, SLOT_RVA, EXP_RVA, 0x40, IMG_SIZE - 4, 1 ) );
    CHECK( !arm64ec_iat_slot_is_x64_export( img, IMG_SIZE, SLOT_RVA, EXP_RVA, 0x40, MAP_RVA, 0x20000000 ) );

    /* the stub's 6 bytes crossing the range end */
    build_image( STUB_RVA | 2, 5 );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );
    build_image( STUB_RVA | 2, 6 );
    CHECK( iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );
    build_image( (STUB_RVA + 4) | 2, 0x100 );  /* starts after the stub */
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );

    /* a stub at the image's very end */
    build_image( (IMG_SIZE - 0x10) | 2, 0x10 );
    put32( img + FUNCS_RVA + 4, IMG_SIZE - 5 );
    CHECK( !iat( SLOT_RVA, EXP_RVA, 0x40, 1 ) );
}

/* D1 */

#define MOD_SIZE 0x1000

struct fake_modules
{
    struct ec_module_view views[4];
    int is_ec[4];
    int count;
    int lookups;
};

static int fake_find( void *ctx, uintptr_t addr, struct ec_module_view *view )
{
    struct fake_modules *mods = ctx;
    int i;

    mods->lookups++;
    for (i = 0; i < mods->count; i++)
    {
        if (addr < mods->views[i].base || addr - mods->views[i].base >= mods->views[i].size) continue;
        if (!mods->is_ec[i]) return 0;
        *view = mods->views[i];
        return 1;
    }
    return 0;
}

static unsigned char *map_high( size_t size )
{
    void *p = mmap( NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    return p == MAP_FAILED ? NULL : p;
}

static void put_ffwd( unsigned char *image, uint32_t rva, int32_t rel )
{
    static const unsigned char seq[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x5d, 0xe9 };
    memcpy( image + rva, seq, sizeof(seq) );
    put32( image + rva + 10, (uint32_t)rel );
}

static void test_redirect(void)
{
    static const struct ec_redirect_entry table_a[] = { { 0x100, 0x500 }, { 0x200, 0x5000 }, { 0x210, MOD_SIZE } };
    static const struct ec_redirect_entry table_b[] = { { 0x100, 0x600 } };
    unsigned char *a = map_high( MOD_SIZE ), *b = map_high( MOD_SIZE ), *c = map_high( MOD_SIZE );
    unsigned char *x = map_high( MOD_SIZE );
    struct fake_modules mods;
    uintptr_t A, B, C, X;

    CHECK( a && b && c && x );
    if (!a || !b || !c || !x) return;
    A = (uintptr_t)a; B = (uintptr_t)b; C = (uintptr_t)c; X = (uintptr_t)x;
    CHECK( A >= EC_BOUND_SLOT_MIN && B >= EC_BOUND_SLOT_MIN && C >= EC_BOUND_SLOT_MIN );

    memset( &mods, 0, sizeof(mods) );
    mods.views[0] = (struct ec_module_view){ A, MOD_SIZE, table_a, 3, NULL, 0 };
    mods.views[1] = (struct ec_module_view){ B, MOD_SIZE, table_b, 1, NULL, 0 };
    mods.views[2] = (struct ec_module_view){ C, MOD_SIZE, NULL, 0, NULL, 0 };
    mods.views[3] = (struct ec_module_view){ X, MOD_SIZE, NULL, 0, NULL, 0 };
    mods.is_ec[0] = mods.is_ec[1] = mods.is_ec[2] = 1;
    mods.is_ec[3] = 0;  /* an x64 module */
    mods.count = 4;

    /* NULL stays NULL */
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], 0, fake_find, &mods ), 0 );

    /* table hit */
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x100, fake_find, &mods ), A + 0x500 );

    /* out-of-image destinations count as no entry; the bytes there decide */
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x200, fake_find, &mods ), A + 0x200 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x210, fake_find, &mods ), A + 0x210 );
    put_ffwd( a, 0x200, 0x40 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x200, fake_find, &mods ), A + 0x200 + 14 + 0x40 );

    /* fast-forward sequence, positive and negative displacement */
    put_ffwd( a, 0x300, 0x40 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x300, fake_find, &mods ), A + 0x300 + 14 + 0x40 );
    put_ffwd( a, 0x320, -0x120 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x320, fake_find, &mods ), A + 0x320 + 14 - 0x120 );
    /* a broken sequence is unknown */
    put_ffwd( a, 0x340, 0x40 );
    a[0x340 + 7] = 0x90;
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x340, fake_find, &mods ), A + 0x340 );

    /* bound FF 25 slot into B, redirected with B's table */
    put_ff25( a, 0x400, 0x800 );
    put64( a + 0x800, B + 0x100 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x400, fake_find, &mods ), B + 0x600 );

    /* unbound slots: small RVA, zero, the stub itself */
    put_ff25( a, 0x480, 0x808 );
    put64( a + 0x808, 0x1234 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x480, fake_find, &mods ), A + 0x480 );
    put64( a + 0x808, 0 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x480, fake_find, &mods ), A + 0x480 );
    put64( a + 0x808, A + 0x480 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x480, fake_find, &mods ), A + 0x480 );
    put64( a + 0x808, EC_BOUND_SLOT_MIN - 1 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x480, fake_find, &mods ), A + 0x480 );

    /* bound into an x64 module or into nothing known: the value as is */
    put64( a + 0x808, X + 0x40 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x480, fake_find, &mods ), X + 0x40 );
    mods.count = 3;  /* X not in the list yet */
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x480, fake_find, &mods ), X + 0x40 );
    mods.count = 4;

    /* a chain of two forwarders: A -> B (FF 25) -> C (fast-forward) */
    put_ff25( a, 0x4c0, 0x810 );
    put64( a + 0x810, B + 0x300 );
    put_ff25( b, 0x300, 0x900 );
    put64( b + 0x900, C + 0x100 );
    put_ffwd( c, 0x100, 0x20 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x4c0, fake_find, &mods ), C + 0x100 + 14 + 0x20 );

    /* the slot outside the image: unknown */
    put_ff25( a, 0x600, 0x800 );
    put32( a + 0x602, 0x7ffffff0 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x600, fake_find, &mods ), A + 0x600 );

    /* an unknown pattern */
    a[0x700] = 0xc3;
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], A + 0x700, fake_find, &mods ), A + 0x700 );

    /* a pointer outside the given module is resolved with the module that holds it */
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], B + 0x100, fake_find, &mods ), B + 0x600 );
    CHECK_EQ( ec_redirect_resolve( &mods.views[0], X + 0x10, fake_find, &mods ), X + 0x10 );

    /* with a code map, only x64 ranges are decoded: native code that looks like a thunk stays */
    {
        static const struct ec_code_range map_a[] = { { 0x000 | 1, 0x300 }, { 0x300 | 2, 0x200 } };
        struct ec_module_view mapped = mods.views[0];

        mapped.code_map = map_a;
        mapped.code_map_count = 2;
        CHECK_EQ( ec_redirect_resolve( &mapped, A + 0x300, fake_find, &mods ), A + 0x300 + 14 + 0x40 );
        CHECK_EQ( ec_redirect_resolve( &mapped, A + 0x200, fake_find, &mods ), A + 0x200 );  /* ARM64EC range */
        CHECK_EQ( ec_redirect_resolve( &mapped, A + 0x4c0, fake_find, &mods ), C + 0x100 + 14 + 0x20 );
        CHECK_EQ( ec_redirect_resolve( &mapped, A + 0x600, fake_find, &mods ), A + 0x600 );  /* no range */
        put_ffwd( a, 0x4f4, 0x10 );  /* 14 bytes crossing the range end at 0x500 */
        CHECK_EQ( ec_redirect_resolve( &mapped, A + 0x4f4, fake_find, &mods ), A + 0x4f4 );
        CHECK_EQ( ec_redirect_resolve( &mapped, A + 0x100, fake_find, &mods ), A + 0x500 );  /* table first */
        /* idempotent on the native result */
        CHECK_EQ( ec_redirect_resolve( &mapped, A + 0x500, fake_find, &mods ), A + 0x500 );
    }

    /* a slot cycle stops at the depth limit without hanging */
    put_ff25( b, 0x400, 0x908 );
    put64( b + 0x908, C + 0x400 );
    put_ff25( c, 0x400, 0x908 );
    put64( c + 0x908, B + 0x400 );
    {
        uintptr_t r = ec_redirect_resolve( &mods.views[1], B + 0x400, fake_find, &mods );
        CHECK( r == B + 0x400 || r == C + 0x400 );
    }

    munmap( a, MOD_SIZE ); munmap( b, MOD_SIZE ); munmap( c, MOD_SIZE ); munmap( x, MOD_SIZE );
}

int main(void)
{
    test_x64_export_iat();
    test_redirect();
    return check_report( "arm64ec_loader" );
}
