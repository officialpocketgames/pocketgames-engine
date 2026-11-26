// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 clarification 13: the pure parts of glGetTexImage's read-back (gl4es_ios_pixels.h).
// New in this tree's gl4es (not upstream); written from the OpenGL 2.1 specification's pixel
// formats and packed types.

#include "gl4es_ios_pixels.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

/* GL's values (gl.xml), named here so this file needs no GL header */
enum
{
    PX_RED = 0x1903, PX_ALPHA = 0x1906, PX_RGB = 0x1907, PX_RGBA = 0x1908, PX_LUMINANCE = 0x1909,
    PX_LUMINANCE_ALPHA = 0x190A, PX_BGR = 0x80E0, PX_BGRA = 0x80E1, PX_RG = 0x8227,
    PX_UNSIGNED_BYTE = 0x1401,
    PX_4444 = 0x8033, PX_5551 = 0x8034, PX_8888 = 0x8035,
    PX_565 = 0x8363, PX_565_REV = 0x8364, PX_4444_REV = 0x8365, PX_1555_REV = 0x8366, PX_8888_REV = 0x8367,
};

int gl4es_ios_attachable( unsigned int format, unsigned int type, int bgra8888 )
{
    switch (format)
    {
    case PX_RGBA: return type == PX_UNSIGNED_BYTE || type == PX_4444 || type == PX_5551;
    case PX_RGB:  return type == PX_565;
    case PX_BGRA: return type == PX_UNSIGNED_BYTE && bgra8888;
    default:      return 0;
    }
}

/* bytes a pixel of format/type takes, 0 when gl4es_ios_from_rgba8 doesn't write it */
static int pixel_size( unsigned int format, unsigned int type )
{
    switch (type)
    {
    case PX_UNSIGNED_BYTE:
        switch (format)
        {
        case PX_RGBA: case PX_BGRA: return 4;
        case PX_RGB: case PX_BGR: return 3;
        case PX_RG: case PX_LUMINANCE_ALPHA: return 2;
        case PX_RED: case PX_ALPHA: case PX_LUMINANCE: return 1;
        default: return 0;
        }
    case PX_8888: case PX_8888_REV:
        return (format == PX_RGBA || format == PX_BGRA) ? 4 : 0;
    case PX_565: case PX_565_REV:
        return (format == PX_RGB || format == PX_BGR) ? 2 : 0;
    case PX_4444: case PX_4444_REV: case PX_5551: case PX_1555_REV:
        return (format == PX_RGBA || format == PX_BGRA) ? 2 : 0;
    default:
        return 0;
    }
}

size_t gl4es_ios_row_size( int width, unsigned int format, unsigned int type, int align )
{
    size_t bytes;
    int size = pixel_size( format, type );

    if (!size || width < 0 || (align != 1 && align != 2 && align != 4 && align != 8)) return 0;
    bytes = (size_t)width * (size_t)size;
    return (bytes + (size_t)align - 1) / (size_t)align * (size_t)align;
}

/* an 8-bit channel in a field of bits bits */
static unsigned int narrow( unsigned int v, int bits )
{
    unsigned int max = (1u << bits) - 1;
    return (v * max + 127) / 255;
}

int gl4es_ios_from_rgba8( const unsigned char *rgba, int width, int height, unsigned int format,
                          unsigned int type, int align, void *dst )
{
    size_t row = gl4es_ios_row_size( width, format, type, align );
    int size = pixel_size( format, type );
    int bgr = (format == PX_BGRA || format == PX_BGR);
    int x, y;

    if (!row || height < 0) return 0;
    for (y = 0; y < height; y++)
    {
        unsigned char *out = (unsigned char *)dst + (size_t)y * row;
        const unsigned char *in = rgba + (size_t)y * (size_t)width * 4;

        for (x = 0; x < width; x++, in += 4, out += size)
        {
            unsigned int r = in[0], g = in[1], b = in[2], a = in[3];
            /* c0..c3: the components in the format's order (RGBA or BGRA) */
            unsigned int c0 = bgr ? b : r, c1 = g, c2 = bgr ? r : b, c3 = a;
            uint32_t v32;
            uint16_t v16;

            switch (type)
            {
            case PX_UNSIGNED_BYTE:
                switch (format)
                {
                case PX_RGBA: case PX_BGRA: out[0] = c0; out[1] = c1; out[2] = c2; out[3] = c3; break;
                case PX_RGB: case PX_BGR:   out[0] = c0; out[1] = c1; out[2] = c2; break;
                case PX_RG:                 out[0] = r; out[1] = g; break;
                case PX_LUMINANCE_ALPHA:    out[0] = r; out[1] = a; break;
                case PX_RED: case PX_LUMINANCE: out[0] = r; break;
                case PX_ALPHA:              out[0] = a; break;
                }
                break;
            case PX_8888:
                v32 = (uint32_t)c0 << 24 | (uint32_t)c1 << 16 | (uint32_t)c2 << 8 | c3;
                memcpy( out, &v32, 4 );
                break;
            case PX_8888_REV:
                v32 = (uint32_t)c3 << 24 | (uint32_t)c2 << 16 | (uint32_t)c1 << 8 | c0;
                memcpy( out, &v32, 4 );
                break;
            case PX_565:
                v16 = (uint16_t)(narrow( c0, 5 ) << 11 | narrow( c1, 6 ) << 5 | narrow( c2, 5 ));
                memcpy( out, &v16, 2 );
                break;
            case PX_565_REV:
                v16 = (uint16_t)(narrow( c2, 5 ) << 11 | narrow( c1, 6 ) << 5 | narrow( c0, 5 ));
                memcpy( out, &v16, 2 );
                break;
            case PX_4444:
                v16 = (uint16_t)(narrow( c0, 4 ) << 12 | narrow( c1, 4 ) << 8 | narrow( c2, 4 ) << 4 | narrow( c3, 4 ));
                memcpy( out, &v16, 2 );
                break;
            case PX_4444_REV:
                v16 = (uint16_t)(narrow( c3, 4 ) << 12 | narrow( c2, 4 ) << 8 | narrow( c1, 4 ) << 4 | narrow( c0, 4 ));
                memcpy( out, &v16, 2 );
                break;
            case PX_5551:
                v16 = (uint16_t)(narrow( c0, 5 ) << 11 | narrow( c1, 5 ) << 6 | narrow( c2, 5 ) << 1 | narrow( c3, 1 ));
                memcpy( out, &v16, 2 );
                break;
            case PX_1555_REV:
                v16 = (uint16_t)(narrow( c3, 1 ) << 15 | narrow( c2, 5 ) << 10 | narrow( c1, 5 ) << 5 | narrow( c0, 5 ));
                memcpy( out, &v16, 2 );
                break;
            }
        }
    }
    return 1;
}

static pthread_mutex_t seen_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int seen[64];
static int seen_count;

int gl4es_ios_first_time( unsigned int key )
{
    int i, first = 0;

    pthread_mutex_lock( &seen_lock );
    for (i = 0; i < seen_count; i++) if (seen[i] == key) break;
    if (i == seen_count && seen_count < (int)(sizeof(seen) / sizeof(seen[0])))
    {
        seen[seen_count++] = key;
        first = 1;
    }
    pthread_mutex_unlock( &seen_lock );
    return first;
}
