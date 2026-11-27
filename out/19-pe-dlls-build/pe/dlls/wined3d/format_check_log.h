/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */

/*
 * Spec 19 A23: once per format per process, wined3d's framebuffer format check (check_fbo_compat
 * in utils.c) logs a format it marks not FBO attachable, with the GL internal format and the
 * framebuffer status or GL error; the pure part shared by utils.c and the host tests.
 */

#ifndef MYIOSDECK_FORMAT_CHECK_LOG_H
#define MYIOSDECK_FORMAT_CHECK_LOG_H

#define FORMAT_CHECK_LOG_MAX 128    /* formats remembered; past that, nothing more is logged */
/* format name, GL internal format, resource type, framebuffer status, GL error (names) */
#define FORMAT_CHECK_LOG_FORMAT \
    "[wined3d] format %s (GL internal format %#x) is not FBO attachable, resource type %u: %s, GL error %s\n"

struct format_check_log
{
    unsigned int ids[FORMAT_CHECK_LOG_MAX];
    unsigned int count;
};

/* whether format id is logged now: the first time it comes, and only while there is room */
static inline int format_check_log_first(struct format_check_log *log, unsigned int id)
{
    unsigned int i;

    for (i = 0; i < log->count; ++i)
    {
        if (log->ids[i] == id)
            return 0;
    }
    if (log->count == FORMAT_CHECK_LOG_MAX)
        return 0;
    log->ids[log->count++] = id;
    return 1;
}

#endif /* MYIOSDECK_FORMAT_CHECK_LOG_H */
