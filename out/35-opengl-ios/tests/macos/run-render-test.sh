#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# run-render-test.sh <angle out (macos)> <gl4es out (macos)> <work dir>
#
# Spec 35 Part J3, CI only (a macOS arm64 runner, which has a Metal device): builds pgl for
# macOS against build-angle.sh's and build-gl4es.sh's macOS products, links render_test.m with
# no Wine, and runs it. Without a Metal device the test reports SKIP and this script passes.
# Never run on the owner's PC (Part J: no GPU test there, in WSL neither).

set -eu
[ $# -eq 3 ] || { echo "usage: $0 <angle out> <gl4es out> <work dir>" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
part=$(cd "$here/../.." && pwd)
angle=$(cd "$1" && pwd)
gl4es=$(cd "$2" && pwd)
mkdir -p "$3"
work=$(cd "$3" && pwd)

[ "$(uname -s)" = Darwin ] || { echo "SKIP not macOS"; exit 0; }

sh "$part/pgl/build-pgl.sh" "$angle" "$gl4es" "$work/pgl" macos
# shellcheck disable=SC2046
xcrun -sdk macosx clang -arch arm64 -mmacosx-version-min=13.0 -fno-objc-arc -O1 -g \
    -I"$part/pgl" -I"$angle/include" -I"$gl4es/include" \
    "$here/render_test.m" "$work/pgl/libpgl.a" "$gl4es/libgl4es.a" "$angle/libangle.a" \
    $(cat "$angle/LINK.txt") -o "$work/render_test"
"$work/render_test"
