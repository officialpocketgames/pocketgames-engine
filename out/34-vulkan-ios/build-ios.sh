#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# build-ios.sh <wine-11.4 tree> <wine build dir> <out dir> <moltenvk dir>
#
# Spec 34 Part A4: part 34's iOS static libraries (arm64, minimum iOS 17.0), into <out dir>:
#   libios_vk_layer.a      the layer (Part E, MIT), built against MoltenVK's Vulkan headers
#   libwinevulkan_unix.a   winevulkan's unix side (Part C, LGPL), winevulkan-unix/build-ios.sh
#   libmoltenvk.a          MoltenVK (Part B, Apache-2.0), copied from <moltenvk dir> with its
#                          include/, LICENSES/ and BUILDINFO.txt
# <moltenvk dir> is build-moltenvk.sh's <out-dir>. The app links all three with the other engine
# libraries (Part I), so the relinking kit carries them.
#
# Environment: CC, AR (default: xcrun -sdk iphoneos clang / ar), EXTRA_CFLAGS.

set -u

if [ $# -ne 4 ]; then
  echo "usage: $0 <wine-11.4 tree> <wine build dir> <out dir> <moltenvk dir>" >&2
  exit 2
fi

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

# refs/ is read only: refuse an out dir that is, or would be created, inside it
REFS=$(cd "$REPO/refs" 2>/dev/null && pwd -P) || REFS=""
if [ -n "$REFS" ]; then
  p=$3; rest=""
  while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
  case "$(cd "$p" && pwd -P)$rest/" in
    "$REFS/"*) echo "$0: refusing an out dir inside refs/: $3" >&2; exit 1 ;;
  esac
fi

WINE=$(cd "$1" && pwd) || exit 1
BUILD=$(cd "$2" && pwd) || exit 1
mkdir -p "$3" || exit 1
OUT=$(cd "$3" && pwd) || exit 1
MVK=$(cd "$4" && pwd) || exit 1
[ -f "$MVK/libmoltenvk.a" ] && [ -f "$MVK/include/vulkan/vulkan_core.h" ] ||
  { echo "$0: $MVK isn't build-moltenvk.sh's output (no libmoltenvk.a or include/vulkan/vulkan_core.h)" >&2; exit 1; }

CC=${CC:-"xcrun -sdk iphoneos clang"}
AR=${AR:-"xcrun -sdk iphoneos ar"}
LAYER="$HERE/layer"
WORK="$OUT/work-layer"
OBJ="$OUT/obj-layer"
LOG="$OUT/log"
rm -rf "$WORK" "$OBJ"
mkdir -p "$WORK" "$OBJ" "$LOG" || exit 1

# ---------------------------------------------------------------- 1. the layer (Part E)
sh "$LAYER/gen-features.sh" "$MVK/include/vulkan/vulkan_core.h" "$WORK/ios_vk_features_gen.h" || exit 1
COMMON="-arch arm64 -miphoneos-version-min=17.0 -O2 -fPIC -fno-strict-aliasing -fno-omit-frame-pointer \
 -Wall -Wextra ${EXTRA_CFLAGS:-}"
INCS="-I$LAYER -I$WORK -I$MVK/include"
failed=""
for f in ios_vk_layer.c ios_vk_features.c ios_vk_release.c ios_vk_pure.c ios_vk_platform.c ios_vk_pool.m; do
  n=${f%.*}
  case "$f" in *.m) lang="-x objective-c" ;; *) lang="-std=gnu11" ;; esac
  # shellcheck disable=SC2086
  if $CC $COMMON $lang $INCS -c "$LAYER/$f" -o "$OBJ/$n.o" > "$LOG/$n.log" 2>&1; then
    echo "$n OK"
  else
    echo "$n FAILED"
    failed="$failed $n"
  fi
done
if [ -n "$failed" ]; then
  echo "FAILED:$failed"
  for f in $failed; do echo "==== $f"; head -60 "$LOG/$f.log"; done
  exit 1
fi
rm -f "$OUT/libios_vk_layer.a"
# shellcheck disable=SC2086
$AR rcs "$OUT/libios_vk_layer.a" "$OBJ"/*.o || exit 1
echo "built $OUT/libios_vk_layer.a"

# ---------------------------------------------------------------- 2. winevulkan's unix side (Part C)
sh "$HERE/winevulkan-unix/build-ios.sh" "$WINE" "$BUILD" "$OUT/winevulkan" || exit 1
cp "$OUT/winevulkan/libwinevulkan_unix.a" "$OUT/" || exit 1
mkdir -p "$LOG/winevulkan" && cp "$OUT/winevulkan/log/"* "$LOG/winevulkan/" 2>/dev/null

# ---------------------------------------------------------------- 3. MoltenVK (Part B)
cp "$MVK/libmoltenvk.a" "$MVK/BUILDINFO.txt" "$OUT/" || exit 1
rm -rf "$OUT/LICENSES" && cp -R "$MVK/LICENSES" "$OUT/LICENSES" || exit 1
echo "built $OUT: libios_vk_layer.a libwinevulkan_unix.a libmoltenvk.a"
