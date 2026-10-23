// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part A: adjustments to the host (macOS) configure's config.h for the iOS build of
// ntdll's unix side. build-ios.sh generates a config.h that includes the host one and then this
// file. Turn off what iOS lacks or what would act on the whole app instead of one pseudo-process.

#ifndef MYIOSDECK_IOS_CONFIG_H
#define MYIOSDECK_IOS_CONFIG_H

#ifdef WINE_IOS

/* no preloader and no re-exec: the app is the only process */
#undef HAVE_WINE_PRELOADER

/* renaming the process would rename the whole app (env.c set_process_name) */
#undef HAVE_SETPROCTITLE
#undef HAVE_SETPROGNAME
#undef HAVE_PRCTL

/* not on Darwin, listed so a cross-configured config.h can't turn them on */
#undef HAVE_LIBPROCSTAT_H
#undef HAVE_SYS_INOTIFY_H
/* spec 16 Part E: build-ios.sh defines HAVE_LINUX_NTSYNC_H=1 and IOS_USYNC=1 (the header and the
 * "driver" are out/16-sync-ios's) */
#ifndef IOS_USYNC
#undef HAVE_LINUX_NTSYNC_H
#endif
#undef HAVE_GETAUXVAL

/* headers the macOS SDK has and the iOS SDK doesn't (first iOS CI build): file.c's vnode list
 * workaround, the BSD process tables (sys/user.h), getrandom (Darwin has none; system.c's
 * random path falls back as upstream). cdrom.c's <sys/disk.h> sits in its __APPLE__ branches,
 * which the iOS build skips. */
#undef HAVE_SYS_VNODE_H
#undef HAVE_SYS_USER_H
#undef HAVE_SYS_RANDOM_H
#undef HAVE_GETRANDOM
#undef HAVE_SYS_DISK_H
#undef HAVE_SYS_CONF_H    /* file.c uses nothing from it on Darwin */

#endif /* WINE_IOS */

#endif /* MYIOSDECK_IOS_CONFIG_H */
