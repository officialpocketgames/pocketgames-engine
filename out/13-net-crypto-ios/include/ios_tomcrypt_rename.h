// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 7.4: name clashes of LibTomCrypt / LibTomMath with the rest of the app.
// build-crypto-deps-ios.sh force-includes this header when it compiles libtomcrypt_unix.a, and
// every file of ours that calls LibTomCrypt includes it before <tomcrypt.h>, so a rename here
// applies on both sides. Wine's PE build of LibTomCrypt (bcrypt's hashing) is a separate PE
// library and never meets these objects.
//
// Empty while CI's check-symbols.sh (A4) reports no clash between libtomcrypt_unix.a and the
// other archives of the app; a symbol it reports gets a line here, for example
//   #define sha1_init ios_ltc_sha1_init

#ifndef MYIOSDECK_IOS_TOMCRYPT_RENAME_H
#define MYIOSDECK_IOS_TOMCRYPT_RENAME_H

#endif /* MYIOSDECK_IOS_TOMCRYPT_RENAME_H */
