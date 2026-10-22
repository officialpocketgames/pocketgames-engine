// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 14: log helpers shared by the winegstreamer unix side, the audio driver and the platform
// decoders (see media_log.h).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_log.h"

/* ntdll's unix side (wine/debug.h declares it; this file includes no Wine header) */
extern int __wine_dbg_output( const char *str );

void iom_logv( const char *prefix, const char *fmt, va_list args )
{
    char buffer[1024];
    int len = snprintf( buffer, sizeof(buffer), "%s", prefix ? prefix : "" );

    if (len < 0) len = 0;
    if (len > (int)sizeof(buffer) - 2) len = sizeof(buffer) - 2;
    len += vsnprintf( buffer + len, sizeof(buffer) - len - 1, fmt, args );
    if (len > (int)sizeof(buffer) - 2) len = sizeof(buffer) - 2;
    if (len < 0) len = 0;
    if (!len || buffer[len - 1] != '\n') buffer[len++] = '\n';
    buffer[len] = 0;
    __wine_dbg_output( buffer );
}

void iom_log( const char *prefix, const char *fmt, ... )
{
    va_list args;

    va_start( args, fmt );
    iom_logv( prefix, fmt, args );
    va_end( args );
}

int iom_env_is( const char *name, const char *value )
{
    const char *v = getenv( name );
    return v && !strcmp( v, value );
}

int iom_env_set( const char *name )
{
    return getenv( name ) != NULL;
}

int iom_diag( void )
{
    static volatile int diag = -1;
    int v = diag;

    if (v < 0)
    {
        v = iom_env_is( "MADEIRA_DIAG", "1" );
        diag = v;
    }
    return v;
}

int iom_cap_take( struct iom_cap *cap, int max, const char *prefix )
{
    int32_t n = __atomic_add_fetch( &cap->count, 1, __ATOMIC_RELAXED );

    if (n <= max) return 1;
    if (n == max + 1) iom_log( prefix, "... further lines suppressed (limit %d per app)", max );
    if (n > max + 1000000) __atomic_store_n( &cap->count, max + 2, __ATOMIC_RELAXED ); /* no wrap */
    return 0;
}

int iom_dedupe_check( struct iom_dedupe *table, const char *line, uint32_t *count )
{
    unsigned int i;
    uint32_t n;

    *count = 0;
    for (i = 0; i < table->used; i++)
    {
        if (strcmp( table->lines[i], line )) continue;
        if (table->counts[i] < UINT32_MAX) table->counts[i]++;
        n = table->counts[i];
        *count = n;
        return !(n & (n - 1));   /* 2, 4, 8, ... (1 was printed when first seen) */
    }
    if (table->used >= IOM_DEDUPE_MAX) return 0;
    if (!(table->lines[table->used] = strdup( line ))) return 0;
    table->counts[table->used++] = 1;
    *count = 1;
    return 1;
}
