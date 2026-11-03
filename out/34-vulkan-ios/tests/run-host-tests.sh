#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part M: the host tests of part 34 (Linux/WSL, any C11 compiler, -Wall -Wextra -Werror,
# no GPU and no Vulkan driver), then check-app-store.sh on the part.
#
#   tests/run-host-tests.sh
#
# Environment: CC (default cc), WINE_SRC (the Wine 11.4 tree; default <repo>/refs/wine-11.4,
# read only: its include/wine/vulkan.h and dlls/winevulkan/ give the declarations), TMPDIR.
#   1. the layer (Part E) against a fake driver: tests/test_layer.c
#   2. gen-features.sh on Wine's vulkan.h: the table compiles with its layout checks
#   3. winevulkan's unix tables (Part C): the generated stat entries and the time-domain rule
#   4. build-moltenvk.sh's checks (Part B3) on fake archives: tests/test-moltenvk-checks.sh
#   5. the build scripts' syntax and dry runs: tests/test-build-scripts.sh
#   6. vktest.c (Part M6) passes -fsyntax-only for Windows x86_64 and i686 (built in CI, run only
#      on the device): tests/windows/syntax-check-vktest.sh
# Exits non-zero on any failure.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PART=$(cd "$HERE/.." && pwd)
REPO=$(cd "$PART/../.." && pwd)
WINE=${WINE_SRC:-"$REPO/refs/wine-11.4"}
CC=${CC:-cc}
OUT=${TMPDIR:-/tmp}/vk34-host-tests.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT

[ -f "$WINE/include/wine/vulkan.h" ] || { echo "run-host-tests: no Wine tree at $WINE (set WINE_SRC)"; exit 1; }
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -O1 -g -pthread"
LAYER="$PART/layer"

# ---------------------------------------------------------------- 2. the feature table
echo "== gen-features.sh"
sh "$LAYER/gen-features.sh" "$WINE/include/wine/vulkan.h" "$OUT/ios_vk_features_gen.h"
for s in VkPhysicalDeviceVulkan11Features VkPhysicalDeviceVulkan12Features VkPhysicalDeviceVulkan13Features \
         VkPhysicalDeviceVulkan14Features VkPhysicalDeviceTransformFeedbackFeaturesEXT \
         VkPhysicalDeviceRobustness2FeaturesKHR VkPhysicalDevice8BitStorageFeatures \
         VkPhysicalDeviceShaderFloat16Int8Features VkPhysicalDeviceImage2DViewOf3DFeaturesEXT; do
    grep -q "^IVK_FEATURE_STRUCT($s," "$OUT/ios_vk_features_gen.h" || { echo "FAIL: $s not in the generated table"; exit 1; }
done
grep -q '^IVK_CORE_FEATURES(55,' "$OUT/ios_vk_features_gen.h" || { echo "FAIL: VkPhysicalDeviceFeatures"; exit 1; }

# ---------------------------------------------------------------- 1. the layer
echo "== layer (Part E) against a fake driver"
# shellcheck disable=SC2086
$CC $CFLAGS -DIOS_VK_WINE_HEADERS -DIOS_VK_DRIVER_GIPA=fake_vkGetInstanceProcAddr \
    -I"$LAYER" -I"$OUT" -I"$WINE/include" \
    "$HERE/test_layer.c" "$LAYER/ios_vk_layer.c" "$LAYER/ios_vk_features.c" "$LAYER/ios_vk_release.c" \
    "$LAYER/ios_vk_pure.c" -o "$OUT/test_layer"
"$OUT/test_layer"

# the platform and pool files compile on a non-Apple host too (the app builds them for iOS)
# shellcheck disable=SC2086
$CC $CFLAGS -DIOS_VK_WINE_HEADERS -I"$LAYER" -I"$WINE/include" -c "$LAYER/ios_vk_platform.c" -o "$OUT/platform.o"
# shellcheck disable=SC2086
$CC $CFLAGS -DIOS_VK_WINE_HEADERS -I"$LAYER" -I"$WINE/include" -c "$LAYER/ios_vk_pool.c" -o "$OUT/pool.o"

# ---------------------------------------------------------------- 3-5
for t in test-winevulkan.sh test-moltenvk-checks.sh test-build-scripts.sh windows/syntax-check-vktest.sh; do
    if [ -f "$HERE/$t" ]; then
        echo "== $t"
        env CC="$CC" WINE_SRC="$WINE" TMPDIR="$OUT" sh "$HERE/$t"
    fi
done

# the layer against Khronos' headers at the MoltenVK fork's pin (the CI's vktest job has them)
if [ -n "${VULKAN_HEADERS:-}" ]; then
    echo "== check-layer-khronos.sh"
    env CC="$CC" TMPDIR="$OUT" sh "$HERE/check-layer-khronos.sh" "$VULKAN_HEADERS"
else
    echo "SKIPPED: check-layer-khronos.sh (set VULKAN_HEADERS to a Vulkan-Headers include dir; the CI's vktest job runs it)"
fi

echo "== check-app-store.sh"
sh "$REPO/check-app-store.sh" "$PART"
