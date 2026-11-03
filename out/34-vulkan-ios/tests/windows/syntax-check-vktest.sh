#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part M6 on the host: vktest.c, ../common/vkgx.c and ../macos/vk-render-test.c pass
# clang's -fsyntax-only for the x86_64 and i686 Windows targets, with -Wall -Wextra -Werror,
# against Wine 11.4's Windows headers (windows.h, its msvcrt C headers, and wine/vulkan.h standing
# in for the Khronos vulkan/vulkan.h that the CI builds use) and stand-in shader arrays. Nothing is
# linked or run. SKIPPED without clang.
# Environment: WINE_SRC (the Wine 11.4 tree, read only), TMPDIR.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
WINE=${WINE_SRC:-"$REPO/refs/wine-11.4"}
command -v clang >/dev/null 2>&1 || { echo "SKIPPED: syntax-check-vktest.sh (no clang)"; exit 0; }
T=${TMPDIR:-/tmp}/vk34-vktest.$$
mkdir -p "$T/vulkan"
trap 'rm -rf "$T"' EXIT

cat > "$T/vulkan/vulkan.h" <<'EOF'
/* the host check's stand-in: Wine's generated header, the same declarations for Windows
 * (winbase.h for SECURITY_ATTRIBUTES, which vkgx.c and vk-render-test.c don't get otherwise) */
#include <windef.h>
#include <winbase.h>
#include "wine/vulkan.h"
EOF
{
    echo "#include <stdint.h>"
    for n in cube_vert cube_frag solid_vert solid_frag tess_tesc tess_tese; do
        echo "static const uint32_t $n[] = { 0x07230203u };"
    done
} > "$T/vktest_shaders.h"
{
    echo "#include <stdint.h>"
    for n in gs_quad_vert gs_quad_geom gs_color_frag xfb_vert xfb_geom; do
        echo "static const uint32_t $n[] = { 0x07230203u };"
    done
} > "$T/vkgx_shaders.h"
COMMON="$HERE/../common"
status=0
for target in x86_64-w64-mingw32 i686-w64-mingw32; do
    # the macOS render test is plain C on Vulkan: the same headers check it
    for c in "$HERE/vktest.c" "$COMMON/vkgx.c" "$HERE/../macos/vk-render-test.c"; do
        if clang -target "$target" -std=gnu11 -fsyntax-only -Wall -Wextra -Werror -nostdlibinc \
                -isystem "$WINE/include/msvcrt" -isystem "$WINE/include" -I"$T" -I"$COMMON" "$c"; then
            echo "$(basename "$c") ($target) OK"
        else
            echo "$(basename "$c") ($target) FAILED"; status=1
        fi
    done
done
exit $status
