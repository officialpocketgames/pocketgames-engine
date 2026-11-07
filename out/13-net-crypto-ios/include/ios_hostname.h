// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 17: the machine's host name as Windows programs see it. iOS's host name
// can carry the device's name and changes when the user renames the phone; Windows programs
// get a name of the prefix's own instead, made once, stored in the prefix and the same at every
// start. ws2_32's gethostname answers with it, and wineboot's computer name keys (DNS host name
// and NetBIOS name, from that gethostname) follow. Kept free of Wine headers for the host tests.

#ifndef MYIOSDECK_IOS_HOSTNAME_H
#define MYIOSDECK_IOS_HOSTNAME_H

#include <stddef.h>
#include <stdint.h>

/* the file in the prefix's directory: the name and a newline */
#define IOS_HOSTNAME_FILE ".pocketgames-computer-name"

/* "DESKTOP-" and 7 characters of [A-Z0-9]: 15 characters, a valid NetBIOS name as it is */
#define IOS_HOSTNAME_PREFIX "DESKTOP-"
#define IOS_HOSTNAME_LEN    15

/* whether name[0..len) is a usable name: 1 to 15 characters of [A-Za-z0-9-], not starting or
 * ending with '-' */
int ios_hostname_valid( const char *name, size_t len );

/* a new name from rnd (uniform over the 36 characters: draws that would bias are drawn again)
 * into out, which holds at least IOS_HOSTNAME_LEN + 1 bytes */
typedef uint32_t (*ios_hostname_random_fn)(void);
void ios_hostname_make( char *out, ios_hostname_random_fn rnd );

/* the prefix's directory as ntdll finds it (spec 09): WINEPREFIX without trailing slashes, else
 * HOME/.wine; both must be absolute. 0 and the path in buf, or -1. */
int ios_hostname_prefix_dir( const char *wineprefix, const char *home, char *buf, size_t size );

/* the stored name of the prefix in dir, into out (at least IOS_HOSTNAME_LEN + 1 bytes):
 * - IOS_HOSTNAME_READ: the file held a valid name (only its first line counts);
 * - IOS_HOSTNAME_CREATED: there was no file: a new name, created with O_CREAT | O_EXCL, mode
 *   0600 (a creator that loses the race takes the winner's name);
 * - IOS_HOSTNAME_REPLACED: the file held no valid name: a new name written over it;
 * - IOS_HOSTNAME_NOT_STORED: the file couldn't be read or written (or dir is NULL): a new name
 *   for this run only.
 * Never derived from the device or from a path (container paths change with app updates). */
#define IOS_HOSTNAME_READ       0
#define IOS_HOSTNAME_CREATED    1
#define IOS_HOSTNAME_REPLACED   2
#define IOS_HOSTNAME_NOT_STORED 3
int ios_hostname_load( const char *dir, char *out, ios_hostname_random_fn rnd );

#endif /* MYIOSDECK_IOS_HOSTNAME_H */
