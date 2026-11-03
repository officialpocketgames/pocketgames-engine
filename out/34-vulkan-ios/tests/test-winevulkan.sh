#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part M (Part C): winevulkan's unix side on the host.
#   1. gen-entries.sh on the Wine tree's loader_thunks.h, and tests/test_winevulkan.c (the entry
#      list, the call statistics' arithmetic)
#   2. when WINE_BUILD names a host Wine build (ci/wine-host-build.sh; the CI host-tests job sets
#      it): the patches apply to the upstream files with no fuzz (as macOS's patch needs), and the
#      patched vulkan.c, upstream vulkan_thunks.c and winevulkan_ios.c pass -fsyntax-only with
#      build-ios.sh's defines, include order and forced include (the C_ASSERTs on the tables
#      included). Without it: SKIPPED, said so.
# Environment: CC, WINE_SRC (the Wine 11.4 tree, read only), WINE_BUILD, TMPDIR.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PART=$(cd "$HERE/.." && pwd)
REPO=$(cd "$PART/../.." && pwd)
WINE=${WINE_SRC:-"$REPO/refs/wine-11.4"}
CC=${CC:-cc}
WV="$PART/winevulkan-unix"
T=${TMPDIR:-/tmp}/vk34-winevulkan.$$
mkdir -p "$T/dlls/winevulkan"
trap 'rm -rf "$T"' EXIT

sh "$WV/gen-entries.sh" "$WINE/dlls/winevulkan/loader_thunks.h" "$T/winevulkan_ios_entries.h"
want=$(awk '/^enum unix_call$/{s=1;next} s&&/unix_count,/{exit} s&&/^ *unix_/{n++} END{print n}' "$WINE/dlls/winevulkan/loader_thunks.h")
got=$(sed -n 's/^#define WINEVULKAN_IOS_ENTRY_COUNT \([0-9]*\)$/\1/p' "$T/winevulkan_ios_entries.h")
[ "$want" = "$got" ] || { echo "FAIL: gen-entries.sh found $got entries, the enum has $want"; exit 1; }
# shellcheck disable=SC2086
$CC -std=c11 -Wall -Wextra -Werror -O1 -g -I"$T" -I"$WV" "$HERE/test_winevulkan.c" "$WV/winevulkan_ios_pure.c" -o "$T/test_winevulkan"
"$T/test_winevulkan"

if [ -z "${WINE_BUILD:-}" ] || [ ! -f "${WINE_BUILD:-/nonexistent}/include/config.h" ]; then
    echo "SKIPPED: winevulkan syntax check (no host Wine build: set WINE_BUILD)"
    exit 0
fi
for f in vulkan.c vulkan_thunks.c vulkan_private.h vulkan_thunks.h vulkan_loader.h loader_thunks.h; do
    cp "$WINE/dlls/winevulkan/$f" "$T/dlls/winevulkan/"
done
for p in "$WV"/patches/*.patch; do
    patch -p1 -N -s -F 0 -d "$T" < "$p" || {
        echo "FAIL: patch $(basename "$p") doesn't apply with no fuzz"
        cat "$T"/dlls/winevulkan/*.rej 2>/dev/null | head -40
        exit 1
    }
    echo "patch $(basename "$p") applies with no fuzz"
done
cp "$T/winevulkan_ios_entries.h" "$WV"/winevulkan_ios.c "$WV"/winevulkan_ios_pure.h "$WV"/winevulkan_ios_config.h "$T/dlls/winevulkan/"
status=0
for f in vulkan.c vulkan_thunks.c winevulkan_ios.c; do
    if (cd "$T/dlls/winevulkan" && $CC -std=gnu11 -fsyntax-only -Wall -Wno-unused-function -Wno-unused-variable \
            -Wno-unused-but-set-variable -Wno-attributes -Werror=implicit-function-declaration \
            -DWINE_IOS=1 -DWINE_UNIX_LIB -D__WINESRC__ -D_REENTRANT -DIOS_VK_WOW64=0 \
            -I. -I"$REPO/out/09-ntdll-processes/include" -I"$WINE_BUILD/include" -I"$WINE/include" \
            -include winevulkan_ios_config.h "$f"); then
        echo "$f OK"
    else
        echo "$f FAILED"; status=1
    fi
done
exit $status
