// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B2: the pure rules of the WMA decoder (no FFmpeg, no Wine), shared with the host
// tests.
//
// WMA v1/v2 decoders see the bit rate and the flags word only through four values derived at
// open (rule 7): byte_offset_bits, nb_block_sizes, use_noise_coding and the coefficient VLC
// table. wma_derive() is that arithmetic, written from the behaviour of FFmpeg 7.1.1's
// libavcodec/wma.c and wmadec.c (LGPL; see PROVENANCE.md), not copied from it.

#ifndef WMA_PARAMS_H
#define WMA_PARAMS_H

#include <stddef.h>
#include <stdint.h>

/* WAVE format tags of the family (rule 1) */
#define WMA_TAG_V1        0x0160
#define WMA_TAG_V2        0x0161
#define WMA_TAG_PRO       0x0162
#define WMA_TAG_LOSSLESS  0x0163
#define WMA_TAG_XMA1      0x0165
#define WMA_TAG_XMA2      0x0166

/* libavcodec refuses byte_offset_bits + 3 above this (MIN_CACHE_BITS of its bit reader) */
#define WMA_MAX_OFFSET_BITS_PLUS_3 25

struct wma_derived
{
    int frame_len_bits;
    int byte_offset_bits;
    int nb_block_sizes;
    int use_noise_coding;
    int coef_vlc_table;
};

/* frame_len_bits of a v1/v2 stream: 9 for rate <= 16000, 10 for <= 22050 or (v1) <= 32000,
 * else 11 */
int wma_frame_len_bits( int version, int rate );

/* The flags word of the private data: bytes 2-3 (v1, at least 4 bytes) or 4-5 (v2, at least
 * 6 bytes), little-endian; 0 otherwise. */
unsigned int wma_private_flags( int version, const uint8_t *priv, size_t size );

/* The four derived values (and frame_len_bits) for a version 1 or 2 stream opened with this
 * bit rate and flags word and `extradata_size` bytes of private data.
 * Returns 0, -1 when libavcodec would refuse the parameters (rate outside 1..50000, channels
 * outside 1..2, bit rate <= 0) or -2 when byte_offset_bits + 3 > 25. */
int wma_derive( int version, int rate, int channels, int64_t bit_rate, unsigned int flags,
                size_t extradata_size, struct wma_derived *out );

/* Rule 2: xWMA callers invent their private data: at least 6 bytes, flags bit 1 (bit
 * reservoir) set, zero everywhere but bytes 4-5. */
int wma_is_xwma_private( const uint8_t *priv, size_t size );
/* Rule 2: the bit rate the Microsoft xWMA encoder meant for a (channels, rate, header bit
 * rate) triple it writes a fake rate for (libavformat/xwma.c's rows), or 0 for any other. */
int64_t wma_xwma_normalised_rate( int channels, int rate, int64_t header_rate );

/* Rule 9: the bit rate a stream's superframes imply: block_align * 8 * rate * packets /
 * (frames << frame_len_bits), from the declared frame counts of `packets` superframe packets
 * summing to `frames`. 0 when unknown. */
int64_t wma_implied_rate( uint32_t block_align, int rate, uint32_t packets, uint32_t frames,
                          int frame_len_bits );
/* The declared rate (20000, 32000, 48000, 64000, 96000, 160000, 192000) nearest `rate`. */
int64_t wma_nearest_declared_rate( int64_t rate );

/* Rule 7: the candidate list */
#define WMA_MAX_CANDIDATES 48
struct wma_candidate
{
    int64_t bit_rate;
    unsigned int flags;
    struct wma_derived d;
};
struct wma_search_inputs
{
    int version;              /* 1 or 2 */
    int rate, channels;
    size_t extradata_size;    /* of the (patched) private data the decoder gets */
    int64_t first_rate;       /* the rate it was first opened at */
    int64_t alt_rate;         /* rule 2's alternative rate, 0 if none */
    int64_t implied_rate;     /* rule 9, 0 if not known yet */
    int64_t header_rate;      /* nAvgBytesPerSec * 8 */
    unsigned int flags;       /* the given flags word */
};
/* Fills `out` (up to `max`, at most WMA_MAX_CANDIDATES) and returns the count. Order: the
 * rates first_rate, alt_rate, the declared rate nearest implied_rate, header_rate, the seven
 * declared rates, then for byte_offset_bits 7..12 a rate in the middle of that value's range;
 * each rate with the flag variants: the given word, the same with bits 3-4 set to 3, 2, 1, 0,
 * the same without bit 2. A candidate whose derived values and flags bits 0-2 equal an earlier
 * one's, one libavcodec would refuse, and one whose byte_offset_bits + 3 > 25 are dropped. */
int wma_build_candidates( const struct wma_search_inputs *in, struct wma_candidate *out, int max );

/* Two parameter sets are equivalent for the decoder (same derived values, same flags bits 0-2) */
int wma_candidates_equivalent( const struct wma_candidate *a, const struct wma_candidate *b );

/* Rule 8: is a packet good?
 *   superframe: the stream is v1/v2 opened with flags bit 1 set
 *   declared:   the low nibble of the packet's first byte (superframe streams)
 *   samples:    samples per channel the packet produced
 *   slack:      3 after an open or a flush, 0 after a good packet, 1 after a bad one */
int wma_packet_good( int superframe, int declared, int64_t samples, int frame_len_bits, int slack,
                     int send_failed, int rejected );

#endif /* WMA_PARAMS_H */
