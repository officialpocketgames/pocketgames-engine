#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 16 Part J: builds and runs the host tests (Linux, any C11 compiler with pthreads; CC picks
# the compiler, default: gcc, then clang when present). The Mach semaphore is a POSIX semaphore
# and the address wait is the futex system call (src/ios_usync.c, src/ios_sync_cells.c).
#
# WINE_BUILD (optional): a host Wine build with generated headers (include/config.h). When set,
# the modified server and ntdll files of this spec are also compiled (syntax only) against it.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT="$HERE/.."
OUT=${TMPDIR:-/tmp}/ios_sync_tests.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

if [ -n "${CC:-}" ]; then
    COMPILERS=$CC
else
    COMPILERS=gcc
    command -v clang >/dev/null 2>&1 && COMPILERS="$COMPILERS clang"
fi

FLAGS="-std=gnu11 -O2 -g -Wall -Wextra -Werror -pthread -I$ROOT/include"
# TSAN=1: also under ThreadSanitizer (data races in the library and the tests)
[ "${TSAN:-0}" = 1 ] && FLAGS="$FLAGS -fsanitize=thread"
SRCS="$ROOT/src/ios_sync_cells.c $ROOT/src/ios_sync_config.c $ROOT/src/ios_fastsync_srv.c
      $ROOT/src/ios_fastsync_cli.c $ROOT/src/ios_sync_pace.c $ROOT/src/ios_usync.c"
TESTS="test_config_pace test_usync test_fastsync_model test_fastsync_client"

status=0
for cc in $COMPILERS; do
    for t in $TESTS; do
        echo "== $cc $t"
        # shellcheck disable=SC2086
        if ! $cc $FLAGS -o "$OUT/$t" "$HERE/$t.c" $SRCS; then
            echo "FAIL: $t does not build with $cc"
            status=1
            continue
        fi
        if ! "$OUT/$t"; then
            echo "FAIL: $t ($cc)"
            status=1
        fi
    done
done

# spec 00 F: no API an App Store build must not use, here and in the files added to other trees
REPO=$(cd "$ROOT/../.." && pwd)
echo "== check-app-store.sh"
sh "$REPO/check-app-store.sh" "$ROOT" \
    "$REPO/out/08-wineserver-ios/server/event.c" "$REPO/out/08-wineserver-ios/server/semaphore.c" \
    "$REPO/out/08-wineserver-ios/server/inproc_sync.c" "$REPO/out/08-wineserver-ios/server/async.c" \
    "$REPO/out/09-ntdll-processes/ntdll-unix/sync.c" || status=1
# private Darwin calls the grep above doesn't know (spec 00 B1): none in this part's sources
if grep -rnE '__ulock_(wait|wake)|__psynch_' "$ROOT/src" "$ROOT/include" "$ROOT/pe" |
   grep -vE ':[0-9]+:\s*(//|/?\*|#)'; then
    echo "FAIL: private Darwin call in out/16-sync-ios"
    status=1
fi

# the modified Wine files compile (syntax only) against a host Wine build
if [ -n "${WINE_BUILD:-}" ]; then
    echo "== Wine files against $WINE_BUILD"
    sh "$HERE/check-wine-files.sh" "$WINE_BUILD" || status=1
else
    echo "(WINE_BUILD not set: the Wine files are not compiled)"
fi

[ $status -eq 0 ] && echo "all host tests passed"
exit $status
