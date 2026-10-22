// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B2 rule 13: FFmpeg's log routed into the Wine log (see wg_avlog.h).

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/log.h>

#include "media_log.h"
#include "wg_avlog.h"

#define AV_LINES_PER_APP 64

static __thread int current_owner;
static struct iom_cap av_cap;
static pthread_once_t install_once = PTHREAD_ONCE_INIT;

static void av_callback( void *avcl, int level, const char *fmt, va_list args )
{
    const char *prefix, *item = NULL;
    char line[512];
    size_t len;

    if (level > AV_LOG_WARNING) return;
    prefix = current_owner == WG_AV_OWNER_WMA ? "[wma] av: " : "[wg-parser] av: ";

    vsnprintf( line, sizeof(line), fmt, args );
    len = strlen( line );
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
    if (!len) return;
    if (!iom_cap_take( &av_cap, AV_LINES_PER_APP, prefix )) return;

    if (avcl)
    {
        const AVClass *cls = *(const AVClass **)avcl;
        if (cls && cls->item_name) item = cls->item_name( avcl );
    }
    if (item) iom_log( prefix, "%s: %s", item, line );
    else iom_log( prefix, "%s", line );
}

static void install( void )
{
    av_log_set_callback( av_callback );
}

void wg_avlog_install( void )
{
    pthread_once( &install_once, install );
}

int wg_avlog_owner( int owner )
{
    int prev = current_owner;
    current_owner = owner;
    return prev;
}
