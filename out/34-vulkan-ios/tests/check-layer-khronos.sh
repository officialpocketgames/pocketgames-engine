#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part E against Khronos' Vulkan-Headers at the revision the MoltenVK fork pins, the
# headers the iOS build compiles the layer with (build-moltenvk.sh's include/): gen-features.sh
# on their vulkan_core.h, then every layer file that builds off Apple too, with -fsyntax-only and
# build-ios.sh's warnings. An identifier those headers don't declare (the first iOS layer build
# stopped on a provisional extension's sType) fails here, on Linux, without a Mac. The CI's
# vktest job runs it on the headers it fetches; tests/run-host-tests.sh runs it when
# VULKAN_HEADERS names such an include dir.
#
#   check-layer-khronos.sh <Vulkan-Headers include dir>
#
# Environment: CC (default cc), EXTRA_CFLAGS, TMPDIR.

set -eu
[ $# -eq 1 ] || { echo "usage: $0 <Vulkan-Headers include dir>" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
LAYER="$HERE/../layer"
CC=${CC:-cc}
inc=$(cd "$1" && pwd)
[ -f "$inc/vulkan/vulkan_core.h" ] || { echo "check-layer-khronos: no vulkan/vulkan_core.h in $inc" >&2; exit 1; }
T=$(mktemp -d "${TMPDIR:-/tmp}/vk34-khronos.XXXXXX")
trap 'rm -rf "$T"' EXIT

sh "$LAYER/gen-features.sh" "$inc/vulkan/vulkan_core.h" "$T/ios_vk_features_gen.h"
status=0
# ios_vk_pool.m is Objective-C (its plain-C twin ios_vk_pool.c stands in); ios_vk_platform.c has
# its non-Apple path here
for f in ios_vk_layer.c ios_vk_features.c ios_vk_release.c ios_vk_pure.c ios_vk_pool.c ios_vk_platform.c; do
    # shellcheck disable=SC2086
    if $CC -std=gnu11 -fsyntax-only -O2 -fno-strict-aliasing -Wall -Wextra ${EXTRA_CFLAGS:-} -I"$LAYER" -I"$T" -I"$inc" \
            "$LAYER/$f" > "$T/$f.log" 2>&1; then
        if [ -s "$T/$f.log" ]; then echo "$f OK, with warnings:"; head -n 20 "$T/$f.log"; else echo "$f OK"; fi
    else
        echo "$f FAILED"; head -n 40 "$T/$f.log"; status=1
    fi
done
[ $status -eq 0 ] && echo "check-layer-khronos: the layer compiles against $(grep -m1 -E '^#define VK_HEADER_VERSION ' "$inc/vulkan/vulkan_core.h" | awk '{print "Vulkan-Headers " $3}')"
exit $status
