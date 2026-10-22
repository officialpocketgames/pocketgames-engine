#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# CI: a host configure of refs/wine-11.4 plus "make include/all", the "Wine build dir" that the
# parts' build-ios.sh (macOS) and run-host-tests.sh (Linux) take for include/config.h, Wine's
# generated headers and tools/widl.
#
#   [MINGW_BIN=<dir>] ci/wine-host-build.sh <wine-11.4 tree> <build dir>
#
# The configure options are the ones out/14-media-audio-ios/tests/run-host-tests.sh uses for
# its own header build. On an aarch64 host Wine's configure requires a PE cross compiler
# ("PE cross-compilation is required for aarch64"): there MINGW_BIN names the bin/ of an
# llvm-mingw (CI: the one spec 19's build-pe.sh pins, unpacked by its toolchain step), which is
# put first in PATH, and --with-mingw replaces --without-mingw. Nothing PE is built: only
# include/all. A finished build leaves <build dir>/.ci-done holding the options, and a second
# run with the same options does nothing.
# Logs: <build dir>/ci-configure.log, <build dir>/ci-make.log.

set -eu

[ $# -eq 2 ] || { echo "usage: $0 <wine-11.4 tree> <build dir>" >&2; exit 2; }
src=$(cd "$1" && pwd)
mkdir -p "$2"
build=$(cd "$2" && pwd)

mingw="--without-mingw"
if [ -n "${MINGW_BIN:-}" ]; then
    [ -x "$MINGW_BIN/aarch64-w64-mingw32-clang" ] || { echo "::error::wine-host-build: no llvm-mingw in $MINGW_BIN"; exit 1; }
    PATH="$MINGW_BIN:$PATH"
    mingw="--with-mingw"
fi
opts="--enable-win64 $mingw --without-x --without-freetype --without-gstreamer --disable-tests"
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

if [ -f "$build/.ci-done" ] && [ "$(cat "$build/.ci-done")" = "$opts" ] && [ -f "$build/include/config.h" ]; then
    echo "wine-host-build: $build already built ($opts)"
    exit 0
fi

# Wine needs bison >= 3.0; macOS ships 2.3, Homebrew's is keg-only.
for d in /opt/homebrew/opt/bison/bin /usr/local/opt/bison/bin /opt/homebrew/opt/flex/bin /usr/local/opt/flex/bin; do
    [ -d "$d" ] && PATH="$d:$PATH"
done
export PATH
echo "wine-host-build: $(uname -sm), $(bison --version | head -n 1), $(flex --version | head -n 1)"
echo "wine-host-build: configure $opts"

rm -f "$build/.ci-done"
# shellcheck disable=SC2086
if ! (cd "$build" && "$src/configure" $opts) > "$build/ci-configure.log" 2>&1; then
    tail -n 40 "$build/ci-configure.log"
    echo "::error::wine-host-build: configure failed: $(grep -m 1 '^configure: error' "$build/ci-configure.log" || echo 'see ci-configure.log')"
    exit 1
fi
grep -E '^configure: (WARNING|error)' "$build/ci-configure.log" | head -n 40 || true

if ! (cd "$build" && make -j"$jobs" include/all) > "$build/ci-make.log" 2>&1; then
    grep -nE 'error|Error' "$build/ci-make.log" | head -n 40 || true
    tail -n 40 "$build/ci-make.log"
    echo "::error::wine-host-build: make include/all failed (see ci-make.log)"
    exit 1
fi
for f in include/config.h include/wine/server_protocol.h include/mfobjects.h include/audioclient.h tools/widl/widl; do
    [ -e "$build/$f" ] || [ -e "$src/$f" ] || echo "::warning::wine-host-build: $f was not made"
done
printf '%s' "$opts" > "$build/.ci-done"
echo "wine-host-build: done in $build"
