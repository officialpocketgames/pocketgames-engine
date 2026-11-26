// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 clarification 13: host test of gl4es/src/gl/gl4es_ios_pixels.c, glGetTexImage's pure
// parts: which stored formats are read directly, the row sizes with GL_PACK_ALIGNMENT, the
// conversion from RGBA8 to every format/type pair wined3d's DirectDraw and Direct3D read-backs
// use (values checked as numbers, so the host's byte order doesn't matter), and the once-per-format
// gate of the failure log.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gl4es_ios_pixels.h"

#define RED_ 0x1903
#define ALPHA_ 0x1906
#define RGB_ 0x1907
#define RGBA_ 0x1908
#define LUM_ 0x1909
#define LUMA_ 0x190A
#define BGR_ 0x80E0
#define BGRA_ 0x80E1
#define RG_ 0x8227
#define UB_ 0x1401
#define U4444 0x8033
#define U5551 0x8034
#define U8888 0x8035
#define U565 0x8363
#define U565R 0x8364
#define U4444R 0x8365
#define U1555R 0x8366
#define U8888R 0x8367
#define FLOAT_ 0x1406

static int checks, failures;

static void check( int ok, const char *what )
{
    checks++;
    if (!ok)
    {
        failures++;
        printf( "FAIL: %s\n", what );
    }
}

/* a 2 x 2 RGBA8 image: red opaque, green half alpha, blue transparent, white opaque */
static const unsigned char src[16] = { 255, 0, 0, 255,  0, 255, 0, 128,  0, 0, 255, 0,  255, 255, 255, 255 };

static uint16_t u16( const unsigned char *p ) { uint16_t v; memcpy( &v, p, 2 ); return v; }
static uint32_t u32( const unsigned char *p ) { uint32_t v; memcpy( &v, p, 4 ); return v; }

/* converts src (2 x 2, align 1) to format/type and checks the four 16-bit values */
static void check16( unsigned int format, unsigned int type, uint16_t a, uint16_t b, uint16_t c, uint16_t d,
                     const char *what )
{
    unsigned char out[8];
    char msg[160];
    memset( out, 0, sizeof(out) );
    check( gl4es_ios_from_rgba8( src, 2, 2, format, type, 1, out ) == 1, what );
    snprintf( msg, sizeof(msg), "%s: %04x %04x %04x %04x", what, u16( out ), u16( out + 2 ), u16( out + 4 ), u16( out + 6 ) );
    check( u16( out ) == a && u16( out + 2 ) == b && u16( out + 4 ) == c && u16( out + 6 ) == d, msg );
}

static void check32( unsigned int format, unsigned int type, uint32_t a, uint32_t b, uint32_t c, uint32_t d,
                     const char *what )
{
    unsigned char out[16];
    char msg[200];
    memset( out, 0, sizeof(out) );
    check( gl4es_ios_from_rgba8( src, 2, 2, format, type, 1, out ) == 1, what );
    snprintf( msg, sizeof(msg), "%s: %08x %08x %08x %08x", what, u32( out ), u32( out + 4 ), u32( out + 8 ), u32( out + 12 ) );
    check( u32( out ) == a && u32( out + 4 ) == b && u32( out + 8 ) == c && u32( out + 12 ) == d, msg );
}

static void check8( unsigned int format, const unsigned char *expect, size_t n, const char *what )
{
    unsigned char out[16];
    memset( out, 0xAA, sizeof(out) );
    check( gl4es_ios_from_rgba8( src, 2, 2, format, UB_, 1, out ) == 1 && !memcmp( out, expect, n ) &&
           (n == sizeof(out) || out[n] == 0xAA), what );
}

int main( void )
{
    /* which stored formats a framebuffer can take as a colour attachment */
    check( gl4es_ios_attachable( RGBA_, UB_, 0 ), "RGBA8 attaches" );
    check( gl4es_ios_attachable( RGBA_, U4444, 0 ) && gl4es_ios_attachable( RGBA_, U5551, 0 ), "RGBA4, RGB5_A1 attach" );
    check( gl4es_ios_attachable( RGB_, U565, 0 ), "RGB565 attaches" );
    check( gl4es_ios_attachable( BGRA_, UB_, 1 ) && !gl4es_ios_attachable( BGRA_, UB_, 0 ),
           "BGRA8 only with EXT_texture_format_BGRA8888" );
    check( !gl4es_ios_attachable( ALPHA_, UB_, 1 ) && !gl4es_ios_attachable( LUM_, UB_, 1 ) &&
           !gl4es_ios_attachable( LUMA_, UB_, 1 ), "ALPHA, LUMINANCE, LUMINANCE_ALPHA are drawn instead" );
    check( !gl4es_ios_attachable( RGB_, UB_, 1 ) && !gl4es_ios_attachable( RED_, UB_, 1 ) &&
           !gl4es_ios_attachable( RGBA_, FLOAT_, 1 ), "RGB8, R8 and float are drawn instead (ES 2.0's set)" );

    /* row sizes with GL_PACK_ALIGNMENT */
    check( gl4es_ios_row_size( 3, RGB_, U565, 4 ) == 8 && gl4es_ios_row_size( 3, RGB_, U565, 1 ) == 6, "565 rows" );
    check( gl4es_ios_row_size( 3, RGBA_, UB_, 8 ) == 16 && gl4es_ios_row_size( 5, RED_, UB_, 4 ) == 8, "byte rows" );
    check( gl4es_ios_row_size( 3, BGR_, UB_, 2 ) == 10 && gl4es_ios_row_size( 4, BGRA_, U8888R, 8 ) == 16, "other rows" );
    check( gl4es_ios_row_size( 3, RGB_, U4444, 4 ) == 0 && gl4es_ios_row_size( 3, RGBA_, FLOAT_, 4 ) == 0, "unknown pairs" );
    check( gl4es_ios_row_size( 3, RGBA_, UB_, 3 ) == 0 && gl4es_ios_row_size( -1, RGBA_, UB_, 4 ) == 0, "bad alignment, width" );

    /* 32-bit: X8R8G8B8/A8R8G8B8 is BGRA with UNSIGNED_INT_8_8_8_8_REV (first component lowest) */
    check32( BGRA_, U8888R, 0xFFFF0000, 0x8000FF00, 0x000000FF, 0xFFFFFFFF, "BGRA 8_8_8_8_REV" );
    check32( RGBA_, U8888R, 0xFF0000FF, 0x8000FF00, 0x00FF0000, 0xFFFFFFFF, "RGBA 8_8_8_8_REV" );
    check32( RGBA_, U8888, 0xFF0000FF, 0x00FF0080, 0x0000FF00, 0xFFFFFFFF, "RGBA 8_8_8_8" );
    check32( BGRA_, U8888, 0x0000FFFF, 0x00FF0080, 0xFF000000, 0xFFFFFFFF, "BGRA 8_8_8_8" );

    /* 16-bit: R5G6B5 is RGB with 5_6_5; X1R5G5B5/A1R5G5B5 BGRA with 1_5_5_5_REV; A4R4G4B4 BGRA
     * with 4_4_4_4_REV (alpha 128 becomes 1 bit 1, 4 bits 8) */
    check16( RGB_, U565, 0xF800, 0x07E0, 0x001F, 0xFFFF, "RGB 5_6_5" );
    check16( BGR_, U565, 0x001F, 0x07E0, 0xF800, 0xFFFF, "BGR 5_6_5" );
    check16( RGB_, U565R, 0x001F, 0x07E0, 0xF800, 0xFFFF, "RGB 5_6_5_REV" );
    check16( BGRA_, U1555R, 0xFC00, 0x83E0, 0x001F, 0xFFFF, "BGRA 1_5_5_5_REV" );
    check16( RGBA_, U1555R, 0x801F, 0x83E0, 0x7C00, 0xFFFF, "RGBA 1_5_5_5_REV" );
    check16( RGBA_, U5551, 0xF801, 0x07C1, 0x003E, 0xFFFF, "RGBA 5_5_5_1" );
    check16( BGRA_, U4444R, 0xFF00, 0x80F0, 0x000F, 0xFFFF, "BGRA 4_4_4_4_REV" );
    check16( RGBA_, U4444, 0xF00F, 0x0F08, 0x00F0, 0xFFFF, "RGBA 4_4_4_4" );

    /* 8-bit: the palettised surface's index as RED or ALPHA, LUMINANCE as the red channel */
    {
        static const unsigned char red[4] = { 255, 0, 0, 255 }, alpha[4] = { 255, 128, 0, 255 };
        static const unsigned char rg[8] = { 255, 0, 0, 255, 0, 0, 255, 255 };
        static const unsigned char la[8] = { 255, 255, 0, 128, 0, 0, 255, 255 };
        static const unsigned char rgb[12] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255 };
        static const unsigned char bgr[12] = { 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255 };
        static const unsigned char bgra[16] = { 0, 0, 255, 255, 0, 255, 0, 128, 255, 0, 0, 0, 255, 255, 255, 255 };
        check8( RED_, red, 4, "RED" );
        check8( LUM_, red, 4, "LUMINANCE (red channel)" );
        check8( ALPHA_, alpha, 4, "ALPHA" );
        check8( RG_, rg, 8, "RG" );
        check8( LUMA_, la, 8, "LUMINANCE_ALPHA" );
        check8( RGB_, rgb, 12, "RGB" );
        check8( BGR_, bgr, 12, "BGR" );
        check8( RGBA_, src, 16, "RGBA (copy)" );
        check8( BGRA_, bgra, 16, "BGRA" );
    }

    /* rows padded to the alignment: the padding is not written (a 1 x 2 image: src's first two
     * pixels, red then green) */
    {
        unsigned char out[16];
        memset( out, 0xAA, sizeof(out) );
        check( gl4es_ios_from_rgba8( src, 1, 2, RGB_, U565, 4, out ) == 1, "padded rows" );
        check( u16( out ) == 0xF800 && out[2] == 0xAA && out[3] == 0xAA && u16( out + 4 ) == 0x07E0 &&
               out[6] == 0xAA, "padded rows: the second row at 4 bytes, padding untouched" );
    }

    /* an unknown pair writes nothing */
    {
        unsigned char out[16];
        int i, untouched = 1;
        memset( out, 0xAA, sizeof(out) );
        check( gl4es_ios_from_rgba8( src, 2, 2, RGBA_, FLOAT_, 4, out ) == 0, "FLOAT refused" );
        check( gl4es_ios_from_rgba8( src, 2, 2, RGB_, U4444, 4, out ) == 0, "RGB with 4_4_4_4 refused" );
        for (i = 0; i < 16; i++) untouched &= out[i] == 0xAA;
        check( untouched, "nothing written for a refused pair" );
    }

    /* the failure log's gate: once per key, then quiet; 64 keys at most */
    {
        unsigned int k;
        int firsts = 0;
        check( gl4es_ios_first_time( 0x8059 ) == 1 && gl4es_ios_first_time( 0x8059 ) == 0, "once per key" );
        check( gl4es_ios_first_time( 0x803C ) == 1, "another key" );
        for (k = 1; k <= 100; k++) firsts += gl4es_ios_first_time( 0x10000 + k );
        check( firsts == 62, "64 keys in all, then quiet" );
        check( gl4es_ios_first_time( 0x8059 ) == 0, "a known key stays quiet" );
    }

    printf( "test_gl4es_ios_pixels: %d checks, %d failures\n", checks, failures );
    return failures != 0;
}
