#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 follow-up A14: vkd3d-shader's HLSL compiler as the farms build it (Wine 11.4's copy of
# vkd3d 1.19 with this part's overlay pe/libs/vkd3d/ on top), built natively, compiles the shaders
# of tests/vkd3d-hlsl/ to what Microsoft's compiler makes of them. It also builds Wine's copy
# without the overlay and expects the ANGLE shader to fail there, so the test keeps proving that
# it exercises the bug. A15 (half for Shader Model 4 and later), A16 (SV_ClipDistance and
# SV_CullDistance), A17 (legacy texture intrinsics, constant offsets and sizes, a field named
# pixel), A19 (Shader Model 1-3 constant tables) and A20 (shifts in #if, ?: on structs) add
# their checks below.
#   test-vkd3d-hlsl.sh [build-dir]
# VKD3D_RELEASE is the unpacked vkd3d 1.19 release (dl.winehq.org/vkd3d/source/vkd3d-1.19.tar.xz,
# SHA-256 034613605baab8ba84674f8d272cf22b5e86bc6bc03fc5728ef9bce07308baa6): its include/ holds
# the headers widl makes from vkd3d's IDL, under the upstream names Wine's copy renames. Without
# it (A17) Wine's own headers are used instead: d3dcommon.h as the configured Wine build WINE_BUILD
# (default ~/wine-build; ~/.cache/myiosdeck/wine-11.4-headers works) made it from Wine's IDL, and
# Wine's include/. With neither the test is skipped. WINE_SRC is the Wine 11.4 tree (default
# refs/wine-11.4). CC (default cc) builds it; it needs bison and flex.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
repo=$(cd "$top/../.." && pwd)
wine=${WINE_SRC:-"$repo/refs/wine-11.4"}
release=${VKD3D_RELEASE:-}
winebuild=${WINE_BUILD:-"$HOME/wine-build"}
cc=${CC:-cc}
build=${1:-"$here/build/vkd3d-hlsl"}
cases="$here/vkd3d-hlsl"

if [ ! -d "$wine/libs/vkd3d/libs/vkd3d-shader" ]; then
    echo "SKIPPED: vkd3d HLSL test (needs WINE_SRC=$wine)"
    exit 0
fi
if [ -n "$release" ] && [ -f "$release/include/vkd3d_d3dcommon.h" ]; then
    headers=release
    driver_inc="$release/include"
elif [ -f "$winebuild/include/d3dcommon.h" ]; then
    headers=wine
    driver_inc="$wine/libs/vkd3d/include"
else
    echo "SKIPPED: vkd3d HLSL test (needs VKD3D_RELEASE=<unpacked vkd3d-1.19> or a configured WINE_BUILD=$winebuild)"
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
    # Wine's copy names a few headers after Wine's own; with the release's headers, map them back
    # to vkd3d's
    [ "$headers" = wine ] || find "$out/src" -type f \( -name '*.[chly]' \) -exec sed -i \
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
    if [ "$headers" = wine ]; then
        hdrs="-DWINE_NO_NAMELESS_EXTENSION -I$wine/libs/vkd3d/include -I$wine/libs/vkd3d/include/private
            -I$s -I$winebuild/include -I$wine/include"
    else
        hdrs="-I$release/include -I$release/include/private -I$wine/libs/vkd3d/include
            -I$wine/libs/vkd3d/include/private -I$s"
    fi
    flags="-O1 -w -D_GNU_SOURCE -DHAVE_CONFIG_H -DLIBVKD3D_SHADER_SOURCE -DVKD3D_NO_TRACE_MESSAGES
        -I$out/gen $hdrs"
    objs=
    for c in "$out"/src/libs/vkd3d-common/*.c "$s"/*.c "$out"/gen/*.c; do
        o="$out/obj/$(basename "$c" .c).o"
        # shellcheck disable=SC2086
        $cc $flags -c "$c" -o "$o" || { fail "$1: compile $(basename "$c")"; return 1; }
        objs="$objs $o"
    done
    # shellcheck disable=SC2086
    $cc -std=c11 -Wall -Wextra -Werror -I"$driver_inc" -I"$top/pe/dlls/d3dcompiler_43" -o "$out/hlslc" "$cases/hlslc.c" $objs -lm -lpthread ||
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

# A15: half for profiles 4.0 and later is float. same <profile> <a.hlsl> <b.hlsl> <compiler a> <compiler b>:
# both compile, and the disassemblies are identical
same() {
    la="$build/$(basename "$2" .hlsl).$1.a" lb="$build/$(basename "$3" .hlsl).$1.b"
    "$4" -p "$1" "$cases/$2" > "$la.asm" 2> "$la.err" || { fail "$2 ($1): $(head -3 "$la.err" | tr '\n' ' ')"; return; }
    "$5" -p "$1" "$cases/$3" > "$lb.asm" 2> "$lb.err" || { fail "$3 ($1): $(head -3 "$lb.err" | tr '\n' ' ')"; return; }
    cmp -s "$la.asm" "$lb.asm" || fail "$2 and $3 ($1): different output ($la.asm, $lb.asm)"
}
upstream="$build/upstream-1.19/hlslc"
same ps_4_0 half-ps40.hlsl float-ps40.hlsl "$fixed" "$fixed"
same vs_5_0 half-vs50.hlsl float-vs50.hlsl "$fixed" "$fixed"
# the bug, as the game met it: Wine's unchanged copy can't compile half arithmetic for SM4
if "$upstream" -p ps_4_0 "$cases/half-ps40.hlsl" > /dev/null 2> "$build/half-ps40.upstream.err"; then
    fail "half-ps40.hlsl: Wine's unchanged vkd3d compiled it (the test no longer shows the bug)"
else
    grep -Eq 'E5017: .*SM4 half[0-9]* (multiplication|addition) expression' "$build/half-ps40.upstream.err" ||
        fail "half-ps40.hlsl: unchanged vkd3d failed differently: $(head -3 "$build/half-ps40.upstream.err" | tr '\n' ' ')"
fi
# below 4.0 nothing changes: the same output as Wine's unchanged copy
same ps_2_0 half-ps20.hlsl half-ps20.hlsl "$fixed" "$upstream"

# A16: SV_ClipDistance / SV_CullDistance for profiles 4.0 and later. sig <profile> <file> <pattern>...:
# compiles with the signatures printed, and every pattern matches a line
sig() {
    p=$1 f=$2
    shift 2
    log="$build/$(basename "$f" .hlsl).$p"
    "$fixed" -s -p "$p" "$cases/$f" > "$log.asm" 2> "$log.err" || { fail "$f ($p): $(head -3 "$log.err" | tr '\n' ' ')"; return; }
    for pat in "$@"; do
        grep -Eq -- "$pat" "$log.asm" || fail "$f ($p): no line matches /$pat/ ($log.asm)"
    done
}
# reg <file.profile> <semantic>: the output register a .param line names
reg() {
    sed -n "s/^ *\\.param $2\\.[xyzw]*, o\\([0-9]*\\)\\..*/\\1/p" "$build/$1.asm"
}
sig vs_4_0 clip-vs40.hlsl '^ *\.param SV_ClipDistance\.x, o[0-9]+\.x, float, CLIPDST$' \
    '^ *\.param SV_ClipDistance1\.xy, o[0-9]+\.xy, float, CLIPDST$' '^ *dcl_output_siv o[0-9]+\.x, clip_distance' \
    '^ *dcl_output_siv o[0-9]+\.xy, clip_distance'
c0=$(reg clip-vs40.vs_4_0 SV_ClipDistance) c1=$(reg clip-vs40.vs_4_0 SV_ClipDistance1) t0=$(reg clip-vs40.vs_4_0 TEXCOORD)
if [ -z "$c0" ] || [ -z "$c1" ] || [ -z "$t0" ] || [ "$c0" = "$c1" ] || [ "$t0" = "$c0" ] || [ "$t0" = "$c1" ]; then
    fail "clip-vs40.hlsl: expected SV_ClipDistance0, SV_ClipDistance1 and TEXCOORD0 in three registers (o$c0, o$c1, o$t0)"
fi
sig vs_4_0 clip4-vs40.hlsl '^ *\.param SV_ClipDistance\.xyzw, o[0-9]+\.xyzw, float, CLIPDST$' \
    '^ *\.param SV_ClipDistance1\.xyzw, o[0-9]+\.xyzw, float, CLIPDST$'
sig vs_5_0 cull-vs50.hlsl '^ *\.param SV_CullDistance\.xyz, o[0-9]+\.xyz, float, CULLDST$' \
    '^ *\.param SV_ClipDistance\.x, o[0-9]+\.x, float, CLIPDST$' '^ *dcl_output_siv o[0-9]+\.xyz, cull_distance'
sig ps_4_0 clip-ps40.hlsl '^ *\.param SV_ClipDistance\.x, v[0-9]+(\.[xyzw]+)?, float, CLIPDST$' \
    '^ *dcl_input_ps_siv .*v[0-9]+\.x, clip_distance'
# the bug, as the game met it (A16)
if "$upstream" -p vs_4_0 "$cases/clip-vs40.hlsl" > /dev/null 2> "$build/clip-vs40.upstream.err"; then
    fail "clip-vs40.hlsl: Wine's unchanged vkd3d compiled it (the test no longer shows the bug)"
else
    grep -Eq "E5013: Invalid semantic 'SV_ClipDistance'" "$build/clip-vs40.upstream.err" ||
        fail "clip-vs40.hlsl: unchanged vkd3d failed differently: $(head -3 "$build/clip-vs40.upstream.err" | tr '\n' ' ')"
fi

# A17. code <profile> <hlslc flags> <file> <pattern>...: compiles, and every pattern matches a line
code() {
    p=$1 fl=$2 f=$3
    shift 3
    log="$build/$(basename "$f" .hlsl).$p.code"
    # shellcheck disable=SC2086
    "$fixed" $fl -p "$p" "$cases/$f" > "$log.asm" 2> "$log.err" || { fail "$f ($p): $(head -3 "$log.err" | tr '\n' ' ')"; return; }
    for pat in "$@"; do
        grep -Eq -- "$pat" "$log.asm" || fail "$f ($p): no line matches /$pat/ ($log.asm)"
    done
}
# legacy texture intrinsics with the backwards-compatibility flag (D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY)
code ps_4_0 -b legacy-tex-ps40.hlsl '^dcl_resource_texturecube .* t0$' '^dcl_resource_texture2d .* t1$' \
    '^sample_l r[0-9]+\.xyzw, v0\.xyzx, t0\.xyzw, s0, l\(3\.0+e\+00\)$' \
    '^sample_l r[0-9]+\.xyzw, v1\.xyxx, t1\.xyzw, s1, l\(1\.0+e\+00\)$' \
    '^sample_b r[0-9]+\.xyzw, v1\.xyxx, t1\.xyzw, s1, l\(-5\.0+e-01\)$' \
    '^div r[0-9]+\.xy, v1\.xyxx, v1\.w$' '^sample r[0-9]+\.xyzw, r[0-9]+\.xyxx, t1\.xyzw, s1$'
if "$upstream" -b -p ps_4_0 "$cases/legacy-tex-ps40.hlsl" > /dev/null 2> "$build/legacy-tex-ps40.upstream.err"; then
    fail "legacy-tex-ps40.hlsl: Wine's unchanged vkd3d compiled it (the test no longer shows the bug)"
else
    grep -q 'E5005: Function "texCUBElod" is not defined' "$build/legacy-tex-ps40.upstream.err" ||
        fail "legacy-tex-ps40.hlsl: unchanged vkd3d failed differently: $(head -3 "$build/legacy-tex-ps40.upstream.err" | tr '\n' ' ')"
fi
# offsets, array sizes and unroll counts from static consts, constructors, unary minus, arithmetic
code ps_4_0 '' offset-static-const.hlsl '^sample\(-1,1,0\) ' '^sample\(-1,3,0\) ' '^ld\(-1,1,0\) ' '^ld\(6,-8,0\) '
if [ "$(grep -c '^sample(-1,1,0) ' "$build/offset-static-const.ps_4_0.code.asm")" -ne 2 ]; then
    fail "offset-static-const.hlsl: expected two samples at offset (-1,1)"
fi
code ps_4_0 '' array-size-static-const.hlsl '^dcl_constantBuffer cb0\[7\],' 'cb0\[6\]\.xyzw' \
    '^mul r[0-9]+\.xyzw, r[0-9]+\.xyzw, l\(2\.0+e\+00, '
# a struct field named pixel, written and read
code ps_4_0 '' field-pixel.hlsl '^mul r0\.xyzw, v0\.xyzw, l\(2\.0+e\+00, ' '^mov o0\.xyzw, r0\.xyzw$'

# A19: Shader Model 1-3 constant tables (hlslc -c prints them as d3dcompiler's dump does)
code vs_2_0 -c sm1-constants-vs20.hlsl \
    '^// constant table: 4 constants, version 0xfffe0200, creator "vkd3d-shader [^"]*", flags 0, target "vs_2_0"$' \
    '^//   WorldViewProj: FLOAT4 c0 count 4, MATRIX_COLUMNS FLOAT 4x4$' \
    '^//   LightDir: FLOAT4 c20 count 1, VECTOR FLOAT 1x4$' \
    '^//   Bones: FLOAT4 c21 count 24, MATRIX_COLUMNS FLOAT 4x3 \[8\]$' \
    '^//   Tint: FLOAT4 c4 count 1, VECTOR FLOAT 1x4, default \{1, 0\.5, 0\.25, 1\}$' \
    '^mova a0?\.x, ' 'c\[a0?\.x \+ 21\]'
if grep -q 'Unused' "$build/sm1-constants-vs20.vs_2_0.code.asm"; then
    fail "sm1-constants-vs20.hlsl: an unused uniform is in the constant table"
fi
code ps_2_0 -c sm1-samplers-ps20.hlsl '^// constant table: 5 constants, .* target "ps_2_0"$' \
    '^//   DiffuseMap: SAMPLER s0 count 1, OBJECT SAMPLER2D 1x1$' \
    '^//   NormalMap: SAMPLER s1 count 1, OBJECT SAMPLER2D 1x1$' \
    '^//   SpecMap: SAMPLER s5 count 1, OBJECT SAMPLER2D 1x1$' \
    '^//   ColorMatrix: FLOAT4 c0 count 3, MATRIX_ROWS FLOAT 3x4$' \
    '^//   Threshold: FLOAT4 c3 count 1, SCALAR FLOAT 1x1$' '^cmp ' '^texld r[0-9]+\.xyzw, t0\.xyxx, s5\.xyzw$'
if grep -Eq '^(if|ifc|else|endif)' "$build/sm1-samplers-ps20.ps_2_0.code.asm"; then
    fail "sm1-samplers-ps20.hlsl: flow control in ps_2_0 (it has none)"
fi
# Wine's unchanged copy leaves the target out (offset 0)
"$upstream" -c -p vs_2_0 "$cases/sm1-constants-vs20.hlsl" > "$build/sm1-constants-vs20.upstream.asm" 2>&1 || true
grep -q '^// constant table: 4 constants, .* target "-"$' "$build/sm1-constants-vs20.upstream.asm" ||
    fail "sm1-constants-vs20.hlsl: Wine's unchanged vkd3d wrote a target string (the test no longer shows the change)"
# A20: << and >> in #if/#elif (any #error fails the compile); the code's own shifts unchanged
code ps_4_0 '' pp-shift-ps40.hlsl '^ishl r0\.x, v0\.x, l\(2\)$' '^ushr r0\.y, v0\.y, l\(1\)$' \
    '^ishl r0\.x, r0\.x, l\(1\)$' '^ushr r0\.y, r0\.y, l\(1\)$' 'l\(1\.0+e\+00\)' 'l\(6\.0+e\+00\)'
if "$upstream" -p ps_4_0 "$cases/pp-shift-ps40.hlsl" > /dev/null 2> "$build/pp-shift-ps40.upstream.err" ||
        ! grep -q 'E4000: syntax error' "$build/pp-shift-ps40.upstream.err"; then
    fail "pp-shift-ps40.hlsl: Wine's unchanged vkd3d didn't fail with E4000 (the test no longer shows the bug)"
fi
# A20: ?: on structs selects the whole struct: day (cb0[0-1], c0-c2) when true, night otherwise
code ps_4_0 '' ternary-struct.hlsl '^movc r[0-9]+\.[xyzw], r[0-9]+\.x, cb0\[0\]\.x, cb0\[2\]\.x$' \
    '^movc r[0-9]+\.[xyzw], r[0-9]+\.x, cb0\[1\]\.w, cb0\[3\]\.w$'
if [ "$(grep -c '^movc ' "$build/ternary-struct.ps_4_0.code.asm")" -ne 8 ]; then
    fail "ternary-struct.hlsl: expected 8 movc (one per component) for ps_4_0"
fi
code ps_2_0 -c ternary-struct.hlsl '^//   day: FLOAT4 c0 count 3, STRUCT VOID 1x8, 3 members$' \
    '^//   night: FLOAT4 c3 count 3, ' '^cmp r[0-9]+\.[xyzw], -r[0-9]+\.x, c3\.x, c0\.x$' \
    '^cmp r[0-9]+\.[xyzw], -r[0-9]+\.x, c5\.x, c2\.x$'
if "$upstream" -p ps_4_0 "$cases/ternary-struct.hlsl" > /dev/null 2> "$build/ternary-struct.upstream.err" ||
        ! grep -q 'E5017: .*Copying from unsupported node type' "$build/ternary-struct.upstream.err"; then
    fail "ternary-struct.hlsl: Wine's unchanged vkd3d didn't fail with E5017 (the test no longer shows the bug)"
fi
# vkd3d can't compile this for ps_1_1, which is why d3dcompiler compiles ps_1_x as ps_2_0 with flag 0x1000
if "$fixed" -p ps_1_1 "$cases/sm1-samplers-ps20.hlsl" > /dev/null 2>&1; then
    fail "sm1-samplers-ps20.hlsl: vkd3d compiled it for ps_1_1 (check A19's ps_1_x promotion is still needed)"
fi

[ $status -eq 0 ] && echo "test-vkd3d-hlsl.sh: all checks passed"
exit $status
