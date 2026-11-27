// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 35 clarification 13: the pure parts of glGetTexImage's read-back (texture_read.c), and
// item 15's framebuffer failure line (framebuffers.c) and ES version (gl4es_ios.c), with no gl4es
// state, so the host tests can run them (tests/test_gl4es_ios_pixels.c). Formats and types are
// GL's enum values.
#ifndef GL4ES_IOS_PIXELS_H
#define GL4ES_IOS_PIXELS_H

#include <stddef.h>

/* 1 when a GLES texture stored as format/type can be a framebuffer's colour attachment (so
 * glGetTexImage reads it directly): RGBA with UNSIGNED_BYTE, 4_4_4_4 or 5_5_5_1, RGB with 5_6_5,
 * and BGRA with UNSIGNED_BYTE when the GLES has EXT_texture_format_BGRA8888 (bgra8888). The set
 * is what OpenGL ES 2.0 and its extensions make renderable; ALPHA, LUMINANCE, LUMINANCE_ALPHA and
 * the rest are not, and are drawn into an RGBA8 target instead. */
int gl4es_ios_attachable( unsigned int format, unsigned int type, int bgra8888 );

/* The bytes one row of width pixels of format/type takes with GL_PACK_ALIGNMENT align (1, 2, 4
 * or 8); 0 when format/type is not one gl4es_ios_from_rgba8 writes. */
size_t gl4es_ios_row_size( int width, unsigned int format, unsigned int type, int align );

/* Writes width x height pixels, read as RGBA8 (4 bytes a pixel, rows packed, row 0 first), as
 * format/type to dst, each row padded to align. Returns 0, writing nothing, for a pair it doesn't
 * know. The pairs are those wined3d's DirectDraw and Direct3D read-backs use, and their GL
 * relatives:
 *   UNSIGNED_BYTE: RGBA, BGRA, RGB, BGR, RED, RG, ALPHA, LUMINANCE (the red channel),
 *                  LUMINANCE_ALPHA
 *   UNSIGNED_INT_8_8_8_8(_REV): RGBA, BGRA
 *   UNSIGNED_SHORT_5_6_5(_REV): RGB, BGR
 *   UNSIGNED_SHORT_4_4_4_4(_REV), UNSIGNED_SHORT_5_5_5_1, UNSIGNED_SHORT_1_5_5_5_REV: RGBA, BGRA
 * Packed values are stored in the host's byte order, as GL defines them; an 8-bit channel n
 * becomes (n * max + 127) / 255 in a narrower field. */
int gl4es_ios_from_rgba8( const unsigned char *rgba, int width, int height, unsigned int format,
                          unsigned int type, int align, void *dst );

/* 1 the first time key is passed, 0 after (for "log once per format"); 0 also once 256 different
 * keys have been seen. Thread-safe. glGetTexImage's keys are internal formats; the framebuffer
 * line's are GL4ES_IOS_ONCE_FBO | internal format. */
int gl4es_ios_first_time( unsigned int key );
#define GL4ES_IOS_ONCE_FBO 0x01000000u

/* Item 15. The name of a GL error or framebuffer status (glGetError, glCheckFramebufferStatus),
 * NULL for another value. */
const char *gl4es_ios_gl_reason( unsigned int code );

/* Item 15. Writes the line for a colour attachment that failed (snprintf's result):
 *   [gl4es] FBO colour attach failed: internal 0x<asked> stored as 0x<format>/0x<type> (ES <n>): <reason>
 * internal is the internal format the application asked for; format/type what gl4es created in
 * GLES; for a renderbuffer (renderbuffer non-zero) "stored as renderbuffer 0x<format>", format
 * being the internal format gl4es gave GLES. es_major is the GLES context's major version; code a
 * GL error or framebuffer status, by name when gl4es_ios_gl_reason knows it, else as 0x<code>. */
int gl4es_ios_fbo_line( char *buf, size_t size, unsigned int internal, unsigned int format,
                        unsigned int type, int renderbuffer, int es_major, unsigned int code );

/* Item 15. The major version in a GLES GL_VERSION string ("OpenGL ES 3.0 (ANGLE ...)" is 3,
 * "OpenGL ES-CM 1.1" 1); 2, gl4es's own assumption, for NULL or anything else. */
int gl4es_ios_es_major_of( const char *version );

/* Item 15. The internal format to give GLES's glTexImage2D for what gl4es passes: gl4es gives
 * the format itself as the internal format (ES 2.0's rule). On ES 3 (es_major >= 3), RED and RG
 * with UNSIGNED_BYTE become the sized R8 and RG8 (ES 3.0 lists no unsized RED or RG); anything
 * else, and anything on ES 2, is returned as it is. */
unsigned int gl4es_ios_gles_internal( int es_major, unsigned int internal, unsigned int format,
                                      unsigned int type );

#endif /* GL4ES_IOS_PIXELS_H */
