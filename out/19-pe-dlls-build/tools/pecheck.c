// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 19 Part B5.2: checks a farm folder the way the owner's dylib converter will see it, which
// can't run here. Built with the host compiler. Layouts from Microsoft's PE/COFF documentation
// (DOS header e_lfanew, COFF file header, optional header PE32/PE32+, data directories, section
// table and long section names, import and delay-import tables, the 64-bit load configuration and
// its CHPE metadata pointer, which ARM64EC and ARM64X images carry).
//
//   pecheck <farm name> <folder>   check every file of a farm; exit status 1 on any error
//   pecheck --imports <file>       print "import <dll>" and "delay <dll>" lines
//   pecheck --machine <file>       print i386, x86_64, x86_64-data (no code), arm64ec, aarch64, arm64x, other:0x....,
//                                  or - for a file that isn't a PE image
//   pecheck --list-provided        print the DLLs other parts provide (Part B4 item 5)
//
// Farm names: arm64ec-windows, aarch64-windows, i386-windows.

#define _POSIX_C_SOURCE 200809L   /* lstat, strdup */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- the names Part B4 items 4 and 5 treat as present ---- */

static const char *const provided_names[] =
{
    "d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll", "winemetal.dll", "nvapi64.dll", /* DXMT */
    "d3d12.dll", "d3d12core.dll",                   /* the owner's Direct3D 12 */
    "xtajit64.dll", "xtajit.dll",                   /* the app's CPU modules */
};

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- PE constants ---- */

#define MACHINE_I386     0x014c
#define MACHINE_AMD64    0x8664
#define MACHINE_ARM64    0xaa64
#define MACHINE_ARM64X   0xa64e
#define MAGIC_PE32       0x10b
#define MAGIC_PE32PLUS   0x20b
#define SCN_CNT_CODE     0x00000020u
#define SCN_MEM_EXECUTE  0x20000000u
#define DIR_IMPORT       1
#define DIR_LOAD_CONFIG  10
#define DIR_DELAY_IMPORT 13
#define LOADCFG64_CHPE   0xc8   /* IMAGE_LOAD_CONFIG_DIRECTORY64.CHPEMetadataPointer */
#define BUILTIN_OFFSET   0x40
#define BUILTIN_SIZE     32

static const char builtin_marker[BUILTIN_SIZE] = "Wine builtin DLL";  /* and 16 NULs */

/* ---- small helpers ---- */

static int str_ieq( const char *a, const char *b )
{
    for (; *a && *b; a++, b++)
        if (tolower( (unsigned char)*a ) != tolower( (unsigned char)*b )) return 0;
    return !*a && !*b;
}

static int str_iprefix( const char *s, const char *prefix )
{
    for (; *prefix; s++, prefix++)
        if (tolower( (unsigned char)*s ) != tolower( (unsigned char)*prefix )) return 0;
    return 1;
}

static int str_isuffix( const char *s, const char *suffix )
{
    size_t ls = strlen( s ), lx = strlen( suffix );
    return ls >= lx && str_ieq( s + ls - lx, suffix );
}

static uint16_t rd16( const uint8_t *p ) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32( const uint8_t *p ) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64( const uint8_t *p ) { return rd32( p ) | (uint64_t)rd32( p + 4 ) << 32; }

static int is_provided( const char *name )
{
    size_t i;
    for (i = 0; i < ARRAY_COUNT(provided_names); i++)
        if (str_ieq( name, provided_names[i] )) return 1;
    return 0;
}

static int is_api_set( const char *name )
{
    return str_iprefix( name, "api-ms-win-" ) || str_iprefix( name, "ext-ms-win-" );
}

/* .tlb and .msstyles files are copied as they are and not checked as images */
static int is_data_file( const char *name )
{
    return str_isuffix( name, ".tlb" ) || str_isuffix( name, ".msstyles" );
}

/* ---- the image ---- */

struct section
{
    char name[64];
    uint32_t va, vsize, raw_ptr, raw_size, chars;
};

struct image
{
    uint8_t *data;
    size_t size;
    uint16_t machine;
    uint16_t magic;
    uint64_t image_base;
    uint32_t section_align, size_of_image, size_of_headers;
    uint32_t dir_count;
    uint32_t dir_rva[16], dir_size[16];
    unsigned int nsections;
    struct section *sections;
    char error[256];
};

static int image_fail( struct image *img, const char *fmt, ... )
{
    va_list args;
    va_start( args, fmt );
    vsnprintf( img->error, sizeof(img->error), fmt, args );
    va_end( args );
    return 0;
}

/* file offset of len bytes at rva, or -1 */
static long rva_to_offset( const struct image *img, uint32_t rva, uint32_t len )
{
    unsigned int i;

    if (rva < img->size_of_headers)
        return (uint64_t)rva + len <= img->size ? (long)rva : -1;
    for (i = 0; i < img->nsections; i++)
    {
        const struct section *s = &img->sections[i];
        uint64_t off;

        if (rva < s->va || rva - s->va >= s->raw_size) continue;
        if ((uint64_t)(rva - s->va) + len > s->raw_size) return -1;
        off = (uint64_t)s->raw_ptr + (rva - s->va);
        return off + len <= img->size ? (long)off : -1;
    }
    return -1;
}

/* the NUL-terminated string at rva (at most 255 characters), or NULL */
static const char *rva_string( const struct image *img, uint32_t rva )
{
    long off = rva_to_offset( img, rva, 1 );
    size_t i;

    if (off < 0) return NULL;
    for (i = 0; i < 256 && (size_t)off + i < img->size; i++)
        if (!img->data[off + i]) return (const char *)img->data + off;
    return NULL;
}

static void image_free( struct image *img )
{
    free( img->data );
    free( img->sections );
    memset( img, 0, sizeof(*img) );
}

/* reads and parses a file; 0 with img->error set when it isn't a PE image */
static int image_load( struct image *img, const char *path )
{
    FILE *f;
    long len;
    uint32_t pe, opt, sect, symtab, nsyms, i;
    uint16_t opt_size;

    memset( img, 0, sizeof(*img) );
    if (!(f = fopen( path, "rb" ))) return image_fail( img, "can't open: %s", strerror( errno ) );
    if (fseek( f, 0, SEEK_END ) || (len = ftell( f )) < 0 || fseek( f, 0, SEEK_SET ))
    {
        fclose( f );
        return image_fail( img, "can't read" );
    }
    img->size = (size_t)len;
    if (!(img->data = malloc( img->size ? img->size : 1 )) || fread( img->data, 1, img->size, f ) != img->size)
    {
        fclose( f );
        return image_fail( img, "can't read" );
    }
    fclose( f );

    if (img->size < 0x40 || img->data[0] != 'M' || img->data[1] != 'Z') return image_fail( img, "not a PE image (no MZ header)" );
    pe = rd32( img->data + 0x3c );
    if ((uint64_t)pe + 24 > img->size || memcmp( img->data + pe, "PE\0\0", 4 ))
        return image_fail( img, "not a PE image (no PE signature)" );
    img->machine = rd16( img->data + pe + 4 );
    img->nsections = rd16( img->data + pe + 6 );
    symtab = rd32( img->data + pe + 12 );
    nsyms = rd32( img->data + pe + 16 );
    opt_size = rd16( img->data + pe + 20 );
    opt = pe + 24;
    sect = opt + opt_size;
    if (opt_size < 2 || (uint64_t)sect + (uint64_t)img->nsections * 40 > img->size)
        return image_fail( img, "truncated headers" );
    img->magic = rd16( img->data + opt );
    if (img->magic == MAGIC_PE32PLUS)
    {
        if (opt_size < 112) return image_fail( img, "truncated optional header" );
        img->image_base = rd64( img->data + opt + 24 );
        img->dir_count = rd32( img->data + opt + 108 );
        opt += 112;
        opt_size -= 112;
    }
    else if (img->magic == MAGIC_PE32)
    {
        if (opt_size < 96) return image_fail( img, "truncated optional header" );
        img->image_base = rd32( img->data + opt + 28 );
        img->dir_count = rd32( img->data + opt + 92 );
        opt += 96;
        opt_size -= 96;
    }
    else return image_fail( img, "unknown optional header magic %#x", img->magic );
    img->section_align = rd32( img->data + pe + 24 + 32 );
    img->size_of_image = rd32( img->data + pe + 24 + 56 );
    img->size_of_headers = rd32( img->data + pe + 24 + 60 );
    if (img->dir_count > 16) img->dir_count = 16;
    if (img->dir_count * 8 > opt_size) img->dir_count = opt_size / 8;
    for (i = 0; i < img->dir_count; i++)
    {
        img->dir_rva[i] = rd32( img->data + opt + i * 8 );
        img->dir_size[i] = rd32( img->data + opt + i * 8 + 4 );
    }

    if (!(img->sections = calloc( img->nsections ? img->nsections : 1, sizeof(*img->sections) )))
        return image_fail( img, "out of memory" );
    for (i = 0; i < img->nsections; i++)
    {
        const uint8_t *s = img->data + sect + i * 40;
        struct section *out = &img->sections[i];

        memcpy( out->name, s, 8 );
        out->name[8] = 0;
        /* a long name: "/<decimal offset>" into the COFF string table after the symbols */
        if (out->name[0] == '/' && symtab)
        {
            uint64_t strtab = (uint64_t)symtab + (uint64_t)nsyms * 18, at;
            unsigned long n = strtoul( out->name + 1, NULL, 10 );
            size_t k;

            at = strtab + n;
            for (k = 0; at + k < img->size && k + 1 < sizeof(out->name); k++)
                if (!(out->name[k] = (char)img->data[at + k])) break;
            out->name[k] = 0;
        }
        out->vsize = rd32( s + 8 );
        out->va = rd32( s + 12 );
        out->raw_size = rd32( s + 16 );
        out->raw_ptr = rd32( s + 20 );
        out->chars = rd32( s + 36 );
    }
    return 1;
}

/* the CHPE metadata pointer of a PE32+ image's load configuration, 0 without one */
static uint64_t image_chpe_pointer( const struct image *img )
{
    uint32_t rva, size, cfg_size;
    long off;

    if (img->magic != MAGIC_PE32PLUS || img->dir_count <= DIR_LOAD_CONFIG) return 0;
    rva = img->dir_rva[DIR_LOAD_CONFIG];
    size = img->dir_size[DIR_LOAD_CONFIG];
    if (!rva || (off = rva_to_offset( img, rva, 4 )) < 0) return 0;
    cfg_size = rd32( img->data + off );
    if (cfg_size < LOADCFG64_CHPE + 8 || (size && size < LOADCFG64_CHPE + 8)) return 0;
    if ((off = rva_to_offset( img, rva + LOADCFG64_CHPE, 8 )) < 0) return 0;
    return rd64( img->data + off );
}

/* does any section hold code or execute? */
static int image_has_code( const struct image *img )
{
    unsigned int i;
    for (i = 0; i < img->nsections; i++)
        if (img->sections[i].chars & (SCN_CNT_CODE | SCN_MEM_EXECUTE)) return 1;
    return 0;
}

static const char *machine_word( const struct image *img, char *buf, size_t len )
{
    switch (img->machine)
    {
    case MACHINE_I386:   return "i386";
    case MACHINE_AMD64:
        if (image_chpe_pointer( img )) return "arm64ec";
        return image_has_code( img ) ? "x86_64" : "x86_64-data";
    case MACHINE_ARM64:  return image_chpe_pointer( img ) ? "arm64x" : "aarch64";
    case MACHINE_ARM64X: return "arm64x";
    default:
        snprintf( buf, len, "other:0x%04x", img->machine );
        return buf;
    }
}

/* calls fn(name, delayed) for every imported DLL; 0 with img->error on a broken table */
static int image_imports( struct image *img, void (*fn)( const char *, int, void * ), void *ctx )
{
    uint32_t rva;
    unsigned int i;
    long off;

    if (img->dir_count > DIR_IMPORT && (rva = img->dir_rva[DIR_IMPORT]))
    {
        for (i = 0;; i++)
        {
            const char *name;
            uint32_t name_rva;

            if (i > 4096 || (off = rva_to_offset( img, rva + i * 20, 20 )) < 0)
                return image_fail( img, "broken import table" );
            name_rva = rd32( img->data + off + 12 );
            if (!name_rva && !rd32( img->data + off ) && !rd32( img->data + off + 16 )) break;
            if (!(name = rva_string( img, name_rva ))) return image_fail( img, "broken import name" );
            fn( name, 0, ctx );
        }
    }
    if (img->dir_count > DIR_DELAY_IMPORT && (rva = img->dir_rva[DIR_DELAY_IMPORT]))
    {
        for (i = 0;; i++)
        {
            const char *name;
            uint32_t attrs, name_ref;

            if (i > 4096 || (off = rva_to_offset( img, rva + i * 32, 32 )) < 0)
                return image_fail( img, "broken delay-import table" );
            attrs = rd32( img->data + off );
            name_ref = rd32( img->data + off + 4 );
            if (!name_ref) break;
            /* attribute bit 0 clear: the old format, with virtual addresses */
            if (!(attrs & 1)) name_ref = (uint32_t)(name_ref - (uint32_t)img->image_base);
            if (!(name = rva_string( img, name_ref ))) return image_fail( img, "broken delay-import name" );
            fn( name, 1, ctx );
        }
    }
    return 1;
}

/* ---- --imports, --machine ---- */

static void print_import( const char *name, int delayed, void *ctx )
{
    (void)ctx;
    printf( "%s %s\n", delayed ? "delay" : "import", name );
}

static int cmd_imports( const char *path )
{
    struct image img;
    int ok = image_load( &img, path ) && image_imports( &img, print_import, NULL );

    if (!ok) fprintf( stderr, "pecheck: %s: %s\n", path, img.error );
    image_free( &img );
    return ok ? 0 : 1;
}

static int cmd_machine( const char *path )
{
    struct image img;
    char buf[32];

    if (image_load( &img, path )) printf( "%s\n", machine_word( &img, buf, sizeof(buf) ) );
    else printf( "-\n" );
    image_free( &img );
    return 0;
}

/* ---- the farm check ---- */

struct entry
{
    char *name;
    unsigned long long size;
};

struct farm
{
    const char *farm;
    const char *dir;
    struct entry *files;
    size_t count;
    unsigned int errors;
    /* delay imports not in the farm, "dll\timporter" */
    char **delays;
    size_t ndelays, delays_cap;
};

static void farm_error( struct farm *farm, const char *file, const char *fmt, ... )
{
    va_list args;

    fprintf( stderr, "ERROR %s/%s: ", farm->farm, file );
    va_start( args, fmt );
    vfprintf( stderr, fmt, args );
    va_end( args );
    fputc( '\n', stderr );
    farm->errors++;
}

static int farm_has( const struct farm *farm, const char *name )
{
    size_t i;
    for (i = 0; i < farm->count; i++)
        if (str_ieq( farm->files[i].name, name )) return 1;
    return 0;
}

struct import_ctx
{
    struct farm *farm;
    const char *file;
};

static void check_import( const char *name, int delayed, void *ctx_ )
{
    struct import_ctx *ctx = ctx_;
    struct farm *farm = ctx->farm;
    char *line;

    if (farm_has( farm, name ) || is_provided( name ) || is_api_set( name )) return;
    if (!delayed)
    {
        farm_error( farm, ctx->file, "imports %s, which is neither in the farm, provided by another part nor an API set", name );
        return;
    }
    if (farm->ndelays == farm->delays_cap)
    {
        size_t cap = farm->delays_cap ? farm->delays_cap * 2 : 64;
        char **grown = realloc( farm->delays, cap * sizeof(*grown) );
        if (!grown) return;
        farm->delays = grown;
        farm->delays_cap = cap;
    }
    if (!(line = malloc( strlen( name ) + strlen( ctx->file ) + 2 ))) return;
    sprintf( line, "%s\t%s", name, ctx->file );
    for (char *p = line; *p != '\t'; p++) *p = (char)tolower( (unsigned char)*p );
    farm->delays[farm->ndelays++] = line;
}

static int cmp_section_va( const void *a, const void *b )
{
    const struct section *const *x = a, *const *y = b;
    return (*x)->va < (*y)->va ? -1 : (*x)->va > (*y)->va;
}

static void check_file( struct farm *farm, const char *file )
{
    char path[4096], buf[32];
    struct image img;
    struct import_ctx ctx = { farm, file };
    int is64 = strcmp( farm->farm, "i386-windows" ) != 0;
    uint64_t chpe;

    snprintf( path, sizeof(path), "%s/%s", farm->dir, file );
    if (!image_load( &img, path ))
    {
        farm_error( farm, file, "%s", img.error );
        image_free( &img );
        return;
    }

    if (img.size < BUILTIN_OFFSET + BUILTIN_SIZE || rd32( img.data + 0x3c ) < BUILTIN_OFFSET + BUILTIN_SIZE ||
        memcmp( img.data + BUILTIN_OFFSET, builtin_marker, BUILTIN_SIZE ))
        farm_error( farm, file, "no Wine builtin marker at 0x40 (Wine's loader would treat it as a native DLL)" );

    chpe = image_chpe_pointer( &img );
    if (img.machine == MACHINE_ARM64X || (img.machine == MACHINE_ARM64 && chpe))
        farm_error( farm, file, "ARM64X image (machine %#x%s): build arm64ec and aarch64 in separate trees",
                    img.machine, chpe ? " with hybrid metadata" : "" );
    else if (!strcmp( farm->farm, "arm64ec-windows" ))
    {
        /* a data-only module (winebuild --data-only: exports or the API set schema, no code
         * section) has nothing for ARM64EC metadata to describe, and Wine builds it without a
         * load configuration */
        if (img.machine == MACHINE_AMD64 && !chpe && !image_has_code( &img ))
            printf( "note %s/%s: data-only image (no code section), accepted without ARM64EC metadata\n",
                    farm->farm, file );
        else if (img.machine == MACHINE_AMD64 && !chpe)
            farm_error( farm, file, "plain x86-64 image (machine 0x8664 without ARM64EC metadata in its load configuration)" );
        else if (img.machine != MACHINE_AMD64)
            farm_error( farm, file, "machine %s, expected arm64ec (0x8664 with ARM64EC metadata)", machine_word( &img, buf, sizeof(buf) ) );
    }
    else if (!strcmp( farm->farm, "aarch64-windows" ))
    {
        if (img.machine != MACHINE_ARM64)
            farm_error( farm, file, "machine %s, expected aarch64 (0xaa64)", machine_word( &img, buf, sizeof(buf) ) );
    }
    else if (img.machine != MACHINE_I386)
        farm_error( farm, file, "machine %s, expected i386 (0x14c)", machine_word( &img, buf, sizeof(buf) ) );

    if (is64)
    {
        struct section **order;
        unsigned int i, first_data = img.nsections;

        if (img.magic != MAGIC_PE32PLUS) farm_error( farm, file, "not PE32+ (magic %#x)", img.magic );
        if (!img.section_align || img.section_align % 0x4000)
            farm_error( farm, file, "section alignment %#x is not a multiple of 0x4000", img.section_align );
        else if (img.section_align != 0x10000)
            printf( "note %s/%s: section alignment %#x (expected 0x10000)\n", farm->farm, file, img.section_align );

        /* code first: every executable or code section before every other one, by address */
        if ((order = malloc( (img.nsections ? img.nsections : 1) * sizeof(*order) )))
        {
            for (i = 0; i < img.nsections; i++) order[i] = &img.sections[i];
            qsort( order, img.nsections, sizeof(*order), cmp_section_va );
            for (i = 0; i < img.nsections; i++)
            {
                int code = (order[i]->chars & (SCN_CNT_CODE | SCN_MEM_EXECUTE)) != 0;
                if (!code && first_data == img.nsections) first_data = i;
                else if (code && first_data < img.nsections)
                    farm_error( farm, file, "code section %s at %#x comes after section %s at %#x, which isn't code",
                                order[i]->name, order[i]->va, order[first_data]->name, order[first_data]->va );
            }
            free( order );
        }
        for (i = 0; i < img.nsections; i++)
            if (!strncmp( img.sections[i].name, ".debug", 6 ))
                farm_error( farm, file, "section %s left (strip --strip-debug)", img.sections[i].name );
    }

    if (!image_imports( &img, check_import, &ctx )) farm_error( farm, file, "%s", img.error );
    image_free( &img );
}

static int cmp_entry_size( const void *a, const void *b )
{
    const struct entry *x = a, *y = b;
    if (x->size != y->size) return x->size < y->size ? 1 : -1;
    return strcmp( x->name, y->name );
}

static int cmp_entry_name( const void *a, const void *b )
{
    const struct entry *x = a, *y = b;
    return strcmp( x->name, y->name );
}

static int cmp_str( const void *a, const void *b )
{
    return strcmp( *(char *const *)a, *(char *const *)b );
}

static int cmd_farm( const char *farm_name, const char *dir_path )
{
    struct farm farm = { farm_name, dir_path, NULL, 0, 0, NULL, 0, 0 };
    size_t cap = 0, i;
    unsigned long long total = 0;
    struct dirent *de;
    DIR *dir;

    if (strcmp( farm_name, "arm64ec-windows" ) && strcmp( farm_name, "aarch64-windows" ) &&
        strcmp( farm_name, "i386-windows" ))
    {
        fprintf( stderr, "pecheck: unknown farm %s (arm64ec-windows, aarch64-windows or i386-windows)\n", farm_name );
        return 2;
    }
    if (!(dir = opendir( dir_path )))
    {
        fprintf( stderr, "pecheck: %s: %s\n", dir_path, strerror( errno ) );
        return 2;
    }
    /* B6: flat, only regular files, no symlinks, no hidden files */
    while ((de = readdir( dir )))
    {
        char path[4096];
        struct stat st;

        if (!strcmp( de->d_name, "." ) || !strcmp( de->d_name, ".." )) continue;
        snprintf( path, sizeof(path), "%s/%s", dir_path, de->d_name );
        if (de->d_name[0] == '.') { farm_error( &farm, de->d_name, "hidden file in a farm folder" ); continue; }
        if (lstat( path, &st )) { farm_error( &farm, de->d_name, "%s", strerror( errno ) ); continue; }
        if (S_ISLNK( st.st_mode )) { farm_error( &farm, de->d_name, "symlink in a farm folder" ); continue; }
        if (!S_ISREG( st.st_mode )) { farm_error( &farm, de->d_name, "not a regular file (farm folders are flat)" ); continue; }
        if (farm.count == cap)
        {
            struct entry *grown;
            cap = cap ? cap * 2 : 256;
            if (!(grown = realloc( farm.files, cap * sizeof(*grown) ))) { closedir( dir ); return 2; }
            farm.files = grown;
        }
        farm.files[farm.count].name = strdup( de->d_name );
        farm.files[farm.count].size = (unsigned long long)st.st_size;
        total += (unsigned long long)st.st_size;
        farm.count++;
    }
    closedir( dir );
    if (farm.count) qsort( farm.files, farm.count, sizeof(*farm.files), cmp_entry_name );

    for (i = 0; i < farm.count; i++)
        if (!is_data_file( farm.files[i].name )) check_file( &farm, farm.files[i].name );

    /* delay imports: reported, never an error */
    if (farm.ndelays)
    {
        const char *last = "";

        qsort( farm.delays, farm.ndelays, sizeof(*farm.delays), cmp_str );
        printf( "%s: delay-imported DLLs not in the farm:\n", farm_name );
        for (i = 0; i < farm.ndelays; i++)
        {
            char *tab = strchr( farm.delays[i], '\t' );
            *tab = 0;
            if (strcmp( farm.delays[i], last )) printf( "%s  %s:", i ? "\n" : "", farm.delays[i] );
            printf( " %s", tab + 1 );
            last = farm.delays[i];
        }
        printf( "\n" );
    }

    /* the table */
    printf( "%s: %zu files, %llu bytes (%.1f MB)\n", farm_name, farm.count, total, total / 1048576.0 );
    if (farm.count)
    {
        qsort( farm.files, farm.count, sizeof(*farm.files), cmp_entry_size );
        printf( "%s: largest files:\n", farm_name );
        for (i = 0; i < farm.count && i < 10; i++)
            printf( "  %12llu  %s\n", farm.files[i].size, farm.files[i].name );
    }
    printf( "%s: %u error(s)\n", farm_name, farm.errors );

    for (i = 0; i < farm.count; i++) free( farm.files[i].name );
    for (i = 0; i < farm.ndelays; i++) free( farm.delays[i] );
    free( farm.files );
    free( farm.delays );
    return farm.errors ? 1 : 0;
}

static int usage(void)
{
    fprintf( stderr,
             "usage: pecheck <farm name> <folder>\n"
             "       pecheck --imports <file>\n"
             "       pecheck --machine <file>\n"
             "       pecheck --list-provided\n" );
    return 2;
}

int main( int argc, char **argv )
{
    size_t i;

    if (argc == 2 && !strcmp( argv[1], "--list-provided" ))
    {
        for (i = 0; i < ARRAY_COUNT(provided_names); i++) printf( "%s\n", provided_names[i] );
        return 0;
    }
    if (argc != 3) return usage();
    if (!strcmp( argv[1], "--imports" )) return cmd_imports( argv[2] );
    if (!strcmp( argv[1], "--machine" )) return cmd_machine( argv[2] );
    if (argv[1][0] == '-') return usage();
    return cmd_farm( argv[1], argv[2] );
}
