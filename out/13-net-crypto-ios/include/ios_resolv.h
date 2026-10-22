// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part H2 (as changed by clarification 3, spec 00 rule D2): dnsapi's resolver
// (common/ios_resolv.c). The app links libresolv (-lresolv) and calls the public res_n* family of
// <resolv.h> on a resolver state of its own per thread; no library is opened by path.
//
// dnsapi/libresolv.c includes this header after the system headers; from then on upstream's three
// uses of the host resolver and h_errno go through functions of our own:
//   _res          -> *ios_resolv_state(): the calling thread's resolver state, allocated zeroed on
//                    first use and released with res_ndestroy() when the thread exits
//   res_init()    -> ios_resolv_init(): res_ninit(state)
//   res_query()   -> ios_resolv_query(): res_nquery(state, ...); on failure the state's
//                    res_h_errno is copied to our thread-local h_errno
//   h_errno       -> that thread-local value (never the process-wide _res / h_errno symbols)

#ifndef MYIOSDECK_IOS_RESOLV_H
#define MYIOSDECK_IOS_RESOLV_H

#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/nameser.h>
#include <resolv.h>
#include <netdb.h>

extern res_state ios_resolv_state( void );
extern int ios_resolv_init( void );
extern int ios_resolv_query( const char *dname, int klass, int type, unsigned char *answer, int anslen );
extern int *ios_resolv_h_errno_location( void );

#ifdef IOS_RESOLV_TEST
/* host tests: common/ios_resolv.c calls these fakes instead of res_ninit / res_nquery /
 * res_ndestroy */
extern int ios_test_res_ninit( res_state state );
extern int ios_test_res_nquery( res_state state, const char *dname, int klass, int type,
                                unsigned char *answer, int anslen );
extern void ios_test_res_ndestroy( res_state state );
#endif

#ifndef IOS_RESOLV_NO_MACROS
# undef _res
# undef res_init
# undef res_query
# undef h_errno
# define _res        (*ios_resolv_state())
# define res_init()  ios_resolv_init()
# define res_query   ios_resolv_query
# define h_errno     (*ios_resolv_h_errno_location())
#endif

#endif /* MYIOSDECK_IOS_RESOLV_H */
