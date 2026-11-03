#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# build-ios.sh <wine-11.4 tree> <wine build dir> <out dir>
#
# Spec 34 Part C2: winevulkan's unix side for iOS arm64 (minimum iOS 17.0, -O2) as one static
# library, <out dir>/libwinevulkan_unix.a.
#
# Like spec 12's win32u, this repository holds no copy of the upstream files: this script copies
# dlls/winevulkan/'s vulkan.c, vulkan_thunks.c and headers from the given Wine tree into
# <out dir>/work, applies patches/*.patch in name order, writes winevulkan_ios_entries.h from
# loader_thunks.h (gen-entries.sh), adds winevulkan_ios*.c/.h and compiles vulkan.c,
# vulkan_thunks.c, winevulkan_ios.c and winevulkan_ios_pure.c with WINE_IOS=1, WINE_UNIX_LIB and
# __WINESRC__. Spec 17's header folder (out/09-ntdll-processes/include/) comes before the Wine
# tree's include/ (spec 12 clarification 1). winevulkan_ios_config.h is included first in every
# file (the build's config.h, the renames of C2, IOS_VK_WOW64).
#
# Environment:
#   CC, AR          compiler / archiver (default: xcrun -sdk iphoneos clang / ar)
#   EXTRA_CFLAGS    appended to every compile
#   IOS_VK_WOW64    1 builds Part G's WoW64 entry (not in phase 1; default 0)

set -u

if [ $# -ne 3 ]; then
  echo "usage: $0 <wine-11.4 tree> <wine build dir> <out dir>" >&2
  exit 2
fi

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)

# refs/ is read only: refuse an out dir that is, or would be created, inside it (checked on the
# nearest existing parent, links resolved, before anything is created)
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

WORK="$OUT/work"
SRC="$WORK/dlls/winevulkan"
OBJ="$OUT/obj"
LOG="$OUT/log"
rm -rf "$WORK" "$OBJ" "$LOG"
mkdir -p "$SRC" "$OBJ" "$LOG" || exit 1

# ---------------------------------------------------------------- 1. sources
for f in vulkan.c vulkan_thunks.c vulkan_private.h vulkan_thunks.h vulkan_loader.h loader_thunks.h; do
  cp "$WINE/dlls/winevulkan/$f" "$SRC/" || exit 1
done
for p in "$HERE"/patches/*.patch; do
  [ -f "$p" ] || continue
  if patch -p1 -N -s -d "$WORK" < "$p" > "$LOG/patch-$(basename "$p").log" 2>&1; then
    echo "patch $(basename "$p") OK"
  else
    echo "patch $(basename "$p") FAILED"
    cat "$LOG/patch-$(basename "$p").log"
    exit 1
  fi
done
sh "$HERE/gen-entries.sh" "$SRC/loader_thunks.h" "$SRC/winevulkan_ios_entries.h" || exit 1
cp "$HERE"/winevulkan_ios.c "$HERE"/winevulkan_ios_pure.c "$HERE"/winevulkan_ios_pure.h \
   "$HERE"/winevulkan_ios_config.h "$SRC/" || exit 1

# ---------------------------------------------------------------- 2. compile
CC=${CC:-"xcrun -sdk iphoneos clang"}
AR=${AR:-"xcrun -sdk iphoneos ar"}
COMMON="-arch arm64 -miphoneos-version-min=17.0 -O2 -std=gnu11 -fPIC -fno-strict-aliasing \
 -fasynchronous-unwind-tables -fno-omit-frame-pointer -ffixed-x18 ${EXTRA_CFLAGS:-}"
DEFS="-DWINE_IOS=1 -DWINE_UNIX_LIB -D__WINESRC__ -D_REENTRANT -DIOS_VK_WOW64=${IOS_VK_WOW64:-0}"
WARN="-Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
 -Wno-pragma-pack -Wno-deprecated-declarations -Wno-unknown-pragmas"
INCS="-I$SRC -I$REPO/out/09-ntdll-processes/include -I$BUILD/include -I$WINE/include"
FORCE="-include $SRC/winevulkan_ios_config.h"

failed=""
for n in vulkan vulkan_thunks winevulkan_ios winevulkan_ios_pure; do
  # shellcheck disable=SC2086
  if $CC $COMMON $DEFS $WARN $INCS $FORCE -c "$SRC/$n.c" -o "$OBJ/$n.o" > "$LOG/$n.log" 2>&1; then
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

# ---------------------------------------------------------------- 3. one library
rm -f "$OUT/libwinevulkan_unix.a"
# shellcheck disable=SC2086
$AR rcs "$OUT/libwinevulkan_unix.a" "$OBJ"/*.o || exit 1
echo "built $OUT/libwinevulkan_unix.a"
