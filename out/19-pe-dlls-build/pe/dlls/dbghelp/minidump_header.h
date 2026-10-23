/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 the MYIOSDECK author */

/*
 * Spec 19 Part A6: the bytes of a header-only minidump. Layout from Microsoft's documentation of
 * MINIDUMP_HEADER (all fields little-endian):
 *   0 Signature (ULONG32)   4 Version (ULONG32)    8 NumberOfStreams (ULONG32)
 *  12 StreamDirectoryRva    16 CheckSum (ULONG32)  20 TimeDateStamp (ULONG32)
 *  24 Flags (ULONG64)                              32 = the header's size
 * Pure code shared by minidump.c and the host tests.
 */

#ifndef MYIOSDECK_MINIDUMP_HEADER_H
#define MYIOSDECK_MINIDUMP_HEADER_H

#define MINIDUMP_HEADER_ONLY_SIZE      32
#define MINIDUMP_HEADER_ONLY_SIGNATURE 0x504d444du   /* 'MDMP' */
#define MINIDUMP_HEADER_ONLY_VERSION   42899u        /* MINIDUMP_VERSION */

static inline void minidump_put32( unsigned char *p, unsigned long v )
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* a header with no streams, its (empty) stream directory right after itself, the given time
 * and the requested dump type as its flags */
static inline void minidump_header_only( unsigned char out[MINIDUMP_HEADER_ONLY_SIZE],
                                         unsigned long time_stamp, unsigned long long dump_type )
{
    minidump_put32( out + 0, MINIDUMP_HEADER_ONLY_SIGNATURE );
    minidump_put32( out + 4, MINIDUMP_HEADER_ONLY_VERSION );
    minidump_put32( out + 8, 0 );                           /* NumberOfStreams */
    minidump_put32( out + 12, MINIDUMP_HEADER_ONLY_SIZE );  /* StreamDirectoryRva */
    minidump_put32( out + 16, 0 );                          /* CheckSum */
    minidump_put32( out + 20, time_stamp );
    minidump_put32( out + 24, (unsigned long)(dump_type & 0xffffffffu) );
    minidump_put32( out + 28, (unsigned long)(dump_type >> 32) );
}

#endif /* MYIOSDECK_MINIDUMP_HEADER_H */
