/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */

/*
 * Spec 19 A17: the off-by-default dump of a failed compile, the pure part shared by compiler.c
 * and the host tests. With PG_D3DCOMPILER_DUMP_FAILED=1 in the environment, a failed D3DCompile
 * writes C:\users\Public\pg-dumps\shaders\<8 hex digits>.hlsl: a comment header (source name,
 * entry point, target, flags, result, the compiler's messages) and then the source after
 * preprocessing (the source as given when preprocessing fails too). The name is a hash of the
 * text, entry point, target and flags, so the same failing shader is written once.
 *
 * A19: with PG_D3DCOMPILER_DUMP_ALL=1, successful compiles for Shader Model 1-3 (vs_1_1 to
 * ps_3_0) are written the same way, with the result's constant table as text and its bytecode
 * in hex added to the header, at most 400 files per process.
 *
 * A22: used by vkd3d-utils' D3DCompile2VKD3D (vkd3d_utils_main.c), which every run-time compile
 * passes, d3dcompiler's and d3dx9's (it was in d3dcompiler's compiler.c before).
 *
 * A25: each dump also has the compiler version, the entry point and DLL the call came through
 * (from the call stack), the macros the program passed (a NULL value apart from an empty one),
 * the include files the include handler was asked for (in the order asked), and, after the
 * preprocessed text, the source exactly as given and each include file's text. A failed
 * D3DPreprocess is dumped too.
 */

#ifndef MYIOSDECK_COMPILE_FAILURE_DUMP_H
#define MYIOSDECK_COMPILE_FAILURE_DUMP_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMPILE_DUMP_ENV        "PG_D3DCOMPILER_DUMP_FAILED"
#define COMPILE_DUMP_PARENT     "C:\\users\\Public\\pg-dumps"
#define COMPILE_DUMP_DIR        COMPILE_DUMP_PARENT "\\shaders"
#define COMPILE_DUMP_MAX_FILES  64      /* files written per process */
#define COMPILE_DUMP_NAME_SIZE  16      /* "xxxxxxxx.hlsl" and its NUL */
/* the line logged for each file written; %s is the file name */
#define COMPILE_DUMP_LOG_FORMAT "[d3dcompiler] failed shader dumped: %s\n"

/* A19: successful Shader Model 1-3 compiles */
#define COMPILE_DUMP_ALL_ENV        "PG_D3DCOMPILER_DUMP_ALL"
#define COMPILE_DUMP_ALL_MAX_FILES  400     /* files written per process */
#define COMPILE_DUMP_ALL_LOG_EVERY  50      /* one log line per this many files */
/* %ld is the number of files written so far, %s the last one's name */
#define COMPILE_DUMP_ALL_LOG_FORMAT "[d3dcompiler] successful shaders dumped: %ld, the last %s\n"
/* A22: logged once per process at the first compile; "on"/"off" twice, then the caller's
 * compiler version (d3dcompiler's 43 or 47, or d3dx9's SDK version, 35 for d3dx9_35) */
#define COMPILE_DUMP_FIRST_LOG_FORMAT \
    "[d3dcompiler] dump: failed %s, all %s (entry vkd3d_D3DCompile2VKD3D, compiler version %u)\n"

/* A25: a macro as the program passed it (the layout of D3D_SHADER_MACRO); a list ends at a NULL name */
struct compile_dump_macro
{
    const char *name;
    const char *definition;
};

/* A25: the include files the include handler was asked for, in the order asked */
#define COMPILE_DUMP_MAX_INCLUDES       64
#define COMPILE_DUMP_MAX_INCLUDE_BYTES  (4u << 20)     /* text kept, all files together */

struct compile_dump_include_file
{
    char *name;             /* a copy; NULL when the handler was given none */
    int local;              /* D3D_INCLUDE_LOCAL ("...") rather than D3D_INCLUDE_SYSTEM (<...>) */
    unsigned int result;    /* the handler's HRESULT */
    size_t size;            /* the size the handler returned */
    char *text;             /* a copy of the data; NULL when the handler failed or past the limit */
};

struct compile_dump_includes
{
    struct compile_dump_include_file files[COMPILE_DUMP_MAX_INCLUDES];
    size_t count;
    size_t dropped;         /* requests past COMPILE_DUMP_MAX_INCLUDES */
    size_t kept;            /* bytes of text kept */
};

/* A25: one return address of the call stack, innermost first */
struct compile_dump_frame
{
    const char *module;         /* the module's file name; NULL when the address is in none */
    const char *symbol;         /* the nearest export at or below the address, or NULL */
    unsigned long long offset;  /* from the symbol, else from the module's base, else the address */
    int own;                    /* in the module that writes the dump */
};

struct compile_dump_info
{
    int succeeded;              /* A19: a successful compile, with its bytecode below */
    const unsigned char *code;  /* the compiled shader (Direct3D 9 bytecode) */
    size_t code_size;
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
    /* A25 */
    int preprocess_call;        /* a failed D3DPreprocess rather than a compile */
    unsigned int compiler_version;  /* D3DCompile2VKD3D's: 43 or 47 (d3dcompiler), d3dx9's SDK version; 0: unknown */
    int has_macros;             /* the program passed a macro list (macros may still be empty) */
    const struct compile_dump_macro *macros;
    const char *source;         /* the source as given; NULL: not written */
    size_t source_size;
    const struct compile_dump_includes *includes;  /* NULL: no include handler */
    const struct compile_dump_frame *frames;
    size_t frame_count;
};

/* the environment variable's value turns the dump on only when it is exactly "1" */
static inline int compile_dump_enabled( const char *value )
{
    return value && !strcmp( value, "1" );
}

/* A19: a Shader Model 1-3 vertex or pixel shader target ("vs_1_1", "ps_2_a", "ps_3_0", ...) */
static inline int compile_dump_sm1_target( const char *target )
{
    return target && (target[0] == 'v' || target[0] == 'p') && target[1] == 's' && target[2] == '_'
           && target[3] >= '1' && target[3] <= '3' && target[4] == '_';
}

/* A19: the profile actually compiled. With D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY (0x1000)
 * Microsoft's compiler compiles ps_1_x shaders as ps_2_0 (it has no effect on other targets);
 * vkd3d-utils doesn't, and vkd3d would write ps_1_x bytecode instead. */
#define COMPILE_FLAG_BACKWARDS_COMPATIBILITY 0x1000u

static inline const char *compile_effective_profile( const char *profile, unsigned int flags )
{
    if ((flags & COMPILE_FLAG_BACKWARDS_COMPATIBILITY) && profile && !strncmp( profile, "ps_1_", 5 ))
        return "ps_2_0";
    return profile;
}

/* A19: log after the n-th successful file (n from 1)? */
static inline int compile_dump_all_log( long n )
{
    return n > 0 && n % COMPILE_DUMP_ALL_LOG_EVERY == 0;
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

/* ---- A19: the constant table (CTAB) of Direct3D 9 bytecode, as text ---- */

static inline unsigned int compile_dump_rd16( const unsigned char *p )
{
    return p[0] | (p[1] << 8);
}

static inline unsigned int compile_dump_rd32( const unsigned char *p )
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* the CTAB comment's data: the comments right after the version token are searched */
static inline const unsigned char *compile_dump_find_ctab( const unsigned char *code, size_t size,
                                                           size_t *ctab_size )
{
    size_t pos = 4;

    if (!code || size < 8) return NULL;
    while (pos + 4 <= size)
    {
        unsigned int token = compile_dump_rd32( code + pos ), len;

        if ((token & 0xffff) != 0xfffe) return NULL;   /* not a comment: the code starts */
        len = (token >> 16) & 0x7fff;                  /* DWORDs */
        if (len > (size - pos - 4) / 4) return NULL;
        if (len >= 1 && compile_dump_rd32( code + pos + 4 ) == 0x42415443u /* "CTAB" */)
        {
            *ctab_size = (size_t)(len - 1) * 4;
            return code + pos + 8;
        }
        pos += 4 + (size_t)len * 4;
    }
    return NULL;
}

/* a string at offset off of the table, printable ASCII only, at most 63 characters */
static inline void compile_dump_ctab_str( struct compile_dump_out *out, const unsigned char *ctab,
                                          size_t size, unsigned int off )
{
    size_t i;

    if (!off || off >= size)
    {
        compile_dump_puts( out, "-" );
        return;
    }
    for (i = off; i < size && ctab[i] && i - off < 63; i++)
    {
        char c = (ctab[i] >= 0x20 && ctab[i] < 0x7f) ? (char)ctab[i] : '?';
        compile_dump_put( out, &c, 1 );
    }
}

static inline void compile_dump_fmt( struct compile_dump_out *out, const char *fmt, unsigned int a, unsigned int b )
{
    char text[64];
    int n = snprintf( text, sizeof(text), fmt, a, b );

    if (n > 0) compile_dump_put( out, text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1 );
}

static inline void compile_dump_ctab( struct compile_dump_out *out, const unsigned char *code, size_t code_size )
{
    static const char *const sets[] = { "BOOL", "INT4", "FLOAT4", "SAMPLER" };
    static const char regs[] = "bics";
    static const char *const classes[] = { "SCALAR", "VECTOR", "MATRIX_ROWS", "MATRIX_COLUMNS", "OBJECT", "STRUCT" };
    static const char *const types[] =
    {
        "VOID", "BOOL", "INT", "FLOAT", "STRING", "TEXTURE", "TEXTURE1D", "TEXTURE2D", "TEXTURE3D",
        "TEXTURECUBE", "SAMPLER", "SAMPLER1D", "SAMPLER2D", "SAMPLER3D", "SAMPLERCUBE",
        "PIXELSHADER", "VERTEXSHADER", "PIXELFRAGMENT", "VERTEXFRAGMENT", "UNSUPPORTED",
    };
    const unsigned char *ctab;
    unsigned int count, info, i;
    size_t size;

    if (!(ctab = compile_dump_find_ctab( code, code_size, &size )))
    {
        compile_dump_puts( out, "// constant table: none\n" );
        return;
    }
    if (size < 28 || compile_dump_rd32( ctab ) != 28)
    {
        compile_dump_puts( out, "// constant table: bad header\n" );
        return;
    }
    count = compile_dump_rd32( ctab + 12 );
    info = compile_dump_rd32( ctab + 16 );
    compile_dump_fmt( out, "// constant table: %u constants, version %#x, creator \"", count, compile_dump_rd32( ctab + 8 ) );
    compile_dump_ctab_str( out, ctab, size, compile_dump_rd32( ctab + 4 ) );
    compile_dump_fmt( out, "\", flags %#x, target \"", compile_dump_rd32( ctab + 20 ), 0 );
    compile_dump_ctab_str( out, ctab, size, compile_dump_rd32( ctab + 24 ) );
    compile_dump_puts( out, "\"\n" );
    if (info > size || count > (size - info) / 20)
    {
        compile_dump_puts( out, "//   (the constants run past the table)\n" );
        return;
    }
    for (i = 0; i < count; i++)
    {
        const unsigned char *c = ctab + info + (size_t)i * 20;
        unsigned int set = compile_dump_rd16( c + 4 ), index = compile_dump_rd16( c + 6 );
        unsigned int regcount = compile_dump_rd16( c + 8 ), type = compile_dump_rd32( c + 12 );
        unsigned int def = compile_dump_rd32( c + 16 );

        compile_dump_puts( out, "//   " );
        compile_dump_ctab_str( out, ctab, size, compile_dump_rd32( c ) );
        if (set < 4)
        {
            compile_dump_puts( out, ": " );
            compile_dump_puts( out, sets[set] );
            compile_dump_fmt( out, " %c%u", (unsigned char)regs[set], index );
        }
        else compile_dump_fmt( out, ": set %u index %u", set, index );
        compile_dump_fmt( out, " count %u", regcount, 0 );
        if (type && type <= size && size - type >= 16)
        {
            const unsigned char *t = ctab + type;
            unsigned int cls = compile_dump_rd16( t ), ty = compile_dump_rd16( t + 2 );

            compile_dump_puts( out, ", " );
            if (cls < 6) compile_dump_puts( out, classes[cls] );
            else compile_dump_fmt( out, "class %u", cls, 0 );
            compile_dump_puts( out, " " );
            if (ty < sizeof(types) / sizeof(types[0])) compile_dump_puts( out, types[ty] );
            else compile_dump_fmt( out, "type %u", ty, 0 );
            compile_dump_fmt( out, " %ux%u", compile_dump_rd16( t + 4 ), compile_dump_rd16( t + 6 ) );
            if (compile_dump_rd16( t + 8 ) > 1) compile_dump_fmt( out, " [%u]", compile_dump_rd16( t + 8 ), 0 );
            if (compile_dump_rd16( t + 10 )) compile_dump_fmt( out, ", %u members", compile_dump_rd16( t + 10 ), 0 );
        }
        else compile_dump_puts( out, ", no type" );
        /* the default value: the first 16 floats at most */
        if (def && def <= size && set == 2)
        {
            unsigned int k, n = regcount * 4;

            if (n > 16) n = 16;
            if (n > (size - def) / 4) n = (unsigned int)((size - def) / 4);
            compile_dump_puts( out, ", default {" );
            for (k = 0; k < n; k++)
            {
                union { unsigned int u; float f; } v;
                char text[32];
                int len;

                v.u = compile_dump_rd32( ctab + def + k * 4 );
                len = snprintf( text, sizeof(text), "%s%.9g", k ? ", " : "", v.f );
                if (len > 0) compile_dump_put( out, text, (size_t)len < sizeof(text) ? (size_t)len : sizeof(text) - 1 );
            }
            compile_dump_puts( out, n < regcount * 4 ? ", ...}" : "}" );
        }
        else if (def) compile_dump_puts( out, ", default" );
        compile_dump_puts( out, "\n" );
    }
}

/* the bytecode as hex, eight little-endian DWORDs a line, the byte offset first */
static inline void compile_dump_hex_code( struct compile_dump_out *out, const unsigned char *code, size_t size )
{
    size_t i;

    compile_dump_fmt( out, "// bytecode: %u bytes\n", (unsigned int)size, 0 );
    for (i = 0; i + 4 <= size; i += 4)
    {
        if (!(i % 32)) compile_dump_fmt( out, "//   %04x:", (unsigned int)i, 0 );
        compile_dump_fmt( out, " %08x", compile_dump_rd32( code + i ), 0 );
        if (i % 32 == 28 || i + 8 > size) compile_dump_puts( out, "\n" );
    }
}

/* ---- A25: macros, include files, the call stack ---- */

static inline void compile_dump_hex64( struct compile_dump_out *out, unsigned long long v )
{
    static const char hex[] = "0123456789abcdef";
    char text[19];
    int i, n = 0;

    text[n++] = '0';
    text[n++] = 'x';
    for (i = 60; i > 0 && !(v >> i); i -= 4) ;
    for (; i >= 0; i -= 4) text[n++] = hex[(v >> i) & 0xf];
    compile_dump_put( out, text, n );
}

static inline void compile_dump_dec( struct compile_dump_out *out, unsigned long long v )
{
    char text[21];
    int n = sizeof(text);

    do text[--n] = (char)('0' + v % 10); while (v /= 10);
    compile_dump_put( out, text + n, sizeof(text) - n );
}

/* s in double quotes, \ " and control characters escaped C-style, so a value stays on its line */
static inline void compile_dump_quoted( struct compile_dump_out *out, const char *s )
{
    static const char hex[] = "0123456789abcdef";

    compile_dump_puts( out, "\"" );
    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;
        char esc[4] = { '\\', 0, 0, 0 };

        if (c == '\\' || c == '"') { esc[1] = (char)c; compile_dump_put( out, esc, 2 ); }
        else if (c == '\n') compile_dump_puts( out, "\\n" );
        else if (c == '\r') compile_dump_puts( out, "\\r" );
        else if (c == '\t') compile_dump_puts( out, "\\t" );
        else if (c < 0x20 || c == 0x7f)
        {
            esc[1] = 'x';
            esc[2] = hex[c >> 4];
            esc[3] = hex[c & 0xf];
            compile_dump_put( out, esc, 4 );
        }
        else compile_dump_put( out, s, 1 );
    }
    compile_dump_puts( out, "\"" );
}

/* records one request to the include handler; data and size are what it returned (data NULL
 * when it failed). Past the limits the request is counted (or recorded without its text). */
static inline void compile_dump_includes_add( struct compile_dump_includes *inc, const char *name, int local,
                                              unsigned int result, const void *data, size_t size )
{
    struct compile_dump_include_file *file;

    if (inc->count >= COMPILE_DUMP_MAX_INCLUDES)
    {
        inc->dropped++;
        return;
    }
    file = &inc->files[inc->count++];
    memset( file, 0, sizeof(*file) );
    file->local = local;
    file->result = result;
    file->size = data ? size : 0;
    if (name && (file->name = malloc( strlen( name ) + 1 ))) strcpy( file->name, name );
    if (data && size <= COMPILE_DUMP_MAX_INCLUDE_BYTES - inc->kept && (file->text = malloc( size ? size : 1 )))
    {
        memcpy( file->text, data, size );
        inc->kept += size;
    }
}

static inline void compile_dump_includes_free( struct compile_dump_includes *inc )
{
    size_t i;

    for (i = 0; i < inc->count; i++)
    {
        free( inc->files[i].name );
        free( inc->files[i].text );
    }
    inc->count = inc->dropped = inc->kept = 0;
}

/* a DLL a program calls to compile or preprocess HLSL: d3dcompiler_NN, d3dx9_NN, d3dx10_NN,
 * d3dx11_NN, d3d10 and d3d10_1 (any case) */
static inline int compile_dump_front_module( const char *name )
{
    static const char *const prefixes[] = { "d3dcompiler_", "d3dx9_", "d3dx10_", "d3dx11_", "d3d10.", "d3d10_1." };
    size_t i, k;

    if (!name) return 0;
    for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
    {
        for (k = 0; prefixes[i][k]; k++)
        {
            char c = name[k];

            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != prefixes[i][k]) break;
        }
        if (!prefixes[i][k]) return 1;
    }
    return 0;
}

/* a PE image mapped at base: SizeOfImage from its headers (avail bytes readable there), 0 if not a PE */
static inline size_t compile_dump_pe_image_size( const unsigned char *base, size_t avail )
{
    unsigned int pe, magic;

    if (!base || avail < 0x40 || base[0] != 'M' || base[1] != 'Z') return 0;
    pe = compile_dump_rd32( base + 0x3c );
    if (pe > avail || avail - pe < 24 + 60 || memcmp( base + pe, "PE\0\0", 4 )) return 0;
    magic = compile_dump_rd16( base + pe + 24 );
    if (magic != 0x10b && magic != 0x20b) return 0;
    return compile_dump_rd32( base + pe + 24 + 56 );
}

/* the exported name whose address is the closest at or below rva, in a PE image mapped at base
 * (image_size bytes); forwarders don't count. NULL if there is none. */
static inline const char *compile_dump_nearest_export( const unsigned char *base, size_t image_size,
                                                       unsigned int rva, unsigned int *export_rva )
{
    unsigned int pe, magic, dir, count_dirs, exp, exp_size, nfuncs, nnames, funcs, names, ords, i, best = 0;
    const char *name = NULL;

    if (!base || image_size < 0x40) return NULL;
    pe = compile_dump_rd32( base + 0x3c );
    if (pe > image_size || image_size - pe < 24 + 112 + 8 || memcmp( base + pe, "PE\0\0", 4 )) return NULL;
    magic = compile_dump_rd16( base + pe + 24 );
    if (magic == 0x10b) dir = pe + 24 + 96, count_dirs = compile_dump_rd32( base + pe + 24 + 92 );
    else if (magic == 0x20b) dir = pe + 24 + 112, count_dirs = compile_dump_rd32( base + pe + 24 + 108 );
    else return NULL;
    if (!count_dirs || image_size - dir < 8) return NULL;
    exp = compile_dump_rd32( base + dir );
    exp_size = compile_dump_rd32( base + dir + 4 );
    if (!exp || exp > image_size || image_size - exp < 40) return NULL;
    nfuncs = compile_dump_rd32( base + exp + 0x14 );
    nnames = compile_dump_rd32( base + exp + 0x18 );
    funcs = compile_dump_rd32( base + exp + 0x1c );
    names = compile_dump_rd32( base + exp + 0x20 );
    ords = compile_dump_rd32( base + exp + 0x24 );
    if (funcs > image_size || nfuncs > (image_size - funcs) / 4) return NULL;
    if (names > image_size || nnames > (image_size - names) / 4) return NULL;
    if (ords > image_size || nnames > (image_size - ords) / 2) return NULL;
    for (i = 0; i < nnames; i++)
    {
        unsigned int ord = compile_dump_rd16( base + ords + 2 * (size_t)i ), f, n;

        if (ord >= nfuncs) continue;
        f = compile_dump_rd32( base + funcs + 4 * (size_t)ord );
        if (!f || f > rva || (name && f <= best)) continue;
        if (f >= exp && f - exp < exp_size) continue;   /* a forwarder: its "address" is a string */
        n = compile_dump_rd32( base + names + 4 * (size_t)i );
        if (n >= image_size || !memchr( base + n, 0, image_size - n )) continue;
        best = f;
        name = (const char *)base + n;
    }
    if (name && export_rva) *export_rva = best;
    return name;
}

static inline void compile_dump_frame_text( struct compile_dump_out *out, const struct compile_dump_frame *f )
{
    if (!f->module)
    {
        compile_dump_hex64( out, f->offset );
        return;
    }
    compile_dump_puts( out, f->module );
    if (f->symbol)
    {
        compile_dump_puts( out, "!" );
        compile_dump_puts( out, f->symbol );
    }
    compile_dump_puts( out, "+" );
    compile_dump_hex64( out, f->offset );
}

/* "// called through: <the outermost compiler DLL frame>, from <the frame that called it>": the
 * export the program called, in the d3dx or d3dcompiler DLL it called; then every frame */
static inline void compile_dump_frames( struct compile_dump_out *out, const struct compile_dump_frame *frames,
                                        size_t count )
{
    size_t i = 0, entry;

    compile_dump_puts( out, "// called through: " );
    while (i < count && frames[i].own) i++;
    if (i == count) compile_dump_puts( out, "unknown (no call stack)" );
    else if (!compile_dump_front_module( frames[i].module ))
    {
        compile_dump_puts( out, "no compiler DLL, called from " );
        compile_dump_frame_text( out, &frames[i] );
    }
    else
    {
        for (entry = i; entry + 1 < count && compile_dump_front_module( frames[entry + 1].module ); entry++) ;
        compile_dump_frame_text( out, &frames[entry] );
        compile_dump_puts( out, ", from " );
        if (entry + 1 < count) compile_dump_frame_text( out, &frames[entry + 1] );
        else compile_dump_puts( out, "-" );
    }
    compile_dump_puts( out, "\n// call stack:" );
    for (i = 0; i < count; i++)
    {
        compile_dump_puts( out, i ? " < " : " " );
        compile_dump_frame_text( out, &frames[i] );
    }
    compile_dump_puts( out, count ? "\n" : " -\n" );
}

static inline void compile_dump_macros( struct compile_dump_out *out, const struct compile_dump_info *info )
{
    const struct compile_dump_macro *m;
    size_t n = 0;

    if (!info->has_macros || !info->macros)
    {
        compile_dump_puts( out, "// macros: none (NULL)\n" );
        return;
    }
    for (m = info->macros; m->name; m++) n++;
    compile_dump_puts( out, "// macros: " );
    compile_dump_dec( out, n );
    compile_dump_puts( out, "\n" );
    for (m = info->macros; m->name; m++)
    {
        compile_dump_puts( out, "//   " );
        compile_dump_quoted( out, m->name );
        compile_dump_puts( out, " = " );
        if (m->definition) compile_dump_quoted( out, m->definition );
        else compile_dump_puts( out, "NULL" );
        compile_dump_puts( out, "\n" );
    }
}

static inline void compile_dump_include_header( struct compile_dump_out *out, size_t index,
                                                const struct compile_dump_include_file *f )
{
    compile_dump_dec( out, index + 1 );
    compile_dump_puts( out, ": " );
    if (f->name) compile_dump_quoted( out, f->name );
    else compile_dump_puts( out, "NULL" );
    compile_dump_puts( out, f->local ? ", local, " : ", system, " );
    if (f->result & 0x80000000u)
    {
        compile_dump_puts( out, "the handler failed, " );
        compile_dump_hex( out, f->result );
        return;
    }
    compile_dump_dec( out, f->size );
    compile_dump_puts( out, f->text ? " bytes" : " bytes, text not kept (the 4 MB limit)" );
}

static inline void compile_dump_includes( struct compile_dump_out *out, const struct compile_dump_includes *inc )
{
    size_t i;

    if (!inc)
    {
        compile_dump_puts( out, "// includes: no include handler\n" );
        return;
    }
    compile_dump_puts( out, "// includes: " );
    compile_dump_dec( out, inc->count + inc->dropped );
    compile_dump_puts( out, inc->count ? " asked, in this order; their text follows the source below\n" : " asked\n" );
    for (i = 0; i < inc->count; i++)
    {
        compile_dump_puts( out, "//   " );
        compile_dump_include_header( out, i, &inc->files[i] );
        compile_dump_puts( out, "\n" );
    }
    if (inc->dropped)
    {
        compile_dump_puts( out, "//   and " );
        compile_dump_dec( out, inc->dropped );
        compile_dump_puts( out, " more, not recorded\n" );
    }
}

/* ends a "\n//==== <what>" marker line, then the text exactly as given and an end marker line
 * (a newline before it when the text has none at its end) */
static inline void compile_dump_section( struct compile_dump_out *out, const char *text, size_t size )
{
    compile_dump_puts( out, " ====\n" );
    compile_dump_put( out, text, size );
    if (size && text[size - 1] != '\n') compile_dump_puts( out, "\n" );
    compile_dump_puts( out, "//==== end ====\n" );
}

/* writes at most size bytes to buf (none when buf is NULL); returns the file's full size */
static inline size_t compile_dump_format( char *buf, size_t size, const struct compile_dump_info *info )
{
    struct compile_dump_out out = { buf, size, 0 };
    const char *pos = info->messages, *end = info->messages ? info->messages + info->messages_size : NULL;
    size_t i;

    compile_dump_puts( &out, info->succeeded
            ? "// d3dcompiler: a successful compile, dumped for diagnosis (" COMPILE_DUMP_ALL_ENV "=1)\n"
            : info->preprocess_call
            ? "// d3dcompiler: a failed D3DPreprocess, dumped for diagnosis (" COMPILE_DUMP_ENV "=1)\n"
            : "// d3dcompiler: a failed compile, dumped for diagnosis (" COMPILE_DUMP_ENV "=1)\n" );
    compile_dump_field( &out, "source", info->source_name ? info->source_name : "(memory)" );
    compile_dump_field( &out, "entry", info->entry );
    compile_dump_field( &out, "target", info->target );
    compile_dump_puts( &out, "// flags: " );
    compile_dump_hex( &out, info->flags );
    compile_dump_puts( &out, "\n// result: " );
    compile_dump_hex( &out, info->result );
    compile_dump_puts( &out, "\n// compiler version: " );
    if (info->compiler_version) compile_dump_dec( &out, info->compiler_version );
    else compile_dump_puts( &out, "-" );
    compile_dump_puts( &out, "\n" );
    compile_dump_frames( &out, info->frames, info->frame_count );
    compile_dump_macros( &out, info );
    compile_dump_includes( &out, info->includes );
    compile_dump_puts( &out, info->preprocessed ? "// text: after preprocessing\n"
                                                : "// text: as given (preprocessing failed)\n" );
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
    if (info->code)     /* A19 */
    {
        compile_dump_ctab( &out, info->code, info->code_size );
        compile_dump_hex_code( &out, info->code, info->code_size );
    }
    compile_dump_puts( &out, "\n" );
    compile_dump_put( &out, info->text, info->text_size );
    if (info->text_size && info->text[info->text_size - 1] != '\n') compile_dump_puts( &out, "\n" );
    /* A25: the source as given (above is the preprocessed text), then the include files' text */
    if (info->source && info->preprocessed)
    {
        compile_dump_puts( &out, "\n//==== source as given: " );
        compile_dump_dec( &out, info->source_size );
        compile_dump_puts( &out, " bytes" );
        compile_dump_section( &out, info->source, info->source_size );
    }
    for (i = 0; info->includes && i < info->includes->count; i++)
    {
        const struct compile_dump_include_file *f = &info->includes->files[i];

        if (!f->text) continue;
        compile_dump_puts( &out, "\n//==== include " );
        compile_dump_include_header( &out, i, f );
        compile_dump_section( &out, f->text, f->size );
    }
    return out.len;
}

/* the text's size without the NULs a blob may end with */
static inline size_t compile_dump_text_size( const char *text, size_t size )
{
    while (size && !text[size - 1]) size--;
    return size;
}

#endif /* MYIOSDECK_COMPILE_FAILURE_DUMP_H */
