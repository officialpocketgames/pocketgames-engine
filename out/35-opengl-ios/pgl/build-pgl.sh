#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# build-pgl.sh <angle out dir> <gl4es out dir> <out dir> <ios|macos>
# build-pgl.sh none <out dir> <ios|macos>
#
# Spec 35 Part D: pgl as one static library, <out dir>/libpgl.a, for iOS arm64 (the app) or
# macOS arm64 (the render test). The ANGLE and gl4es dirs are build-angle.sh's and
# build-gl4es.sh's outputs (their include/). winemetal_thunks.h comes from spec 05's tree (DXMT,
# MIT): pgl calls winemetal's unix entries for the window's layer (D5.1).
# "none" builds pgl without its GL backend (pgl_none.c: pgl_init fails, OpenGL stays off), for a
# CI run in which ANGLE or gl4es couldn't be built, so the engine still links.
#
# The Objective-C files are built without ARC (manual retain/release).

set -eu

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)

if [ $# -eq 3 ] && [ "$1" = none ]; then
    mkdir -p "$2"
    out=$(cd "$2" && pwd)
    case "$3" in
        ios) sdk=iphoneos; arch="-arch arm64 -miphoneos-version-min=17.0" ;;
        macos) sdk=macosx; arch="-arch arm64 -mmacosx-version-min=13.0" ;;
        *) echo "$0: target must be ios or macos" >&2; exit 2 ;;
    esac
    CC=${CC:-"xcrun -sdk $sdk clang"}
    AR=${AR:-"xcrun -sdk $sdk ar"}
    rm -rf "$out/obj-pgl"
    mkdir -p "$out/obj-pgl"
    for s in pgl_pure.c pgl_none.c; do
        # shellcheck disable=SC2086
        $CC -std=gnu11 $arch -O2 -Wall -Wno-unused-parameter -Werror -fPIC -I"$here" -c "$here/$s" -o "$out/obj-pgl/${s%.c}.o"
    done
    rm -f "$out/libpgl.a"
    # shellcheck disable=SC2086
    $AR rcs "$out/libpgl.a" "$out/obj-pgl"/*.o
    echo "built $out/libpgl.a WITHOUT a GL backend (OpenGL stays off)"
    exit 0
fi

[ $# -eq 4 ] || { echo "usage: $0 <angle out> <gl4es out> <out dir> <ios|macos> | none <out dir> <ios|macos>" >&2; exit 2; }
angle=$(cd "$1" && pwd)
gl4es=$(cd "$2" && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
target=$4

case "$target" in
    ios) sdk=iphoneos; arch="-arch arm64 -miphoneos-version-min=17.0" ;;
    macos) sdk=macosx; arch="-arch arm64 -mmacosx-version-min=13.0" ;;
    *) echo "$0: target must be ios or macos" >&2; exit 2 ;;
esac
CC=${CC:-"xcrun -sdk $sdk clang"}
AR=${AR:-"xcrun -sdk $sdk ar"}

obj="$out/obj-pgl"
rm -rf "$obj"
mkdir -p "$obj"
CFLAGS="$arch -O2 -g -Wall -Wextra -Wno-unused-parameter -fPIC \
 -I$here -I$angle/include -I$gl4es/include -I$repo/out/05-dxmt-ios/dxmt/src/winemetal"

failed=""
for s in pgl_pure.c pgl_core.c; do
    # shellcheck disable=SC2086
    $CC -std=gnu11 $CFLAGS -Werror -c "$here/$s" -o "$obj/${s%.c}.o" || failed="$failed $s"
done
# (no -Werror: pgl_present.m includes DXMT's winemetal headers, which aren't warning-clean)
for s in pgl_present.m pgl_pool.m; do
    # shellcheck disable=SC2086
    $CC -fno-objc-arc $CFLAGS -c "$here/$s" -o "$obj/${s%.m}.o" || failed="$failed $s"
done
[ -z "$failed" ] || { echo "build-pgl: FAILED:$failed" >&2; exit 1; }

rm -f "$out/libpgl.a"
# shellcheck disable=SC2086
$AR rcs "$out/libpgl.a" "$obj"/*.o
echo "built $out/libpgl.a"
