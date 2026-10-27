#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 follow-up A14: vkd3d-shader's HLSL compiler as the farms build it (Wine 11.4's copy of
# vkd3d 1.19 with this part's overlay pe/libs/vkd3d/ on top), built natively, compiles the shaders
# of tests/vkd3d-hlsl/ to what Microsoft's compiler makes of them. It also builds Wine's copy
# without the overlay and expects the ANGLE shader to fail there, so the test keeps proving that
# it exercises the bug.
#   test-vkd3d-hlsl.sh [build-dir]
# VKD3D_RELEASE is the unpacked vkd3d 1.19 release (dl.winehq.org/vkd3d/source/vkd3d-1.19.tar.xz,
# SHA-256 034613605baab8ba84674f8d272cf22b5e86bc6bc03fc5728ef9bce07308baa6): its include/ holds
# the headers widl makes from vkd3d's IDL, under the upstream names Wine's copy renames. Without
# it the test is skipped. WINE_SRC is the Wine 11.4 tree (default refs/wine-11.4). CC (default
# cc) builds it; it needs bison and flex.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
repo=$(cd "$top/../.." && pwd)
wine=${WINE_SRC:-"$repo/refs/wine-11.4"}
release=${VKD3D_RELEASE:-}
cc=${CC:-cc}
build=${1:-"$here/build/vkd3d-hlsl"}
cases="$here/vkd3d-hlsl"

if [ -z "$release" ] || [ ! -f "$release/include/vkd3d_d3dcommon.h" ] || [ ! -d "$wine/libs/vkd3d/libs/vkd3d-shader" ]; then
    echo "SKIPPED: vkd3d HLSL test (needs VKD3D_RELEASE=<unpacked vkd3d-1.19> and WINE_SRC=$wine)"
    exit 0
fi
for tool in "$cc" bison flex; do
    command -v "$tool" >/dev/null 2>&1 || { echo "SKIPPED: vkd3d HLSL test ($tool not installed)"; exit 0; }
done

status=0
fail() {
    echo "FAILED: $*" >&2
    status=1
}

# build <name> <with overlay: 0|1>: $build/<name>/hlslc
build_compiler() {
    out="$build/$1"
    rm -rf "$out"
    mkdir -p "$out/src" "$out/gen" "$out/obj"
    cp -R "$wine/libs/vkd3d/libs" "$out/src/"
    rm -rf "$out/src/libs/vkd3d" "$out/src/libs/vkd3d-utils" "$out/src/libs/vkd3d-common/blob.c"
    if [ "$2" = 1 ]; then
        cp -R "$top/pe/libs/vkd3d/libs" "$out/src/"
    fi
    # Wine's copy names a few headers after Wine's own; map them back to vkd3d's
    find "$out/src" -type f \( -name '*.[chly]' \) -exec sed -i \
        -e 's|#include "wine/rbtree.h"|#include "rbtree.h"|' \
        -e 's|#include "wine/list.h"|#include "list.h"|' \
        -e 's|#include "d3dx9shader.h"|#include "vkd3d_d3dx9shader.h"|' \
        -e 's|#include "d3dcommon.h"|#include "vkd3d_d3dcommon.h"|' {} +
    cat > "$out/gen/config.h" <<'EOF'
#define PACKAGE_NAME "vkd3d"
#define PACKAGE_STRING "vkd3d 1.19"
#define PACKAGE_VERSION "1.19"
#define HAVE_PTHREAD_H 1
#define HAVE_BUILTIN_POPCOUNT 1
#define HAVE_BUILTIN_CLZ 1
#define HAVE_BUILTIN_CTZ 1
#define HAVE_BUILTIN_ADD_OVERFLOW 1
#define HAVE_SYNC_ADD_AND_FETCH 1
#define HAVE_SYNC_BOOL_COMPARE_AND_SWAP 1
#define HAVE_ATOMIC_EXCHANGE_N 1
EOF
    s="$out/src/libs/vkd3d-shader"
    (cd "$out/gen" &&
        bison -d -o hlsl.tab.c "$s/hlsl.y" 2>/dev/null &&
        bison -d -o preproc.tab.c "$s/preproc.y" 2>/dev/null &&
        flex -o hlsl.yy.c "$s/hlsl.l" &&
        flex -o preproc.yy.c "$s/preproc.l") || { fail "$1: bison/flex"; return 1; }
    # vkd3d's own sources build with its own warnings; only the driver gets -Werror
    flags="-O1 -w -D_GNU_SOURCE -DHAVE_CONFIG_H -DLIBVKD3D_SHADER_SOURCE -DVKD3D_NO_TRACE_MESSAGES
        -I$out/gen -I$release/include -I$release/include/private -I$wine/libs/vkd3d/include
        -I$wine/libs/vkd3d/include/private -I$s"
    objs=
    for c in "$out"/src/libs/vkd3d-common/*.c "$s"/*.c "$out"/gen/*.c; do
        o="$out/obj/$(basename "$c" .c).o"
        # shellcheck disable=SC2086
        $cc $flags -c "$c" -o "$o" || { fail "$1: compile $(basename "$c")"; return 1; }
        objs="$objs $o"
    done
    # shellcheck disable=SC2086
    $cc -std=c11 -Wall -Wextra -Werror -I"$release/include" -o "$out/hlslc" "$cases/hlslc.c" $objs -lm -lpthread ||
        { fail "$1: link hlslc"; return 1; }
}

# expect <compiler> <file> ok|fail <pattern that must be there> [<pattern that must not be there>]
# ok: the disassembly is matched; fail: the messages are.
expect() {
    log="$build/$(basename "$2" .hlsl).$(basename "$(dirname "$1")")"
    if "$1" "$cases/$2" > "$log.asm" 2> "$log.err"; then result=ok; text="$log.asm"; else result=fail; text="$log.err"; fi
    if [ "$result" != "$3" ]; then
        fail "$2: expected $3, got $result: $(head -3 "$log.err" | tr '\n' ' ')"
        return
    fi
    grep -Eq -- "$4" "$text" || fail "$2: no line matches /$4/ ($text)"
    if [ -n "${5:-}" ] && grep -Eq -- "$5" "$text"; then
        fail "$2: a line matches /$5/ ($text)"
    fi
}

mkdir -p "$build"
build_compiler fixed 1 || exit 1
build_compiler upstream-1.19 0 || exit 1
fixed="$build/fixed/hlslc"

# The ANGLE pattern: every index folded, the right register for every read, no other register.
expect "$fixed" angle-array-textures.hlsl ok '^ *sample_b r[0-9]+\.[xyzw]+, [^,]+, t1\.xyzw, s1,' 't[4-9]\.|s[4-9][, ]'
for reg in 0 2; do
    expect "$fixed" angle-array-textures.hlsl ok "^ *sample_b r[0-9]+\\.[xyzw]+, [^,]+, t$reg\\.xyzw, s$reg,"
done
expect "$fixed" angle-array-textures.hlsl ok '^ *sample_c r[0-9]+\.[xyzw]+, [^,]+, t3\.xyzw, s3,'
if [ "$(grep -Ec '^ *sample_b .* t1\.xyzw, s1,' "$build/angle-array-textures.fixed.asm")" -ne 4 ]; then
    fail "angle-array-textures.hlsl: expected 4 reads of t1/s1"
fi
expect "$build/upstream-1.19/hlslc" angle-array-textures.hlsl fail \
    'E5022: Loaded resource from "textures2DArray" must be determinable at compile time'

# const locals: initialised where they are declared unless the value is a compile-time constant
expect "$fixed" const-from-parameter.hlsl ok '^mul r[0-9]+\.xy, v0\.xyxx, l\(2\.0+e\+00, 2\.0+e\+00'
expect "$fixed" const-from-local.hlsl ok '^mov r[0-9]+\.x, v0\.x$'
expect "$fixed" const-then-source-changes.hlsl ok '^mov r[0-9]+\.x, l\(3\.0+e\+00\)$'
expect "$fixed" const-then-source-changes.hlsl ok '^mov r[0-9]+\.y, v0\.x$'
expect "$fixed" const-from-changed-static.hlsl ok '^mov r[0-9]+\.xy, v0\.xyxx$' '^mov o0\.xyzw, l\('
expect "$fixed" const-from-changed-static-no-initialiser.hlsl ok '^mov r[0-9]+\.x, v0\.x$' '^mov o0\.xyzw, l\('
expect "$fixed" const-from-static-const.hlsl ok '^mov r[0-9]+\.x, l\(6\.0+e\+00\)$'
expect "$fixed" const-from-static-const-of-uniform.hlsl ok '^mul r[0-9]+\.x, cb0\[0\]\.x, l\(2\.0+e\+00\)$'
expect "$fixed" array-size-from-static.hlsl fail 'Expected literal expression'

[ $status -eq 0 ] && echo "test-vkd3d-hlsl.sh: all checks passed"
exit $status
