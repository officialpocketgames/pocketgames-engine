// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14 Part B2 rule 13: libavcodec/libavformat's own diagnostics (warning and worse) go to
// the Wine log, tagged "[wma] av:" or "[wg-parser] av:" by owner, at most 64 lines per app.
// The owner is the component that is calling into FFmpeg on this thread.

#ifndef WG_AVLOG_H
#define WG_AVLOG_H

enum wg_av_owner
{
    WG_AV_OWNER_NONE = 0,
    WG_AV_OWNER_WMA,
    WG_AV_OWNER_PARSER,
};

/* Installs the av_log callback once per app (idempotent, thread safe). */
void wg_avlog_install( void );
/* Sets this thread's owner and returns the previous one (restore it with the same call). */
int wg_avlog_owner( int owner );

#endif /* WG_AVLOG_H */
