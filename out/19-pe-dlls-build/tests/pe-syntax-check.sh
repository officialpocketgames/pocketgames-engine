#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 Part C: compile every C file of Part A (pe/) with clang for the three farms' targets
# (arm64ec, aarch64, i386), syntax only, against the overlaid headers: spec 17's shared headers
# (out/09-ntdll-processes/include/) in front of the Wine tree's include/. This is no build (that
# is build-pe.sh), but it catches what a reader can miss, in seconds.
#   pe-syntax-check.sh
# CLANG selects clang (default clang). WINE_SRC is the Wine 11.4 tree (default refs/wine-11.4).
# WINE_BUILD is a configured Wine build directory whose include/ holds the headers widl makes
# from include/*.idl (default ~/wine-build); without it the check is skipped.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
repo=$(cd "$top/../.." && pwd)
clang=${CLANG:-clang}
wine=${WINE_SRC:-"$repo/refs/wine-11.4"}
build=${WINE_BUILD:-"$HOME/wine-build"}
shared="$repo/out/09-ntdll-processes/include"

if [ ! -f "$wine/include/windef.h" ] || [ ! -f "$build/include/servprov.h" ]; then
    echo "SKIPPED: PE syntax check (needs WINE_SRC=$wine and a configured WINE_BUILD=$build with generated headers)"
    exit 0
fi
if ! "$clang" --target=aarch64-windows -x c -fsyntax-only /dev/null 2>/dev/null; then
    echo "SKIPPED: PE syntax check ($clang can't target Windows)"
    exit 0
fi
resdir=$("$clang" -print-resource-dir)
widl="$build/tools/widl/widl"
gen=$(mktemp -d)
trap 'rm -rf "$gen"' EXIT

status=0
# the vkd3d overlay's files (A16's tpf.c, hlsl_codegen.c) are checked in the vkd3d section below
files=$(cd "$top/pe" && find . -name '*.c' ! -path './libs/vkd3d/*' | sed 's|^\./||' | sort)
for f in $files; do
    dir=$(dirname "$f")
    mkdir -p "$gen/$dir"
    # the module's own .idl headers (dlls/combase/irpcss.h, ...), made by the host build's widl
    for idl in "$wine/$dir"/*.idl; do
        [ -f "$idl" ] || continue
        h="$gen/$dir/$(basename "$idl" .idl).h"
        [ -f "$h" ] || "$widl" -o "$h" -h -I"$shared" -I"$build/include" -I"$wine/include" "$idl" || status=1
    done
done
for target in arm64ec aarch64 i686; do
    for f in $files; do
        dir=$(dirname "$f")
        # as makedep compiles a PE object with a plain clang: the module's EXTRADEFS, then
        # -D__WINE_PE_BUILD and the Windows target (MSVC environment)
        extradefs=$(sed -n 's/^EXTRADEFS *= *//p' "$wine/$dir/Makefile.in")
        # EXTRAINCL's bundled libraries, as configure sets them without system copies
        if grep -q '^EXTRAINCL.*VKD3D_PE_CFLAGS' "$wine/$dir/Makefile.in"; then
            extradefs="$extradefs -I$wine/libs/vkd3d/include"
        fi
        # shellcheck disable=SC2086
        if "$clang" --target=$target-windows -fsyntax-only -std=gnu11 -Wall -Werror \
            -Wno-pragma-pack -Wno-microsoft-anon-tag -Wno-unknown-pragmas \
            -D__WINESRC__ -D_UCRT -D__WINE_PE_BUILD $extradefs -nostdinc -isystem "$resdir/include" \
            -I"$top/pe/$dir" -I"$gen/$dir" -I"$wine/$dir" -I"$shared" -I"$build/include" \
            -I"$wine/include" -I"$wine/include/msvcrt" "$top/pe/$f"; then
            :
        else
            echo "FAILED: $target $f" >&2
            status=1
        fi
    done
done

# vkd3d-shader with the overlay pe/libs/vkd3d/ (A14-A20): its parsers hlsl.y and preproc.y are made
# with bison, then the parsers and every vkd3d-shader file that includes hlsl.h compile with Wine's
# flags for libs/vkd3d (vkd3d's own warnings stay warnings; errors fail). The lexers need flex and
# are built by test-vkd3d-hlsl.sh.
vk="$top/pe/libs/vkd3d"
if [ -d "$vk" ] && command -v bison >/dev/null 2>&1; then
    t="$gen/vkd3d"
    cp -R "$wine/libs/vkd3d" "$t"
    cp -R "$vk/." "$t/"
    s="$t/libs/vkd3d-shader"
    if (cd "$s" && bison -d -o hlsl.tab.c hlsl.y 2> "$gen/bison.log" &&
            bison -d -o preproc.tab.c preproc.y 2>> "$gen/bison.log"); then
        # plus every file the overlay replaces (tpf.c doesn't include hlsl.h)
        vkfiles="$s/hlsl.tab.c $s/preproc.tab.c $( (grep -l '#include "hlsl.h"' "$s"/*.c; cd "$vk/libs/vkd3d-shader" && ls *.c 2>/dev/null | sed "s|^|$s/|") | sort -u | tr '\n' ' ')"
        for target in arm64ec aarch64 i686; do
            for f in $vkfiles; do
                # shellcheck disable=SC2086
                if ! "$clang" --target=$target-windows -fsyntax-only -std=gnu11 -w \
                    -D__WINESRC__ -D_UCRT -D__WINE_PE_BUILD -DWINE_NO_NAMELESS_EXTENSION \
                    -DLIBVKD3D_SOURCE -DLIBVKD3D_SHADER_SOURCE -DLIBVKD3D_UTILS_SOURCE \
                    -nostdinc -isystem "$resdir/include" -I"$t" -I"$t/include" -I"$t/include/private" \
                    -I"$t/libs/vkd3d" -I"$s" -I"$shared" -I"$build/include" -I"$wine/include" \
                    -I"$wine/include/msvcrt" "$f"; then
                    echo "FAILED: $target vkd3d-shader $(basename "$f")" >&2
                    status=1
                fi
            done
        done
    else
        echo "FAILED: bison hlsl.y or preproc.y: $(head -3 "$gen/bison.log")" >&2
        status=1
    fi
fi

[ $status = 0 ] && echo "PE syntax check: all Part A files and vkd3d-shader (with the overlay) compile for arm64ec, aarch64 and i686"
exit $status
