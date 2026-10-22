// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part G2: the system resolver calls inside an autorelease pool of their own.
//
// On iOS getaddrinfo, getnameinfo, gethostbyname and gethostbyaddr run through Apple's info
// daemon client, which autoreleases Objective-C objects. A Wine thread has no autorelease pool,
// so those objects would land in the thread's implicit pool, drained at thread exit, and that
// drain crashed. Each call here pushes a pool (libobjc's objc_autoreleasePoolPush, what
// @autoreleasepool compiles to) and pops it right after. The results are malloc'ed (or, for
// gethostby*, libinfo's own storage), never autoreleased, so popping doesn't touch them.
// errno and h_errno are those the call left: the callers read them afterwards, and the pop
// (which releases objects) must not change them.
//
// Elsewhere than Apple the wrappers are the plain calls. Include after <netdb.h>.

#ifndef MYIOSDECK_IOS_OBJC_POOL_H
#define MYIOSDECK_IOS_OBJC_POOL_H

#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#ifdef __APPLE__
extern void *objc_autoreleasePoolPush( void );
extern void objc_autoreleasePoolPop( void *pool );
#else
static inline void *objc_autoreleasePoolPush( void ) { return NULL; }
static inline void objc_autoreleasePoolPop( void *pool ) { }
#endif

static inline int ios_pool_getaddrinfo( const char *node, const char *service,
                                        const struct addrinfo *hints, struct addrinfo **res )
{
    void *pool = objc_autoreleasePoolPush();
    int ret = getaddrinfo( node, service, hints, res );
    int err = errno;
    objc_autoreleasePoolPop( pool );
    errno = err;
    return ret;
}

static inline int ios_pool_getnameinfo( const struct sockaddr *addr, socklen_t addrlen,
                                        char *host, socklen_t hostlen,
                                        char *serv, socklen_t servlen, int flags )
{
    void *pool = objc_autoreleasePoolPush();
    int ret = getnameinfo( addr, addrlen, host, hostlen, serv, servlen, flags );
    int err = errno;
    objc_autoreleasePoolPop( pool );
    errno = err;
    return ret;
}

static inline struct hostent *ios_pool_gethostbyname( const char *name )
{
    void *pool = objc_autoreleasePoolPush();
    struct hostent *ret = gethostbyname( name );
    int err = errno, herr = h_errno;
    objc_autoreleasePoolPop( pool );
    errno = err;
    h_errno = herr;
    return ret;
}

static inline struct hostent *ios_pool_gethostbyaddr( const void *addr, socklen_t len, int type )
{
    void *pool = objc_autoreleasePoolPush();
    struct hostent *ret = gethostbyaddr( addr, len, type );
    int err = errno, herr = h_errno;
    objc_autoreleasePoolPop( pool );
    errno = err;
    h_errno = herr;
    return ret;
}

#endif /* MYIOSDECK_IOS_OBJC_POOL_H */
