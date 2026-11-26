/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */

/*
 * Spec 19 A17: the off-by-default dump of a failed compile, the pure part shared by compiler.c
 * and the host tests. With PG_D3DCOMPILER_DUMP_FAILED=1 in the environment, a failed D3DCompile
 * writes C:\users\Public\pg-dumps\shaders\<8 hex digits>.hlsl: a comment header (source name,
 * entry point, target, flags, result, the compiler's messages) and then the source after
 * preprocessing (the source as given when preprocessing fails too). The name is a hash of the
 * text, entry point, target and flags, so the same failing shader is written once.
 */

#ifndef MYIOSDECK_COMPILE_FAILURE_DUMP_H
#define MYIOSDECK_COMPILE_FAILURE_DUMP_H

#include <stddef.h>
#include <string.h>

#define COMPILE_DUMP_ENV        "PG_D3DCOMPILER_DUMP_FAILED"
#define COMPILE_DUMP_PARENT     "C:\\users\\Public\\pg-dumps"
#define COMPILE_DUMP_DIR        COMPILE_DUMP_PARENT "\\shaders"
#define COMPILE_DUMP_MAX_FILES  64      /* files written per process */
#define COMPILE_DUMP_NAME_SIZE  16      /* "xxxxxxxx.hlsl" and its NUL */
/* the line logged for each file written; %s is the file name */
#define COMPILE_DUMP_LOG_FORMAT "[d3dcompiler] failed shader dumped: %s\n"

struct compile_dump_info
{
    const char *text;           /* the source, preprocessed when that worked */
    size_t text_size;           /* without a trailing NUL */
    int preprocessed;
    const char *source_name;    /* each of these may be NULL */
    const char *entry;
    const char *target;
    unsigned int flags;
    unsigned int result;        /* the HRESULT */
    const char *messages;       /* the error blob's text (maybe not NUL-terminated) */
    size_t messages_size;
};

/* the environment variable's value turns the dump on only when it is exactly "1" */
static inline int compile_dump_enabled( const char *value )
{
    return value && !strcmp( value, "1" );
}

/* FNV-1a, 32 bits, continued from h (start with 2166136261u) */
static inline unsigned int compile_dump_hash_bytes( unsigned int h, const void *data, size_t size )
{
    const unsigned char *p = data;
    size_t i;

    for (i = 0; i < size; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static inline unsigned int compile_dump_hash_str( unsigned int h, const char *s )
{
    /* the NUL too, so ("ab", "c") and ("a", "bc") differ */
    return s ? compile_dump_hash_bytes( h, s, strlen( s ) + 1 ) : compile_dump_hash_bytes( h, "", 1 );
}

static inline unsigned int compile_dump_hash( const struct compile_dump_info *info )
{
    unsigned char flags[4];
    unsigned int h = 2166136261u;

    flags[0] = info->flags & 0xff;
    flags[1] = (info->flags >> 8) & 0xff;
    flags[2] = (info->flags >> 16) & 0xff;
    flags[3] = (info->flags >> 24) & 0xff;
    h = compile_dump_hash_bytes( h, info->text, info->text_size );
    h = compile_dump_hash_str( h, info->entry );
    h = compile_dump_hash_str( h, info->target );
    return compile_dump_hash_bytes( h, flags, sizeof(flags) );
}

/* "<8 lower-case hex digits>.hlsl" */
static inline void compile_dump_name( char name[COMPILE_DUMP_NAME_SIZE], unsigned int hash )
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 8; i++) name[i] = hex[(hash >> (28 - 4 * i)) & 0xf];
    memcpy( name + 8, ".hlsl", sizeof(".hlsl") );
}

/* the file's contents are built in two passes: the first (buf NULL) counts the bytes */
struct compile_dump_out
{
    char *buf;
    size_t size, len;
};

static inline void compile_dump_put( struct compile_dump_out *out, const char *s, size_t n )
{
    if (out->buf && out->len < out->size)
        memcpy( out->buf + out->len, s, n < out->size - out->len ? n : out->size - out->len );
    out->len += n;
}

static inline void compile_dump_puts( struct compile_dump_out *out, const char *s )
{
    compile_dump_put( out, s, strlen( s ) );
}

static inline void compile_dump_hex( struct compile_dump_out *out, unsigned int v )
{
    static const char hex[] = "0123456789abcdef";
    char text[11];
    int i, n = 0;

    text[n++] = '0';
    text[n++] = 'x';
    for (i = 28; i > 0 && !(v >> i); i -= 4) ;
    for (; i >= 0; i -= 4) text[n++] = hex[(v >> i) & 0xf];
    compile_dump_put( out, text, n );
}

/* one "// <label>: <value>" line; a value never runs past its line */
static inline void compile_dump_field( struct compile_dump_out *out, const char *label, const char *value )
{
    size_t n = 0;

    compile_dump_puts( out, "// " );
    compile_dump_puts( out, label );
    compile_dump_puts( out, ": " );
    if (!value) value = "-";
    while (value[n] && value[n] != '\n' && value[n] != '\r') n++;
    compile_dump_put( out, value, n );
    compile_dump_puts( out, "\n" );
}

/* writes at most size bytes to buf (none when buf is NULL); returns the file's full size */
static inline size_t compile_dump_format( char *buf, size_t size, const struct compile_dump_info *info )
{
    struct compile_dump_out out = { buf, size, 0 };
    const char *pos = info->messages, *end = info->messages ? info->messages + info->messages_size : NULL;

    compile_dump_puts( &out, "// d3dcompiler: a failed compile, dumped for diagnosis (" COMPILE_DUMP_ENV "=1)\n" );
    compile_dump_field( &out, "source", info->source_name ? info->source_name : "(memory)" );
    compile_dump_field( &out, "entry", info->entry );
    compile_dump_field( &out, "target", info->target );
    compile_dump_puts( &out, "// flags: " );
    compile_dump_hex( &out, info->flags );
    compile_dump_puts( &out, "\n// result: " );
    compile_dump_hex( &out, info->result );
    compile_dump_puts( &out, info->preprocessed ? "\n// text: after preprocessing\n"
                                                : "\n// text: as given (preprocessing failed)\n" );
    compile_dump_puts( &out, "// messages:\n" );
    /* every line of the messages, as a comment; a trailing '\r' is dropped, empty lines skipped */
    while (pos && pos < end && *pos)
    {
        const char *stop = pos;
        size_t n;

        while (stop < end && *stop && *stop != '\n') stop++;
        n = (size_t)(stop - pos);
        if (n && pos[n - 1] == '\r') n--;
        if (n)
        {
            compile_dump_puts( &out, "//   " );
            compile_dump_put( &out, pos, n );
            compile_dump_puts( &out, "\n" );
        }
        pos = (stop < end && *stop == '\n') ? stop + 1 : stop;
    }
    compile_dump_puts( &out, "\n" );
    compile_dump_put( &out, info->text, info->text_size );
    if (info->text_size && info->text[info->text_size - 1] != '\n') compile_dump_puts( &out, "\n" );
    return out.len;
}

/* the text's size without the NULs a blob may end with */
static inline size_t compile_dump_text_size( const char *text, size_t size )
{
    while (size && !text[size - 1]) size--;
    return size;
}

#endif /* MYIOSDECK_COMPILE_FAILURE_DUMP_H */
