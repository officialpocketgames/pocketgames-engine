#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
# Diagnostic: compile one Wine source file with llvm-mingw in the two modes Wine's configure can
# pick: the GNU triple (--with-mingw=llvm-mingw) and the MSVC target (plain clang).
#   try-arm64ec-modes.sh <llvm-mingw bin> <wine tree> <configured build dir> <source> [cpu]
# cpu: arm64ec (default), aarch64, i686, x86_64
set -u
tc=$1 wine=$2 build=$3 file=$4 cpu=${5:-arm64ec}
dir=$(dirname "$file")
for mode in gnu msvc; do
    if [ "$mode" = gnu ]; then set -- "$tc/$cpu-w64-mingw32-clang"
    else set -- "$tc/clang" -target "$cpu-windows" -fuse-ld=lld
    fi
    echo "== $cpu $mode: $*"
    out=$(mktemp)
    "$@" -c -o "$out" -I"$wine/$dir" -I"$build/include" -I"$wine/include" -I"$wine/include/msvcrt" \
        -D__WINESRC__ -D_CRTIMP= -D__WINE_PE_BUILD -Wall --no-default-config -fno-builtin -O2 \
        "$wine/$file" 2>&1 | grep -E "error|warning:|generated" | head -6
    [ -s "$out" ] && echo "object: $(wc -c < "$out") bytes" || echo "no object"
    rm -f "$out"
done
