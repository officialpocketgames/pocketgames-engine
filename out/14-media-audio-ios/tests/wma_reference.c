// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part F: the reference for wma_derive(). Opens FFmpeg's own WMA v1/v2 decoder with the
// given parameters and reads the values its init derived (byte_offset_bits, nb_block_sizes,
// use_noise_coding, the coefficient VLC table, frame_len_bits) off its private context. Built
// against the host FFmpeg's source tree (internal header libavcodec/wma.h) and linked with the
// libavcodec.a of that same tree, so the struct layout matches. Test-only code.

#include "config.h"   /* the host FFmpeg build's */

#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavcodec/wma.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"

#include "wma_params.h"

/* the sizes of the code tables coef_vlcs[0], [2], [4] in libavcodec/wmadata.h (coef0_huffbits,
 * coef2_huffbits, coef4_huffbits): the first table of each pair identifies tables 0, 1, 2 */
static int table_index( const CoefVLCTable *t )
{
    switch (t ? t->n : -1)
    {
    case 666: return 0;
    case 1336: return 1;
    case 476: return 2;
    }
    return -1;
}

/* 0 and *out filled, or -1 when FFmpeg refuses to open the decoder */
int wma_reference_derive( int version, int rate, int channels, int64_t bit_rate, unsigned int flags,
                          size_t extradata_size, struct wma_derived *out )
{
    const AVCodec *codec = avcodec_find_decoder( version == 1 ? AV_CODEC_ID_WMAV1 : AV_CODEC_ID_WMAV2 );
    AVCodecContext *ctx;
    WMACodecContext *s;
    uint8_t *extra = NULL;
    int ret;

    memset( out, 0, sizeof(*out) );
    if (!codec || !(ctx = avcodec_alloc_context3( codec ))) return -2;
    ctx->sample_rate = rate;
    ctx->bit_rate = bit_rate;
    ctx->block_align = 2048;
    av_channel_layout_default( &ctx->ch_layout, channels );
    if (extradata_size)
    {
        extra = av_mallocz( extradata_size + AV_INPUT_BUFFER_PADDING_SIZE );
        if (version == 1 && extradata_size >= 4)
        {
            extra[2] = flags & 0xff;
            extra[3] = (flags >> 8) & 0xff;
        }
        if (version == 2 && extradata_size >= 6)
        {
            extra[4] = flags & 0xff;
            extra[5] = (flags >> 8) & 0xff;
        }
        ctx->extradata = extra;
        ctx->extradata_size = (int)extradata_size;
    }
    av_log_set_level( AV_LOG_QUIET );
    ret = avcodec_open2( ctx, codec, NULL );
    if (ret < 0)
    {
        avcodec_free_context( &ctx );
        return -1;
    }
    s = ctx->priv_data;
    out->frame_len_bits = s->frame_len_bits;
    out->byte_offset_bits = s->byte_offset_bits;
    out->nb_block_sizes = s->nb_block_sizes;
    out->use_noise_coding = s->use_noise_coding;
    out->coef_vlc_table = table_index( s->coef_vlcs[0] );
    avcodec_free_context( &ctx );
    return 0;
}
