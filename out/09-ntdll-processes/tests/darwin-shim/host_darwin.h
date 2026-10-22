/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */
/*
 * Host syntax check only (tests/host-syntax-check.sh, forced include): Darwin constants that the
 * iOS code uses outside the Mach headers, with Darwin's values from its public <fcntl.h>. Never
 * part of the iOS build.
 */
#ifndef MYIOSDECK_HOST_DARWIN_H
#define MYIOSDECK_HOST_DARWIN_H

#include <fcntl.h>

#ifndef F_GETPATH
#define F_GETPATH 50   /* fcntl: the path of an open descriptor */
#endif

#endif
