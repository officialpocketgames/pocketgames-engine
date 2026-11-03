#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 clarification 1, item 6: builds the macOS render test (vk-render-test.c with
# ../common/vkgx.c) against the macOS MoltenVK that build-moltenvk.sh --platform macos made.
# The macOS CI only (job vk-render-macos), which then runs it; never on the owner's PC.
#
#   build-render-test.sh <moltenvk macOS out dir> <out dir>
#
# <moltenvk macOS out dir> holds libmoltenvk.a and include/ (vulkan/vulkan.h). Writes
# <out dir>/vk-render-test.

set -eu
[ $# -eq 2 ] || { echo "usage: $0 <moltenvk macOS out dir> <out dir>" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
common="$here/../common"
mvk=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
[ -f "$mvk/libmoltenvk.a" ] && [ -f "$mvk/include/vulkan/vulkan.h" ] || { echo "build-render-test: no libmoltenvk.a and include/ in $mvk" >&2; exit 1; }
[ "$(uname -s)" = Darwin ] || { echo "build-render-test: needs macOS" >&2; exit 1; }

sh "$common/gen-vkgx-shaders.sh" "$out/vkgx_shaders.h"
xcrun clang -std=gnu11 -O2 -Wall -Wextra -Werror -arch arm64 -mmacosx-version-min=13.0 \
    -I"$mvk/include" -I"$out" -I"$common" "$here/vk-render-test.c" "$common/vkgx.c" "$mvk/libmoltenvk.a" \
    -framework Metal -framework QuartzCore -framework IOSurface -framework IOKit -framework Foundation \
    -framework CoreGraphics -framework AppKit -lc++ -o "$out/vk-render-test"
echo "build-render-test: $out/vk-render-test"
