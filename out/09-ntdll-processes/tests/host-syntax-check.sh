#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# A best-effort syntax check of this tree's modified upstream files on a Linux host, with
# WINE_IOS defined, the merged sources of build-ios.sh and a host Wine build's generated headers.
# It is not part of run-host-tests.sh: files whose iOS code needs Darwin headers (Mach, IOKit)
# don't compile on Linux. It checks the files named on the command line, e.g.
#   WINE_BUILD=~/wine-build tests/host-syntax-check.sh syscall.c debug.c
# Environment: WINE_BUILD (default ~/wine-build), WINE_SRC (default refs/wine-11.4), CC.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TREE="$HERE/.."
WINE_SRC=${WINE_SRC:-"$HERE/../../../refs/wine-11.4"}
B=${WINE_BUILD:-"$HOME/wine-build"}
CC=${CC:-cc}
S=${TMPDIR:-/tmp}/host-syntax-check.$$
status=0

[ -f "$B/include/config.h" ] || { echo "no host Wine build at $B (set WINE_BUILD)"; exit 2; }
mkdir -p "$S" || exit 2
cp "$WINE_SRC"/dlls/ntdll/unix/*.c "$WINE_SRC"/dlls/ntdll/unix/*.h "$S"/ || exit 2
cp "$TREE"/ntdll-unix/*.c "$TREE"/ntdll-unix/*.h "$S"/ || exit 2
cp "$TREE"/../13-net-crypto-ios/ntdll-unix/socket.c "$TREE"/../13-net-crypto-ios/ntdll-unix/ios_afd.h "$S"/ || exit 2
printf '#include "%s/include/config.h"\n#include "ios_config.h"\n' "$B" > "$S/config.h"

for f in "$@"; do
    echo "== $f"
    # glibc declares what Darwin declares by default (dladdr, pipe2, sched_getcpu) only with
    # _GNU_SOURCE; build-ios.sh's loader.c defines
    extra="-D_GNU_SOURCE"
    [ "$f" = loader.c ] && extra="$extra -DBINDIR=\"/usr/local/wine\" -DLIBDIR=\"/usr/local\" -DDATADIR=\"/usr/local\" -DSYSTEMDLLPATH=\"\""
    # shellcheck disable=SC2086
    # As the iOS build (Apple clang): an undeclared function is an error. tests/darwin-shim stands
    # in for the Darwin headers the iOS parts include (searched last, after the system's).
    if ! $CC -fsyntax-only -std=gnu11 $extra -DWINE_IOS=1 -DWINE_UNIX_LIB -D__WINESRC__ -D_NTSYSTEM_ \
        -D_ACRTIMP= -DWINBASEAPI= -D_REENTRANT -Wall -Wno-unused-function -Wno-unused-variable \
        -Wno-unused-but-set-variable -Wno-unknown-pragmas -Werror=implicit-function-declaration \
        -DHAVE_LINUX_NTSYNC_H=1 -DIOS_USYNC=1 \
        -I"$S" -I"$TREE/../16-sync-ios/include" -I"$WINE_SRC/dlls/ntdll" -I"$B/dlls/ntdll" -I"$TREE/include" -I"$B/include" \
        -I"$WINE_SRC/include" -I"$TREE/../../interfaces" -idirafter "$HERE/darwin-shim" \
        -include "$HERE/darwin-shim/host_darwin.h" -include "$S/ios_noexit.h" -include "$S/ios_rename.h" "$S/$f"; then
        status=1
    fi
done
rm -rf "$S"
exit $status
