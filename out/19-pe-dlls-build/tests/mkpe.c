// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 19 Part C: writes a small synthetic PE image for the pecheck tests, from Microsoft's
// PE/COFF layout. Not a loadable program: just the headers, sections and tables pecheck reads.
//
//   mkpe <out> [--machine 0xNNNN] [--pe32] [--chpe] [--no-marker] [--align 0xNNNN]
//              [--data-first] [--debug-section] [--import <dll>]... [--delay <dll>]...
//              [--old-delay] [--data-only]

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_ALIGN 0x200
#define MAX_NAMES  16

static uint8_t image[0x4000];

static void wr16( uint32_t off, uint32_t v ) { image[off] = (uint8_t)v; image[off + 1] = (uint8_t)(v >> 8); }
static void wr32( uint32_t off, uint32_t v ) { wr16( off, v & 0xffff ); wr16( off + 2, v >> 16 ); }
static void wr64( uint32_t off, uint64_t v ) { wr32( off, (uint32_t)v ); wr32( off + 4, (uint32_t)(v >> 32) ); }

int main( int argc, char **argv )
{
    unsigned int machine = 0x8664, align = 0x10000;
    int pe32 = 0, chpe = 0, marker = 1, data_first = 0, debug = 0, old_delay = 0, data_only = 0, i;
    const char *imports[MAX_NAMES], *delays[MAX_NAMES];
    int nimports = 0, ndelays = 0;
    uint32_t pe = 0x80, opt, sect, nsect, opt_size, text_rva, rdata_rva, text_raw, rdata_raw;
    uint32_t debug_raw = 0, strtab, end, str_at;
    uint64_t base;
    FILE *f;

    if (argc < 2) { fprintf( stderr, "usage: mkpe <out> [options]\n" ); return 2; }
    for (i = 2; i < argc; i++)
    {
        if (!strcmp( argv[i], "--machine" ) && i + 1 < argc) machine = (unsigned int)strtoul( argv[++i], NULL, 0 );
        else if (!strcmp( argv[i], "--align" ) && i + 1 < argc) align = (unsigned int)strtoul( argv[++i], NULL, 0 );
        else if (!strcmp( argv[i], "--pe32" )) pe32 = 1;
        else if (!strcmp( argv[i], "--chpe" )) chpe = 1;
        else if (!strcmp( argv[i], "--no-marker" )) marker = 0;
        else if (!strcmp( argv[i], "--data-first" )) data_first = 1;
        else if (!strcmp( argv[i], "--debug-section" )) debug = 1;
        else if (!strcmp( argv[i], "--old-delay" )) old_delay = 1;
        else if (!strcmp( argv[i], "--data-only" )) data_only = 1;
        else if (!strcmp( argv[i], "--import" ) && i + 1 < argc && nimports < MAX_NAMES) imports[nimports++] = argv[++i];
        else if (!strcmp( argv[i], "--delay" ) && i + 1 < argc && ndelays < MAX_NAMES) delays[ndelays++] = argv[++i];
        else { fprintf( stderr, "mkpe: bad option %s\n", argv[i] ); return 2; }
    }
    base = pe32 ? 0x10000000u : 0x180000000ull;

    /* DOS header and the builtin marker */
    image[0] = 'M';
    image[1] = 'Z';
    wr32( 0x3c, pe );
    if (marker) memcpy( image + 0x40, "Wine builtin DLL", 16 );

    /* sections: .text and .rdata (in that address order unless data_first), maybe .debug_info */
    nsect = debug ? 3 : 2;
    opt_size = pe32 ? 96 + 16 * 8 : 112 + 16 * 8;
    opt = pe + 24;
    sect = opt + opt_size;
    text_raw = FILE_ALIGN;
    rdata_raw = 2 * FILE_ALIGN;
    if (debug) debug_raw = 5 * FILE_ALIGN;
    text_rva = data_first ? 2 * align : align;
    rdata_rva = data_first ? align : 2 * align;
    strtab = (debug ? debug_raw + FILE_ALIGN : 5 * FILE_ALIGN);
    end = strtab + 4 + 16;

    /* PE signature and COFF header */
    memcpy( image + pe, "PE\0\0", 4 );
    wr16( pe + 4, machine );
    wr16( pe + 6, nsect );
    wr32( pe + 12, debug ? strtab : 0 );  /* PointerToSymbolTable (only for the string table) */
    wr32( pe + 16, 0 );                   /* NumberOfSymbols */
    wr16( pe + 20, opt_size );
    wr16( pe + 22, 0x2022 );

    /* optional header */
    wr16( opt, pe32 ? 0x10b : 0x20b );
    wr32( opt + 16, text_rva );           /* AddressOfEntryPoint */
    if (pe32) wr32( opt + 28, (uint32_t)base ); else wr64( opt + 24, base );
    wr32( opt + 32, align );
    wr32( opt + 36, FILE_ALIGN );
    wr32( opt + 56, 4 * align );          /* SizeOfImage */
    wr32( opt + 60, FILE_ALIGN );         /* SizeOfHeaders */
    wr32( opt + (pe32 ? 92 : 108), 16 );  /* NumberOfRvaAndSizes */
    {
        uint32_t dirs = opt + (pe32 ? 96 : 112);
        if (nimports) { wr32( dirs + 1 * 8, rdata_rva ); wr32( dirs + 1 * 8 + 4, (nimports + 1) * 20 ); }
        if (chpe) { wr32( dirs + 10 * 8, rdata_rva + 0x300 ); wr32( dirs + 10 * 8 + 4, 0x140 ); }
        if (ndelays) { wr32( dirs + 13 * 8, rdata_rva + 0x100 ); wr32( dirs + 13 * 8 + 4, (ndelays + 1) * 32 ); }
    }

    /* section table */
    memcpy( image + sect, data_only ? ".edata" : ".text", data_only ? 6 : 5 );
    wr32( sect + 8, 0x10 );
    wr32( sect + 12, text_rva );
    wr32( sect + 16, FILE_ALIGN );
    wr32( sect + 20, text_raw );
    wr32( sect + 36, data_only ? 0x40000040 : 0x60000020 );  /* data only, or code, execute, read */
    memcpy( image + sect + 40, ".rdata", 6 );
    wr32( sect + 40 + 8, 3 * FILE_ALIGN );
    wr32( sect + 40 + 12, rdata_rva );
    wr32( sect + 40 + 16, 3 * FILE_ALIGN );
    wr32( sect + 40 + 20, rdata_raw );
    wr32( sect + 40 + 36, 0x40000040 );   /* initialized data, read */
    if (debug)
    {
        memcpy( image + sect + 80, "/4", 2 );   /* ".debug_info", at offset 4 of the string table */
        wr32( sect + 80 + 8, 0x10 );
        wr32( sect + 80 + 12, 3 * align );
        wr32( sect + 80 + 16, FILE_ALIGN );
        wr32( sect + 80 + 20, debug_raw );
        wr32( sect + 80 + 36, 0x42000040 );     /* discardable data */
        wr32( strtab, 4 + 12 );
        memcpy( image + strtab + 4, ".debug_info", 11 );
    }

    /* .text: a ret */
    image[text_raw] = 0xc3;

    /* .rdata: import descriptors at +0, delay descriptors at +0x100, names from +0x200,
     * the load configuration at +0x300 */
    str_at = 0x200;
    for (i = 0; i < nimports; i++)
    {
        wr32( rdata_raw + i * 20 + 12, rdata_rva + str_at );
        wr32( rdata_raw + i * 20 + 16, rdata_rva + 0x3f0 );  /* FirstThunk (unused here) */
        strcpy( (char *)image + rdata_raw + str_at, imports[i] );
        str_at += (uint32_t)strlen( imports[i] ) + 1;
    }
    for (i = 0; i < ndelays; i++)
    {
        uint32_t d = rdata_raw + 0x100 + i * 32;
        wr32( d, old_delay ? 0 : 1 );
        wr32( d + 4, old_delay ? (uint32_t)base + rdata_rva + str_at : rdata_rva + str_at );
        strcpy( (char *)image + rdata_raw + str_at, delays[i] );
        str_at += (uint32_t)strlen( delays[i] ) + 1;
    }
    if (str_at > 0x300) { fprintf( stderr, "mkpe: names too long\n" ); return 2; }
    if (chpe)
    {
        wr32( rdata_raw + 0x300, 0x140 );                        /* Size */
        wr64( rdata_raw + 0x300 + 0xc8, base + rdata_rva + 0x380 );  /* CHPEMetadataPointer */
    }

    if (!(f = fopen( argv[1], "wb" )) || fwrite( image, 1, end, f ) != end || fclose( f ))
    {
        perror( argv[1] );
        return 1;
    }
    return 0;
}
