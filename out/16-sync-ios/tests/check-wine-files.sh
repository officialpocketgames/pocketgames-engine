#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 16 Part J: compiles (syntax only) the Wine files this spec changes, against a host Wine
# build's generated headers, with and without WINE_IOS. Linux is not Darwin: files whose iOS
# paths need Mach or Apple headers are compiled without WINE_IOS only (listed below), the rest
# both ways. Usage: check-wine-files.sh <wine build dir> [wine source tree]

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
BUILD=$(cd "$1" && pwd)
SRC=${2:-"$ROOT/refs/wine-11.4"}
SRC=$(cd "$SRC" && pwd)
CC=${CC:-gcc}
SYNC_INC="$ROOT/out/16-sync-ios/include"
STAGE=${TMPDIR:-/tmp}/ios_sync_wine.$$
trap 'rm -rf "$STAGE"' EXIT
status=0

# clang counts "discards qualifiers" as an incompatible-pointer-types error; gcc only warns
# (-Wdiscarded-qualifiers). Upstream server.c has one such line: a warning for both.
CC_EXTRA=""
if "$CC" --version 2>/dev/null | grep -qi clang; then
    CC_EXTRA="-Wno-error=incompatible-pointer-types-discards-qualifiers"
fi

check() {   # name, then the compiler arguments
    name=$1; shift
    # shellcheck disable=SC2086
    if "$CC" -std=gnu11 -fsyntax-only -Wall -Wno-unused-function -Wno-pointer-sign \
        -Werror=implicit-function-declaration -Werror=incompatible-pointer-types $CC_EXTRA "$@" 2>"$STAGE/err"; then
        echo "  ok   $name"
        if [ -s "$STAGE/err" ]; then sed 's/^/       /' "$STAGE/err"; fi
    else
        echo "  FAIL $name"
        cat "$STAGE/err"
        status=1
    fi
}

# ---- the server (spec 08's tree over upstream) ----
mkdir -p "$STAGE/server"
cp "$SRC"/server/*.c "$SRC"/server/*.h "$STAGE/server/"
cp "$ROOT"/out/08-wineserver-ios/server/*.c "$ROOT"/out/08-wineserver-ios/server/*.h "$STAGE/server/"
cp "$ROOT"/out/09-ntdll-processes/ntdll-unix/ios_syscall_frame.h "$STAGE/server/"   # as build-ios.sh does
SERVER_INC="-I$STAGE/server -I$BUILD/include -I$SRC/include -D__WINESRC__ -D_REENTRANT"
# the shared syscall-frame header (spec 11 A2) is arm64's: on an x86-64 host, winnt.h has the
# same 16-byte register type only as ARM_NEON128
for f in event.c semaphore.c inproc_sync.c async.c; do
    # shellcheck disable=SC2086
    check "server/$f (WINE_IOS)" -DWINE_IOS=1 -DHAVE_LINUX_NTSYNC_H=1 -DIOS_USYNC=1 -I"$SYNC_INC" -DNEON128=ARM_NEON128 \
        -include "$BUILD/include/config.h" $SERVER_INC "$STAGE/server/$f"
done
for f in event.c semaphore.c inproc_sync.c async.c thread.c process.c; do
    # shellcheck disable=SC2086
    check "server/$f (upstream paths)" $SERVER_INC "$STAGE/server/$f"
done

# ---- ntdll's unix side (spec 09's tree over upstream, spec 17's header overrides first) ----
mkdir -p "$STAGE/ntdll"
cp "$SRC"/dlls/ntdll/unix/*.c "$SRC"/dlls/ntdll/unix/*.h "$STAGE/ntdll/"
cp "$ROOT"/out/09-ntdll-processes/ntdll-unix/*.c "$ROOT"/out/09-ntdll-processes/ntdll-unix/*.h "$STAGE/ntdll/"
NTDLL_INC="-I$STAGE/ntdll -I$SRC/dlls/ntdll -I$BUILD/dlls/ntdll -I$ROOT/out/09-ntdll-processes/include
 -I$BUILD/include -I$SRC/include -DWINE_UNIX_LIB -D__WINESRC__ -D_NTSYSTEM_ -D_ACRTIMP= -DWINBASEAPI= -D_REENTRANT"
for f in sync.c server.c; do
    # shellcheck disable=SC2086
    check "ntdll/$f (WINE_IOS)" -DWINE_IOS=1 -DHAVE_LINUX_NTSYNC_H=1 -DIOS_USYNC=1 -I"$SYNC_INC" -DNEON128=ARM_NEON128 \
        -include "$BUILD/include/config.h" -include "$STAGE/ntdll/ios_config.h" \
        -include "$STAGE/ntdll/ios_noexit.h" -include "$STAGE/ntdll/ios_rename.h" $NTDLL_INC "$STAGE/ntdll/$f"
    # shellcheck disable=SC2086
    check "ntdll/$f (upstream paths)" -include "$BUILD/include/config.h" $NTDLL_INC "$STAGE/ntdll/$f"
done

# ---- the PE file (Part F), with the flags the host build uses for PE sources (64-bit) ----
PE_FLAGS="-m64 -I$BUILD/dlls/ntdll -I$SRC/dlls/ntdll -I$BUILD/include -I$SRC/include -I$SRC/include/msvcrt
 -D_MSVCR_VER=0 -D__WINESRC__ -D_NTSYSTEM_ -D_ACRTIMP= -DWINBASEAPI= -D_WIN32 -mlong-double-64
 -fno-builtin -fshort-wchar -Wno-format"
# gcc's whole-file Microsoft calling convention; clang has no -mabi= for x86-64 (Wine's headers
# mark the Windows API functions ms_abi themselves, which is enough for a syntax check)
if ! "$CC" --version 2>/dev/null | grep -qi clang; then PE_FLAGS="$PE_FLAGS -mabi=ms"; fi
# shellcheck disable=SC2086
check "pe/dlls/ntdll/sync.c (64-bit)" $PE_FLAGS "$ROOT/out/16-sync-ios/pe/dlls/ntdll/sync.c"

[ $status -eq 0 ] && echo "  the Wine files compile"
exit $status
