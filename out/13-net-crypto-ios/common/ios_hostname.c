// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 17: the prefix's own host name. See include/ios_hostname.h for the
// rules.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ios_hostname.h"

static const char name_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

int ios_hostname_valid( const char *name, size_t len )
{
    size_t i;

    if (!name || !len || len > IOS_HOSTNAME_LEN) return 0;
    if (name[0] == '-' || name[len - 1] == '-') return 0;
    for (i = 0; i < len; i++)
    {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
            return 0;
    }
    return 1;
}

void ios_hostname_make( char *out, ios_hostname_random_fn rnd )
{
    const uint32_t count = sizeof(name_chars) - 1;
    const uint32_t limit = UINT32_MAX - UINT32_MAX % count;   /* draws at or above it would bias */
    size_t i, pos = sizeof(IOS_HOSTNAME_PREFIX) - 1;

    memcpy( out, IOS_HOSTNAME_PREFIX, pos );
    for (i = pos; i < IOS_HOSTNAME_LEN; i++)
    {
        uint32_t r;

        do r = rnd(); while (r >= limit);
        out[i] = name_chars[r % count];
    }
    out[IOS_HOSTNAME_LEN] = 0;
}

int ios_hostname_prefix_dir( const char *wineprefix, const char *home, char *buf, size_t size )
{
    size_t len;
    int n;

    if (wineprefix && *wineprefix)
    {
        if (wineprefix[0] != '/') return -1;
        len = strlen( wineprefix );
        while (len > 1 && wineprefix[len - 1] == '/') len--;
        if (len >= size) return -1;
        memcpy( buf, wineprefix, len );
        buf[len] = 0;
        return 0;
    }
    if (!home || home[0] != '/') return -1;
    n = snprintf( buf, size, "%s/.wine", home );
    return (n < 0 || (size_t)n >= size) ? -1 : 0;
}

/* the file's first line into out if it is a valid name: 1; a file without one: 0; no file: -1;
 * a file that can't be read: -2 */
static int read_name( const char *path, char *out )
{
    char buf[64];
    ssize_t got;
    size_t len;
    int fd = open( path, O_RDONLY | O_CLOEXEC );

    if (fd == -1) return errno == ENOENT ? -1 : -2;
    got = read( fd, buf, sizeof(buf) - 1 );
    close( fd );
    if (got < 0) return -2;
    buf[got] = 0;
    len = strcspn( buf, "\r\n" );
    if (!ios_hostname_valid( buf, len )) return 0;
    memcpy( out, buf, len );
    out[len] = 0;
    return 1;
}

/* writes name and a newline into fd and closes it; 0 on success */
static int write_name( int fd, const char *name )
{
    char line[IOS_HOSTNAME_LEN + 2];
    size_t len = strlen( name );
    ssize_t done;

    memcpy( line, name, len );
    line[len++] = '\n';
    done = write( fd, line, len );
    if (close( fd ) || done != (ssize_t)len) return -1;
    return 0;
}

int ios_hostname_load( const char *dir, char *out, ios_hostname_random_fn rnd )
{
    char path[4096], name[IOS_HOSTNAME_LEN + 1];
    int n, ret, fd;

    if (!dir || (n = snprintf( path, sizeof(path), "%s/%s", dir, IOS_HOSTNAME_FILE )) < 0 ||
        (size_t)n >= sizeof(path))
    {
        ios_hostname_make( out, rnd );
        return IOS_HOSTNAME_NOT_STORED;
    }

    if ((ret = read_name( path, out )) == 1) return IOS_HOSTNAME_READ;
    if (ret == -2)
    {
        ios_hostname_make( out, rnd );
        return IOS_HOSTNAME_NOT_STORED;
    }

    ios_hostname_make( name, rnd );
    if (ret == -1)   /* no file: create it, unless another creator wins */
    {
        if ((fd = open( path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600 )) != -1)
        {
            if (write_name( fd, name )) goto not_stored;
            strcpy( out, name );
            return IOS_HOSTNAME_CREATED;
        }
        if (errno == EEXIST && read_name( path, out ) == 1) return IOS_HOSTNAME_READ;
        goto not_stored;
    }

    /* a file without a valid name: replace it */
    if ((fd = open( path, O_WRONLY | O_TRUNC | O_CLOEXEC )) == -1 || write_name( fd, name ))
        goto not_stored;
    strcpy( out, name );
    return IOS_HOSTNAME_REPLACED;

not_stored:
    strcpy( out, name );
    return IOS_HOSTNAME_NOT_STORED;
}
