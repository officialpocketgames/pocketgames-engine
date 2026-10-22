#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# syntax-check-patched.sh <host wine build dir> [file.c ...]
#
# Spec 12: a host check of the Wine-dependent sources, short of the iOS build. Copies
# refs/wine-11.4's dlls/win32u and include/ntuser.h to a scratch tree in $TMPDIR, applies
# patches/*.patch in order (as build-ios.sh does), adds win32u-unix/, and runs
# `$CC -fsyntax-only -DWINE_IOS` with build-ios.sh's defines, include order and forced includes
# on the given files (default: the w32ios_*.c files that include Wine headers, and sysparams.c).
# The build dir gives config.h and the headers generated from IDL (a host build of Wine 11.4).
# Exits non-zero when a patch fails or a file has an error. Never writes into refs/.

set -eu
[ $# -ge 1 ] || { echo "usage: $0 <host wine build dir> [file.c ...]" >&2; exit 2; }
BUILD=$(cd "$1" && pwd)
shift
HERE=$(cd "$(dirname "$0")" && pwd)
PART=$(cd "$HERE/.." && pwd)
REPO=$(cd "$PART/../.." && pwd)
WINE="$REPO/refs/wine-11.4"
CC=${CC:-cc}
W=${TMPDIR:-/tmp}/w32ios-syntax.$$
trap 'rm -rf "$W"' EXIT

mkdir -p "$W/dlls/win32u/dibdrv" "$W/include"
cp "$WINE"/dlls/win32u/*.c "$WINE"/dlls/win32u/*.h "$W/dlls/win32u/"
cp "$WINE"/dlls/win32u/dibdrv/*.c "$WINE"/dlls/win32u/dibdrv/*.h "$W/dlls/win32u/dibdrv/"
cp "$WINE/include/ntuser.h" "$W/include/"
# Many hunks have less context after the change than before it. GNU patch takes such a hunk to
# belong at the end of the file unless fuzz evens it out (macOS's patch, used by CI, doesn't), so
# this host check allows more fuzz than GNU patch's default 2.
for p in "$PART"/patches/*.patch; do
  patch -p1 -N -s -F "${PATCH_FUZZ:-5}" -d "$W" < "$p" || { echo "patch $(basename "$p") FAILED"; exit 1; }
done
mv "$W/dlls/win32u/freetype.c" "$W/dlls/win32u/freetype_upstream.inc"
cp "$PART"/win32u-unix/* "$W/dlls/win32u/"

if [ $# -eq 0 ]; then
  set -- sysparams.c w32ios_proc.c w32ios_display.c w32ios_desktop.c w32ios_driver.c \
         w32ios_gamepad.c w32ios_init.c w32ios_kmt.c w32ios_mem.c
fi

cd "$W/dlls/win32u"
status=0
for f in "$@"; do
  if $CC -std=gnu11 -fsyntax-only -Wall -Wno-unused-function -Wno-unused-variable \
       -Wno-unused-but-set-variable -Wno-attributes -Wno-address \
       -DWINE_IOS=1 -DWINE_UNIX_LIB -D__WINESRC__ -D_WIN32U_ -D_REENTRANT \
       -I. -I"$W/include" -I"$REPO/out/09-ntdll-processes/include" -I"$BUILD/include" \
       -I"$WINE/include" -I"$REPO/interfaces" \
       -include w32ios_config.h -include w32ios_rename.h "$f"; then
    echo "$f OK"
  else
    echo "$f FAILED"; status=1
  fi
done
exit $status
