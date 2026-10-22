// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F, WMA rules: the derived-parameter function against FFmpeg's own decoder for a
// grid of rates, channels, bit rates and flags (wma_reference.c); the candidate list (order,
// dedupe, cap, the byte_offset_bits limit); the good-packet rule with each slack; the implied
// rate; the xWMA synthetic-data rule (winegstreamer-unix/wma_params.c).

#include <stdint.h>
#include <string.h>

#include "check.h"
#include "wma_params.h"

int wma_reference_derive( int version, int rate, int channels, int64_t bit_rate, unsigned int flags,
                          size_t extradata_size, struct wma_derived *out );

static void test_against_ffmpeg( void )
{
    static const int rates[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000 };
    static const int64_t bit_rates[] = { 4000, 8000, 12000, 16000, 20000, 24000, 32000, 40000, 48000, 64000, 80000,
                                         96000, 128000, 160000, 192000, 256000, 320000, 640000 };
    static const unsigned int flag_words[] = { 0x0000, 0x0001, 0x0002, 0x0003, 0x0005, 0x0007, 0x000d, 0x000f,
                                               0x0017, 0x001f, 0x000e, 0x0006 };
    static const struct { int version; size_t extradata; } layouts[] = { { 1, 4 }, { 2, 6 }, { 2, 10 } };
    unsigned int r, b, f, l;
    int channels, compared = 0;

    for (l = 0; l < sizeof(layouts) / sizeof(layouts[0]); l++)
    for (r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
    for (channels = 1; channels <= 3; channels++)
    for (b = 0; b < sizeof(bit_rates) / sizeof(bit_rates[0]); b++)
    for (f = 0; f < sizeof(flag_words) / sizeof(flag_words[0]); f++)
    {
        struct wma_derived mine, ref;
        int version = layouts[l].version, rm, rr;

        rm = wma_derive( version, rates[r], channels, bit_rates[b], flag_words[f], layouts[l].extradata, &mine );
        rr = wma_reference_derive( version, rates[r], channels, bit_rates[b], flag_words[f], layouts[l].extradata, &ref );
        if ((rm == 0) != (rr == 0))
        {
            fprintf( stderr, "v%d %d Hz %d ch %lld bps flags %#x extradata %zu: mine %d, FFmpeg %d\n", version,
                     rates[r], channels, (long long)bit_rates[b], flag_words[f], layouts[l].extradata, rm, rr );
            CHECK( (rm == 0) == (rr == 0) );
            continue;
        }
        if (rm) continue;
        compared++;
        if (memcmp( &mine, &ref, sizeof(mine) ))
        {
            fprintf( stderr, "v%d %d Hz %d ch %lld bps flags %#x: mine flb %d bob %d nbs %d noise %d vlc %d,"
                     " FFmpeg flb %d bob %d nbs %d noise %d vlc %d\n", version, rates[r], channels,
                     (long long)bit_rates[b], flag_words[f], mine.frame_len_bits, mine.byte_offset_bits,
                     mine.nb_block_sizes, mine.use_noise_coding, mine.coef_vlc_table, ref.frame_len_bits,
                     ref.byte_offset_bits, ref.nb_block_sizes, ref.use_noise_coding, ref.coef_vlc_table );
        }
        CHECK_INT( mine.frame_len_bits, ref.frame_len_bits );
        CHECK_INT( mine.byte_offset_bits, ref.byte_offset_bits );
        CHECK_INT( mine.nb_block_sizes, ref.nb_block_sizes );
        CHECK_INT( mine.use_noise_coding, ref.use_noise_coding );
        CHECK_INT( mine.coef_vlc_table, ref.coef_vlc_table );
    }
    printf( "compared %d parameter sets with FFmpeg's decoder\n", compared );
    CHECK( compared > 1000 );
}

static void test_simple_rules( void )
{
    static const uint8_t xwma[6] = { 0, 0, 0, 0, 0x1f, 0 };
    uint8_t bad[6];

    CHECK_INT( wma_frame_len_bits( 2, 16000 ), 9 );
    CHECK_INT( wma_frame_len_bits( 2, 22050 ), 10 );
    CHECK_INT( wma_frame_len_bits( 1, 32000 ), 10 );
    CHECK_INT( wma_frame_len_bits( 2, 32000 ), 11 );
    CHECK_INT( wma_frame_len_bits( 2, 44100 ), 11 );

    CHECK_INT( wma_private_flags( 1, (const uint8_t *)"\0\0\x05\0", 4 ), 5 );
    CHECK_INT( wma_private_flags( 2, xwma, 6 ), 0x1f );
    CHECK_INT( wma_private_flags( 2, xwma, 5 ), 0 );

    /* rule 2 */
    CHECK_INT( wma_is_xwma_private( xwma, 6 ), 1 );
    CHECK_INT( wma_is_xwma_private( xwma, 5 ), 0 );
    memcpy( bad, xwma, 6 );
    bad[4] = 0x1d;    /* no bit reservoir */
    CHECK_INT( wma_is_xwma_private( bad, 6 ), 0 );
    memcpy( bad, xwma, 6 );
    bad[1] = 1;       /* not zero elsewhere */
    CHECK_INT( wma_is_xwma_private( bad, 6 ), 0 );
    CHECK_INT( wma_xwma_normalised_rate( 1, 22050, 48000 ), 20000 );
    CHECK_INT( wma_xwma_normalised_rate( 1, 22050, 192000 ), 20000 );
    CHECK_INT( wma_xwma_normalised_rate( 1, 32000, 48000 ), 20000 );
    CHECK_INT( wma_xwma_normalised_rate( 1, 44100, 96000 ), 48000 );
    CHECK_INT( wma_xwma_normalised_rate( 2, 22050, 48000 ), 32000 );
    CHECK_INT( wma_xwma_normalised_rate( 2, 32000, 192000 ), 48000 );
    CHECK_INT( wma_xwma_normalised_rate( 2, 32000, 48000 ), 0 );
    CHECK_INT( wma_xwma_normalised_rate( 2, 44100, 96000 ), 0 );
    CHECK_INT( wma_xwma_normalised_rate( 1, 22050, 20000 ), 0 );

    /* rule 9 */
    CHECK_INT( wma_implied_rate( 2230, 44100, 16, 16 * 4, 11 ), 2230LL * 8 * 44100 * 16 / ((16 * 4) << 11) );
    CHECK_INT( wma_implied_rate( 2230, 44100, 0, 0, 11 ), 0 );
    CHECK_INT( wma_nearest_declared_rate( 50000 ), 48000 );
    CHECK_INT( wma_nearest_declared_rate( 1000 ), 20000 );
    CHECK_INT( wma_nearest_declared_rate( 1000000 ), 192000 );
    CHECK_INT( wma_nearest_declared_rate( 130000 ), 160000 );   /* 130000 - 96000 > 160000 - 130000 */

    /* rule 8, superframes (frame_len_bits 11) */
    CHECK_INT( wma_packet_good( 1, 3, 3 * 2048, 11, 0, 0, 0 ), 1 );
    CHECK_INT( wma_packet_good( 1, 3, 2 * 2048, 11, 0, 0, 0 ), 0 );
    CHECK_INT( wma_packet_good( 1, 3, 2 * 2048, 11, 1, 0, 0 ), 1 );
    CHECK_INT( wma_packet_good( 1, 3, 1 * 2048, 11, 1, 0, 0 ), 0 );
    CHECK_INT( wma_packet_good( 1, 3, 0, 11, 3, 0, 0 ), 1 );         /* after an open */
    CHECK_INT( wma_packet_good( 1, 3, 4 * 2048, 11, 3, 0, 0 ), 0 );  /* more than declared */
    CHECK_INT( wma_packet_good( 1, 0, 0, 11, 0, 0, 0 ), 1 );
    CHECK_INT( wma_packet_good( 1, 0, 2048, 11, 3, 0, 0 ), 0 );      /* a declared 0 is good only when empty */
    CHECK_INT( wma_packet_good( 1, 3, 3 * 2048, 11, 0, 1, 0 ), 0 );  /* send failed */
    CHECK_INT( wma_packet_good( 1, 3, 3 * 2048, 11, 0, 0, 1 ), 0 );  /* rejected */
    CHECK_INT( wma_packet_good( 0, 15, 0, 11, 0, 0, 0 ), 1 );        /* not a superframe stream */
    CHECK_INT( wma_packet_good( 0, 0, 2048, 11, 0, 0, 1 ), 0 );

    /* refusals */
    {
        struct wma_derived d;
        CHECK_INT( wma_derive( 2, 44100, 3, 128000, 0, 6, &d ), -1 );
        CHECK_INT( wma_derive( 2, 50001, 2, 128000, 0, 6, &d ), -1 );
        CHECK_INT( wma_derive( 2, 44100, 2, 0, 0, 6, &d ), -1 );
        CHECK_INT( wma_derive( 2, 8000, 1, 1000000000, 0, 6, &d ), -2 );   /* byte_offset_bits + 3 > 25 */
    }
}

static void test_candidates( void )
{
    struct wma_candidate list[WMA_MAX_CANDIDATES + 8];
    struct wma_search_inputs in;
    int n, i, j, first_header = -1, first_alt = -1;

    memset( &in, 0, sizeof(in) );
    in.version = 2;
    in.rate = 44100;
    in.channels = 2;
    in.extradata_size = 6;
    in.first_rate = 48000;
    in.alt_rate = 96000;
    in.implied_rate = 61000;
    in.header_rate = 128000;
    in.flags = 0x001f;

    n = wma_build_candidates( &in, list, WMA_MAX_CANDIDATES + 8 );
    CHECK( n > 6 );
    CHECK( n <= WMA_MAX_CANDIDATES );
    CHECK_INT( list[0].bit_rate, 48000 );    /* the rate it was opened at, its flags first */
    CHECK_INT( list[0].flags, 0x001f );
    for (i = 0; i < n; i++)
    {
        CHECK( list[i].d.byte_offset_bits + 3 <= 25 );
        for (j = 0; j < i; j++) CHECK( !wma_candidates_equivalent( &list[i], &list[j] ) );
        if (list[i].bit_rate == in.alt_rate && first_alt < 0) first_alt = i;
        if (list[i].bit_rate == in.header_rate && first_header < 0) first_header = i;
    }
    CHECK( first_alt > 0 );
    CHECK( first_header < 0 || first_header > first_alt );
    /* the flag variants of one rate: bits 3-4 forced to 3, 2, 1, 0, then without bit 2 */
    CHECK( list[1].bit_rate == 48000 || list[1].bit_rate == 96000 );

    /* the cap */
    n = wma_build_candidates( &in, list, 10 );
    CHECK_INT( n, 10 );

    /* dedupe: without variable block lengths every flag variant derives the same values */
    in.flags = 0x0002;
    in.rate = 22050;
    in.channels = 1;
    in.first_rate = 20000;
    in.alt_rate = 0;
    in.implied_rate = 0;
    in.header_rate = 20000;
    n = wma_build_candidates( &in, list, WMA_MAX_CANDIDATES );
    for (i = 0; i < n; i++)
    {
        CHECK_INT( list[i].d.nb_block_sizes, 1 );
        for (j = 0; j < i; j++) CHECK( !wma_candidates_equivalent( &list[i], &list[j] ) );
    }
    CHECK_INT( list[0].bit_rate, 20000 );
    CHECK( n < 7 + 6 + 2 );   /* far fewer than rates x variants */

    /* the byte_offset_bits middles: one candidate per value 7..12 reachable at 8 kHz mono */
    in.rate = 8000;
    in.first_rate = in.header_rate = 192000;
    n = wma_build_candidates( &in, list, WMA_MAX_CANDIDATES );
    {
        int seen[32] = { 0 };
        for (i = 0; i < n; i++) seen[list[i].d.byte_offset_bits] = 1;
        for (i = 7; i <= 12; i++) CHECK( seen[i] );
    }
}

int main( void )
{
    test_simple_rules();
    test_candidates();
    test_against_ffmpeg();
    return test_report( "test_wma_params" );
}
