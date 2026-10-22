// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B2: the pure WMA rules (see wma_params.h). The arithmetic of wma_derive()
// reproduces the values FFmpeg 7.1.1's WMA v1/v2 decoder derives at open; it is written from
// that behaviour (libavcodec/wma.c ff_wma_init, wma_common.c, wmadec.c), with the same C types
// in the same places so that float rounding gives the same answers.

#include <string.h>

#include "wma_params.h"

int wma_frame_len_bits( int version, int rate )
{
    if (rate <= 16000) return 9;
    if (rate <= 22050 || (rate <= 32000 && version == 1)) return 10;
    return 11;
}

unsigned int wma_private_flags( int version, const uint8_t *priv, size_t size )
{
    if (version == 1 && size >= 4) return priv[2] | (priv[3] << 8);
    if (version == 2 && size >= 6) return priv[4] | (priv[5] << 8);
    return 0;
}

/* floor(log2(v)), 0 for v <= 1 (as libavutil's av_log2) */
static int log2_floor( unsigned int v )
{
    int n = 0;

    while (v > 1)
    {
        v >>= 1;
        n++;
    }
    return n;
}

int wma_derive( int version, int rate, int channels, int64_t bit_rate, unsigned int flags,
                size_t extradata_size, struct wma_derived *out )
{
    int variable_block_len, frame_len, rate1;
    float bps, bps1;

    memset( out, 0, sizeof(*out) );
    if (rate <= 0 || rate > 50000 || channels <= 0 || channels > 2 || bit_rate <= 0) return -1;
    if (version != 1) version = 2;

    out->frame_len_bits = wma_frame_len_bits( version, rate );
    frame_len = 1 << out->frame_len_bits;

    /* the variable block length flag, which a v2 decoder ignores for flags word 0x000d when it
     * has at least 8 bytes of private data */
    variable_block_len = !!(flags & 0x0004);
    if (version == 2 && extradata_size >= 8 && (flags & 0xffff) == 0x000d) variable_block_len = 0;

    if (variable_block_len)
    {
        int nb = (int)((flags >> 3) & 3) + 1;
        int nb_max = out->frame_len_bits - 7;   /* the smallest block is 2^7 */

        if (bit_rate / channels >= 32000) nb += 2;
        if (nb > nb_max) nb = nb_max;
        out->nb_block_sizes = nb + 1;
    }
    else out->nb_block_sizes = 1;

    /* v2 normalises the rate to the nearest lower standard one for these choices */
    rate1 = rate;
    if (version == 2)
    {
        if (rate1 >= 44100) rate1 = 44100;
        else if (rate1 >= 22050) rate1 = 22050;
        else if (rate1 >= 16000) rate1 = 16000;
        else if (rate1 >= 11025) rate1 = 11025;
        else if (rate1 >= 8000) rate1 = 8000;
    }

    bps = (float)bit_rate / (float)(channels * rate);
    out->byte_offset_bits = log2_floor( (unsigned int)(int)(bps * frame_len / 8.0 + 0.5) ) + 2;
    if (out->byte_offset_bits + 3 > WMA_MAX_OFFSET_BITS_PLUS_3) return -2;

    out->use_noise_coding = 1;
    bps1 = bps;
    if (channels == 2) bps1 = bps * 1.6;
    if (rate1 == 44100)
    {
        if (bps1 >= 0.61) out->use_noise_coding = 0;
    }
    else if (rate1 == 22050)
    {
        if (bps1 >= 1.16) out->use_noise_coding = 0;
    }
    else if (rate1 == 8000)
    {
        if (bps > 0.75) out->use_noise_coding = 0;
    }

    out->coef_vlc_table = 2;
    if (rate >= 32000)
    {
        if (bps1 < 0.72) out->coef_vlc_table = 0;
        else if (bps1 < 1.16) out->coef_vlc_table = 1;
    }
    return 0;
}

int wma_is_xwma_private( const uint8_t *priv, size_t size )
{
    size_t i;

    if (!priv || size < 6) return 0;
    if (!(priv[4] & 0x02)) return 0;
    for (i = 0; i < size; i++)
        if (i != 4 && i != 5 && priv[i]) return 0;
    return 1;
}

int64_t wma_xwma_normalised_rate( int channels, int rate, int64_t br )
{
    if (channels == 1)
    {
        if (rate == 22050 && (br == 48000 || br == 192000)) return 20000;
        if (rate == 32000 && (br == 48000 || br == 192000)) return 20000;
        if (rate == 44100 && (br == 96000 || br == 192000)) return 48000;
    }
    else if (channels == 2)
    {
        if (rate == 22050 && (br == 48000 || br == 192000)) return 32000;
        if (rate == 32000 && br == 192000) return 48000;
    }
    return 0;
}

int64_t wma_implied_rate( uint32_t block_align, int rate, uint32_t packets, uint32_t frames,
                          int frame_len_bits )
{
    uint64_t num, den;

    if (!block_align || rate <= 0 || !packets || !frames) return 0;
    num = (uint64_t)block_align * 8 * (uint64_t)rate * packets;
    den = (uint64_t)frames << frame_len_bits;
    return den ? (int64_t)(num / den) : 0;
}

static const int64_t declared_rates[7] = { 20000, 32000, 48000, 64000, 96000, 160000, 192000 };

int64_t wma_nearest_declared_rate( int64_t rate )
{
    int64_t best = declared_rates[0], best_diff = -1;
    unsigned int i;

    for (i = 0; i < 7; i++)
    {
        int64_t diff = declared_rates[i] > rate ? declared_rates[i] - rate : rate - declared_rates[i];
        if (best_diff < 0 || diff < best_diff)
        {
            best = declared_rates[i];
            best_diff = diff;
        }
    }
    return best;
}

int wma_candidates_equivalent( const struct wma_candidate *a, const struct wma_candidate *b )
{
    return a->d.byte_offset_bits == b->d.byte_offset_bits &&
           a->d.nb_block_sizes == b->d.nb_block_sizes &&
           a->d.use_noise_coding == b->d.use_noise_coding &&
           a->d.coef_vlc_table == b->d.coef_vlc_table &&
           (a->flags & 7) == (b->flags & 7);
}

/* a rate in the middle of the range that gives byte_offset_bits == bits */
static int64_t middle_rate( int bits, int rate, int channels, int frame_len_bits )
{
    /* byte_offset_bits = floor(log2(x)) + 2 with x = round(bit_rate * frame_len / (8 * channels * rate)) */
    int64_t lo = (int64_t)1 << (bits - 2), hi = ((int64_t)1 << (bits - 1)) - 1;
    int64_t x = (lo + hi) / 2;

    return x * 8 * channels * (int64_t)rate / ((int64_t)1 << frame_len_bits);
}

int wma_build_candidates( const struct wma_search_inputs *in, struct wma_candidate *out, int max )
{
    int64_t rates[4 + 7 + 6];
    unsigned int nrates = 0, r, v, i;
    int count = 0, frame_len_bits;

    if (max > WMA_MAX_CANDIDATES) max = WMA_MAX_CANDIDATES;
    if (max <= 0) return 0;
    frame_len_bits = wma_frame_len_bits( in->version, in->rate );

    rates[nrates++] = in->first_rate;
    if (in->alt_rate > 0) rates[nrates++] = in->alt_rate;
    if (in->implied_rate > 0) rates[nrates++] = wma_nearest_declared_rate( in->implied_rate );
    rates[nrates++] = in->header_rate;
    for (i = 0; i < 7; i++) rates[nrates++] = declared_rates[i];
    for (i = 7; i <= 12; i++) rates[nrates++] = middle_rate( i, in->rate, in->channels, frame_len_bits );

    for (r = 0; r < nrates && count < max; r++)
    {
        unsigned int variants[6];

        if (rates[r] <= 0) continue;
        variants[0] = in->flags & 0xffff;
        for (v = 1; v <= 4; v++) variants[v] = (in->flags & 0xffe7) | ((4 - v) << 3);   /* 3, 2, 1, 0 */
        variants[5] = in->flags & 0xfffb;

        for (v = 0; v < 6 && count < max; v++)
        {
            struct wma_candidate c;
            int k, dup = 0;

            c.bit_rate = rates[r];
            c.flags = variants[v];
            if (wma_derive( in->version, in->rate, in->channels, c.bit_rate, c.flags,
                            in->extradata_size, &c.d )) continue;
            for (k = 0; k < count && !dup; k++) dup = wma_candidates_equivalent( &out[k], &c );
            if (dup) continue;
            out[count++] = c;
        }
    }
    return count;
}

int wma_packet_good( int superframe, int declared, int64_t samples, int frame_len_bits, int slack,
                     int send_failed, int rejected )
{
    int64_t produced;

    if (send_failed || rejected) return 0;
    if (!superframe) return 1;
    produced = samples >> frame_len_bits;
    if (!declared) return samples == 0;
    return produced <= declared && produced >= declared - slack;
}
