// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B4: parsing the decoder configuration records of ISO/IEC 14496-15 (avcC, hvcC)
// and building the ES_Descriptor of ISO/IEC 14496-1 for AAC (see wg_platform.h). Written from
// the published standards.

#include <string.h>

#include "wg_platform.h"

static unsigned int be16( const uint8_t *p )
{
    return (p[0] << 8) | p[1];
}

static int add_set( struct wgp_param_sets *out, const uint8_t *data, size_t size, const char **why )
{
    if (out->count >= WGP_MAX_PARAM_SETS)
    {
        *why = "more than 32 parameter sets";
        return -1;
    }
    if (!size)
    {
        *why = "an empty parameter set";
        return -1;
    }
    out->data[out->count] = data;
    out->size[out->count] = size;
    out->count++;
    return 0;
}

int wgp_parse_avcc( const uint8_t *data, size_t size, struct wgp_param_sets *out, const char **why )
{
    size_t pos;
    unsigned int n, i, list;

    memset( out, 0, sizeof(*out) );
    *why = NULL;
    if (!data || !size) { *why = "no avcC"; return -1; }
    if (size < 7) { *why = "avcC too short"; return -1; }
    if (data[0] != 1) { *why = "avcC version is not 1"; return -1; }
    out->profile = data[1];
    out->level = data[3];
    out->nal_length_size = (data[4] & 3) + 1;
    if (out->nal_length_size == 3) { *why = "avcC NAL length size 3"; return -1; }

    pos = 5;
    for (list = 0; list < 2; list++)   /* SPS, then PPS */
    {
        if (pos >= size) { *why = "avcC truncated"; return -1; }
        n = list ? data[pos] : data[pos] & 0x1f;
        pos++;
        for (i = 0; i < n; i++)
        {
            size_t len;

            if (pos + 2 > size) { *why = "avcC truncated"; return -1; }
            len = be16( data + pos );
            pos += 2;
            if (pos + len > size) { *why = "avcC parameter set past its end"; return -1; }
            if (add_set( out, data + pos, len, why )) return -1;
            pos += len;
        }
    }
    if (out->count < 2) { *why = "avcC has fewer than 2 parameter sets"; return -1; }
    return 0;
}

int wgp_parse_hvcc( const uint8_t *data, size_t size, struct wgp_param_sets *out, const char **why )
{
    size_t pos;
    unsigned int arrays, a, n, i, type;

    memset( out, 0, sizeof(*out) );
    *why = NULL;
    if (!data || !size) { *why = "no hvcC"; return -1; }
    if (size < 23) { *why = "hvcC too short"; return -1; }
    if (data[0] != 1) { *why = "hvcC version is not 1"; return -1; }
    out->nal_length_size = (data[21] & 3) + 1;
    if (out->nal_length_size == 3) { *why = "hvcC NAL length size 3"; return -1; }

    arrays = data[22];
    pos = 23;
    for (a = 0; a < arrays; a++)
    {
        if (pos + 3 > size) { *why = "hvcC truncated"; return -1; }
        type = data[pos] & 0x3f;
        n = be16( data + pos + 1 );
        pos += 3;
        for (i = 0; i < n; i++)
        {
            size_t len;

            if (pos + 2 > size) { *why = "hvcC truncated"; return -1; }
            len = be16( data + pos );
            pos += 2;
            if (pos + len > size) { *why = "hvcC NAL unit past its end"; return -1; }
            if (type >= 32 && type <= 34 && add_set( out, data + pos, len, why )) return -1;   /* VPS, SPS, PPS */
            pos += len;
        }
    }
    if (out->count < 3) { *why = "hvcC has fewer than 3 parameter sets"; return -1; }
    return 0;
}

size_t wgp_esds_cookie( const uint8_t *asc, size_t asc_size, uint8_t *out, size_t out_size )
{
    size_t dsi_len, dcd_len, es_len, total, pos = 0;

    if (!asc || asc_size < 2 || asc_size > 64) return 0;
    dsi_len = asc_size;                  /* DecoderSpecificInfo payload */
    dcd_len = 13 + 2 + dsi_len;          /* DecoderConfigDescriptor payload */
    es_len = 3 + 2 + dcd_len;            /* ES_Descriptor payload */
    total = 2 + es_len;                  /* every length fits in one byte (< 128) */
    if (out_size < total) return 0;

    out[pos++] = 0x03;                   /* ES_DescrTag */
    out[pos++] = (uint8_t)es_len;
    out[pos++] = 0x00;                   /* ES_ID */
    out[pos++] = 0x00;
    out[pos++] = 0x00;                   /* no dependsOn, URL or OCR stream; priority 0 */

    out[pos++] = 0x04;                   /* DecoderConfigDescrTag */
    out[pos++] = (uint8_t)dcd_len;
    out[pos++] = 0x40;                   /* objectTypeIndication: Audio ISO/IEC 14496-3 */
    out[pos++] = (0x05 << 2) | 0x01;     /* streamType AudioStream, upStream 0, reserved 1 */
    out[pos++] = 0x00;                   /* bufferSizeDB (24 bits) */
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    memset( out + pos, 0, 8 );           /* maxBitrate, avgBitrate */
    pos += 8;

    out[pos++] = 0x05;                   /* DecSpecificInfoTag */
    out[pos++] = (uint8_t)dsi_len;
    memcpy( out + pos, asc, asc_size );
    pos += asc_size;
    return pos;
}
