#!/bin/sh
# SPDX-License-Identifier: MIT
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
files=$(cd "$top/pe" && find . -name '*.c' | sed 's|^\./||' | sort)
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
[ $status = 0 ] && echo "PE syntax check: all Part A files compile for arm64ec, aarch64 and i686"
exit $status
