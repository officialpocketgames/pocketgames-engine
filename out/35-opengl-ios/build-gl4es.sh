#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# build-gl4es.sh <work dir> <out dir> <ios|macos>
#
# Spec 35 Part C2: gl4es at the pinned commit plus this tree's changed files (gl4es/, at their
# path in gl4es; Part C3), built with CMake for iOS arm64 or macOS arm64, as one static archive
# with hidden visibility. Runs on macOS with Xcode and CMake.
#
# <out dir> receives libgl4es.a, include/ (gl4esinit.h, gl4es_ios.h) and sources.txt (every
# compiled file with its licence, Part C2.4). The script fails when:
#   - the archive exports anything but the four functions of include/gl4esinit.h and the
#     gl4es_ios_* functions (C2.1), or lacks one of the gl4es_ios_* functions of gl4es_ios.h
#     (the C3 changes);
#   - check-app-store.sh finds a forbidden name in a compiled source (C2.3: gl4es's X11 helpers
#     call system(); NOX11 removes them, and this proves it);
#   - a compiled file carries a licence other than MIT, BSD, zlib or public domain (C2.4).
# GLES comes only through ANGLE's eglGetProcAddress, handed to set_getprocaddress (C2.2): no
# dlopen or dlsym (the import check of ci/link-check.sh covers the archive too).

set -euo pipefail

GL4ES_URL=https://github.com/ptitSeb/gl4es
GL4ES_COMMIT=bc4314d8369f169d3fa81c5ed1b46b3dba4e74a5   # master, 2026-10-30 (after v1.1.6)

[ $# -eq 3 ] || { echo "usage: $0 <work dir> <out dir> <ios|macos>" >&2; exit 2; }
target=$3
case "$target" in ios|macos) ;; *) echo "$0: target must be ios or macos" >&2; exit 2 ;; esac

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)

refs=$(cd "$repo/refs" 2>/dev/null && pwd -P) || refs=""
for d in "$1" "$2"; do
    [ -n "$refs" ] || break
    p=$d; rest=""
    while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
    case "$(cd "$p" && pwd -P)$rest/" in
        "$refs/"*) echo "$0: refusing a directory inside refs/: $d" >&2; exit 1 ;;
    esac
done

mkdir -p "$1" "$2"
work=$(cd "$1" && pwd)
out=$(cd "$2" && pwd)

# ---------------------------------------------------------------- source: pinned commit + changes
src="$work/gl4es-$target"
rm -rf "$src"
mkdir -p "$src"
git -C "$src" init -q
git -C "$src" remote add origin "$GL4ES_URL"
git -C "$src" fetch -q --depth 1 origin "$GL4ES_COMMIT"
git -C "$src" checkout -q FETCH_HEAD
[ "$(git -C "$src" rev-parse HEAD)" = "$GL4ES_COMMIT" ] || { echo "build-gl4es: not at $GL4ES_COMMIT" >&2; exit 1; }
echo "build-gl4es: gl4es $GL4ES_COMMIT"
(cd "$here/gl4es" && find . -type f) | while read -r f; do
    mkdir -p "$src/$(dirname "$f")"
    cp "$here/gl4es/$f" "$src/$f"
    echo "build-gl4es: changed $f"
done

# ---------------------------------------------------------------- build
if [ "$target" = ios ]; then
    sys="-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0"
else
    sys="-DCMAKE_OSX_SYSROOT=macosx -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0"
fi
bld="$work/build-$target"
rm -rf "$bld"
# GL4ES_IOS selects the C3 changes; gl4es_ios_config.h, forced into every source, sends gl4es's
# printf/fprintf(stderr) to the log callback (C3.3) and defines the per-thread marker (C3.1)
cflags="-fvisibility=hidden -DNDEBUG -DGL4ES_IOS -include $src/src/gl/gl4es_ios_config.h"
# C2's options but NO_LOADER: on Apple targets src/gl/loader.h defines NO_LOADER itself, and
# CMake's NO_LOADER=ON (add_definitions(-DNO_LOADER), i.e. 1) only redefined it in every file.
# Both files that test it (loader.c, init.c) include loader.h first, so the loader stays off.
# shellcheck disable=SC2086
cmake -S "$src" -B "$bld" $sys -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_C_FLAGS="$cflags" -DCMAKE_C_VISIBILITY_PRESET=hidden \
    -DNOX11=ON -DNOEGL=ON -DNO_GBM=ON -DSTATICLIB=ON -DNO_INIT_CONSTRUCTOR=ON -DDEFAULT_ES=2
cmake --build "$bld" -j "$(sysctl -n hw.ncpu)"

# gl4es's CMakeLists.txt sets CMAKE_ARCHIVE_OUTPUT_DIRECTORY to the source tree's lib/ (in the
# file, so no -D moves it): the static GL target is <src>/lib/libGL.a
lib="$src/lib/libGL.a"
if [ ! -f "$lib" ]; then
    lib=$({ find "$src/lib" "$bld" -name '*.a' 2>/dev/null || true; } | sed -n '1p')
fi
[ -n "$lib" ] && [ -f "$lib" ] || {
    echo "build-gl4es: no static library built (not at $src/lib/libGL.a, none in $src/lib or $bld)" >&2; exit 1; }
echo "build-gl4es: gl4es's archive: $lib ($(ar t "$lib" | wc -l | tr -d ' ') members)"

# C2.1: in a static link, hidden symbols are still global and gl4es's gl* aliases would clash
# with ANGLE's. So the archive's objects are pre-linked into one object (ld -r) that keeps only
# the allowed functions global: every other symbol becomes local to it.
sdk=iphoneos
[ "$target" = macos ] && sdk=macosx
# the pre-linked object's minimum OS, as CMake compiled the objects (without it ld -r marks it
# for the SDK's own version, and a link for 13.0 warned that libgl4es.a was built for 26.0)
minos="-miphoneos-version-min=17.0"
[ "$target" = macos ] && minos="-mmacosx-version-min=13.0"
pre="$work/prelink-$target"
rm -rf "$pre"
mkdir -p "$pre/objs"
(cd "$pre/objs" && ar x "$lib")
# the four of gl4esinit.h and exactly the functions gl4es_ios.h declares (gl4es_ios.c's printf
# redirection and its data stay local)
ios_funcs=$(grep -oE 'gl4es_ios_[A-Za-z0-9_]+ *\(' "$here/gl4es/include/gl4es_ios.h" | tr -d ' (' | sort -u)
{ printf '%s\n' _initialize_gl4es _set_getprocaddress _set_getmainfbsize _gl4es_GetProcAddress
  for f in $ios_funcs; do echo "_$f"; done; } > "$pre/exports.txt"
# every one of them must be defined in the archive and not hidden (ld -r stops on the first
# hidden one; this names them all at once): "(__TEXT,__text) external _f", not "private external"
nm -m "$lib" 2>/dev/null > "$pre/archive-symbols.txt" || true
bad=0
while read -r s; do
    if grep -qE "\(__[A-Z]+,[^)]*\) external $s\$" "$pre/archive-symbols.txt"; then
        :
    elif grep -qE "\(__[A-Z]+,[^)]*\) private external $s\$" "$pre/archive-symbols.txt"; then
        echo "build-gl4es: FAIL: $s is hidden; the pre-link can't export it (C2.1)"; bad=1
    else
        echo "build-gl4es: FAIL: $s is not defined in gl4es's archive (C2.1)"; bad=1
    fi
done < "$pre/exports.txt"
[ $bad -eq 0 ] || { echo "build-gl4es: the export list ($(wc -l < "$pre/exports.txt" | tr -d ' ') names) can't be exported" >&2; exit 1; }
echo "build-gl4es: all $(wc -l < "$pre/exports.txt" | tr -d ' ') names of the export list are defined and visible"
# shellcheck disable=SC2086
xcrun -sdk "$sdk" clang -arch arm64 $minos -r -nostdlib -Wl,-exported_symbols_list,"$pre/exports.txt" \
    -o "$pre/gl4es.o" "$pre"/objs/*.o
rm -f "$out/libgl4es.a"
xcrun -sdk "$sdk" ar rcs "$out/libgl4es.a" "$pre/gl4es.o"
rm -rf "$out/include"
mkdir -p "$out/include"
cp "$src/include/gl4esinit.h" "$out/include/"
[ -f "$src/include/gl4es_ios.h" ] && cp "$src/include/gl4es_ios.h" "$out/include/"

failed=0

# ---------------------------------------------------------------- C2.1: exports
nm -gUm "$out/libgl4es.a" 2>/dev/null | grep ' external ' | grep -v 'private external' | awk '{print $NF}' | sort -u > "$work/exports-$target.txt"
if grep -vxF -f "$pre/exports.txt" "$work/exports-$target.txt" > "$work/extra-exports-$target.txt"; then
    echo "build-gl4es: FAIL: exported beyond C2.1 (first 40):"
    head -n 40 "$work/extra-exports-$target.txt"
    failed=1
fi
for f in $ios_funcs; do
    grep -qx "_$f" "$work/exports-$target.txt" || { echo "build-gl4es: FAIL: $f is missing (the C3 changes)"; failed=1; }
done

# ---------------------------------------------------------------- C3.6: no exit
# no path may end the process: the pre-linked object must not import exit, abort or assert's
# handler (assert compiles out with NDEBUG; this proves it for every compiled file)
nm -um "$pre/gl4es.o" 2>/dev/null | awk '{print $NF}' | sort -u > "$work/imports-$target.txt"
if grep -xE '_(exit|_exit|_Exit|abort|quick_exit|__assert_rtn|__assert_fail|err|errx|verr|verrx)' "$work/imports-$target.txt" > "$work/exit-imports-$target.txt"; then
    echo "build-gl4es: FAIL: gl4es can end the process (C3.6):"
    cat "$work/exit-imports-$target.txt"
    failed=1
fi
# C3.3: nothing printed: every printf/fprintf went to gl4es_ios.c's collector
if grep -xE '_(printf|puts|putchar|vprintf|perror)' "$work/imports-$target.txt" > "$work/print-imports-$target.txt"; then
    echo "build-gl4es: FAIL: gl4es still prints to stdout (C3.3):"
    cat "$work/print-imports-$target.txt"
    failed=1
fi

# ---------------------------------------------------------------- C2.3, C2.4: compiled sources
chk="$work/check-$target"
rm -rf "$chk"
mkdir -p "$chk"
python3 -I -c '
import json, sys
for e in json.load(open(sys.argv[1])):
    print(e["file"])
' "$bld/compile_commands.json" | sort -u > "$work/compiled-$target.txt"
: > "$out/sources.txt"
while read -r f; do
    rel=${f#"$src"/}
    mkdir -p "$chk/$(dirname "$rel")"
    cp "$f" "$chk/$rel"
    head=$(head -n 40 "$f")
    case "$head" in
        *"GNU General Public"*|*"GPL"*) lic="GPL"; ;;
        *"MIT"*|*"Permission is hereby granted, free of charge"*) lic="MIT" ;;
        *"Redistribution and use in source and binary forms"*) lic="BSD" ;;
        *"zlib"*|*"This software is provided 'as-is'"*) lic="zlib" ;;
        *"public domain"*|*"Public Domain"*) lic="public domain" ;;
        *) lic="MIT (gl4es's LICENSE; no header)" ;;
    esac
    echo "$rel: $lic" >> "$out/sources.txt"
    case "$lic" in GPL) echo "build-gl4es: FAIL: $rel is under the GPL"; failed=1 ;; esac
done < "$work/compiled-$target.txt"
echo "build-gl4es: $(wc -l < "$work/compiled-$target.txt" | tr -d ' ') compiled files (sources.txt)"
if ! sh "$repo/check-app-store.sh" "$chk"; then
    echo "build-gl4es: FAIL: check-app-store.sh on gl4es's compiled sources"
    failed=1
fi
cp "$src/LICENSE" "$out/LICENSE.gl4es" 2>/dev/null || true

echo "build-gl4es: libgl4es.a $(ls -l "$out/libgl4es.a" | awk '{print $5}') bytes"
exit $failed
