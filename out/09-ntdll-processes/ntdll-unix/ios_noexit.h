// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part A, "No exits". build-ios.sh passes "-include ios_noexit.h" so this header comes
// first in every file of ntdll's unix side. The whole app is one process, so exit(), _exit()
// and abort() must never reach libc from here: they end only the calling pseudo-process
// (Part D), or only the calling thread when it has no Windows state (Part E3).
//
// The libc headers are included first, so their own declarations of exit/_exit/abort are
// already seen when the macros are defined and are not renamed.

#ifndef MYIOSDECK_IOS_NOEXIT_H
#define MYIOSDECK_IOS_NOEXIT_H

#ifdef WINE_IOS

#include <stdlib.h>
#include <unistd.h>

/* Ends the calling pseudo-process with this exit status (ios_process.c). Never returns. */
extern void ios_unix_exit( int status, const char *why ) __attribute__((noreturn));

/* exit code used for abort(): the value Windows' abort() reports */
#define IOS_ABORT_EXIT_CODE 3

#define exit(status)   ios_unix_exit( (status), "exit" )
#define _exit(status)  ios_unix_exit( (status), "_exit" )
#define abort()        ios_unix_exit( IOS_ABORT_EXIT_CODE, "abort" )

#endif /* WINE_IOS */

#endif /* MYIOSDECK_IOS_NOEXIT_H */
