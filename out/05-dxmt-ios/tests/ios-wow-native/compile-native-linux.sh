#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 Part B: compiles the ios_wow_native build on Linux, so the iOS CI build is not the
# first compile. Every source of src/ios_wow_native/sources.txt with the flags of
# src/ios_wow_native/meson.build, Part C's allocator, and the B4 queries against Wine 11.4's
# headers; then links them with fakes of what the app links next to them (the unix side's call
# table, Wine's win32u and memory calls, ios_wow_base, the DXSO translator) into
# test_native_seam, so any other function the build still calls (a Win32 window, GDI or file
# mapping call that slipped through) fails the link. The test then runs as the seam's fake
# caller.
#
# Stand-ins for what only Apple's tools make: version.h (vcs_tag) and dxmt_command.h (the
# command shaders' metallib) are generated here; nothing Apple-only is included on Linux.
#
# Usage: compile-native-linux.sh [work dir]
# CC / CXX pick the compilers (default cc / c++); JOBS the parallel compiles (default 4);
# WINE_SRC a Wine 11.4 tree (default: the repository's refs/wine-11.4).
# The frontend is compiled with its build's warnings (meson warning_level 2) but not -Werror,
# as upstream DXMT and the imported frontend are; the test and fakes use -Werror.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "${DXMT_TREE:-$here/../../dxmt}" && pwd)
src="$tree/src"
repo=$(cd "$tree/../../.." && pwd)
work=${1:-"$here/build"}
wine=${WINE_SRC:-"$repo/refs/wine-11.4"}
cc=${CC:-cc}
cxx=${CXX:-c++}
jobs=${JOBS:-4}
alloc="$repo/out/33-d3d9-native-wow64/alloc"

[ -f "$wine/include/ntuser.h" ] || { echo "compile-native-linux: no Wine 11.4 tree at $wine (set WINE_SRC)" >&2; exit 1; }
mkdir -p "$work/gen" "$work/obj"
work=$(cd "$work" && pwd)

# stand-ins for vcs_tag and the metallib header
printf '#pragma once\n#define DXMT_VERSION "linux-compile-check"\n' > "$work/gen/version.h"
printf '/* stand-in: the iOS build embeds the compiled command shaders */\nstatic const unsigned char dxmt_command[] = {0};\nstatic const unsigned int dxmt_command_len = 1;\n' > "$work/gen/dxmt_command.h"

defines="-DDXMT_NATIVE=1 -DDXMT_IOS=1 -DDXMT_IOS_WOW_NATIVE=1 -DDXMT_PAGE_SIZE=4096 -DNOMINMAX -D_WIN32_WINNT=0xa00"
warnings="-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unused-const-variable -Wno-missing-braces"
case "$($cxx --version 2>/dev/null)" in
    *clang*) warnings="$warnings -fblocks -Wno-unused-private-field -Wno-extern-c-compat -Wno-microsoft-exception-spec" ;;
    *) warnings="$warnings -Wno-cast-function-type -Wno-class-memaccess" ;;
esac
incs="-I$work/gen -I$src/ios_wow_native -I$tree/include -I$tree/include/native/windows -I$tree/include/native/directx \
 -I$tree/libs -I$src/util -I$src/dxmt -I$src/d3d9 -I$src/airconv -I$src/winemetal -I$repo/interfaces"

# one compile per line: <compiler and flags>|<source>|<object>
list="$work/compile.list"
: > "$list"
grep -v '^[[:space:]]*#' "$src/ios_wow_native/sources.txt" | tr -d '\r' | while read -r f; do
    [ -n "$f" ] || continue
    obj="$work/obj/$(echo "$f" | tr '/' '_').o"
    case "$f" in
        *.c) echo "$cc -std=gnu11 -O0 -g0 -c $defines $warnings $incs|$src/$f|$obj" ;;
        *) echo "$cxx -std=c++20 -O0 -g0 -c $defines $warnings $incs -include $src/util/util_ios_wow_native.h -include $here/linux-standins.h|$src/$f|$obj" ;;
    esac
done >> "$list"
echo "$cc -std=gnu11 -O0 -c -Wall -Wextra -Werror -D__WINESRC__ -DWINE_UNIX_LIB -isystem $wine/include|$src/util/util_ios_wow_native_user.c|$work/obj/util_ios_wow_native_user.o" >> "$list"
echo "$cc -std=c11 -O0 -c -Wall -Wextra -Werror|$alloc/ios_wow_window_alloc.c|$work/obj/ios_wow_window_alloc.o" >> "$list"

echo "compile-native-linux: $(wc -l < "$list") files with $cc / $cxx, $jobs at a time"
failed="$work/failed.list"
: > "$failed"
# shellcheck disable=SC2016
tr '\n' '\0' < "$list" | xargs -0 -n 1 -P "$jobs" sh -c '
    cmd=${1%%|*}; rest=${1#*|}; source=${rest%%|*}; obj=${rest#*|}
    log="$obj.log"
    if $cmd -o "$obj" "$source" > "$log" 2>&1; then
        echo "  $(basename "$source") OK"
    else
        echo "  $(basename "$source") FAILED"
        echo "$source" >> "$0"
    fi' "$failed"

if [ -s "$failed" ]; then
    echo "compile-native-linux: $(wc -l < "$failed") files failed to compile:"
    while read -r f; do
        obj="$work/obj/$(echo "${f#$src/}" | tr '/' '_').o"
        [ -f "$obj.log" ] || obj="$work/obj/$(basename "${f%.*}").o"
        echo "== $f"
        grep -E 'error' "$obj.log" | head -n 40
    done < "$failed"
    exit 1
fi
rm -f "$work/libd3d9_ios_wow_native.a"
ar rcs "$work/libd3d9_ios_wow_native.a" "$work"/obj/*.o
echo "compile-native-linux: $work/libd3d9_ios_wow_native.a"

# the link check and the seam's fake caller
$cc -std=gnu11 -O0 -c -Wall -Wextra -Werror -D__WINESRC__ -DWINE_UNIX_LIB -isystem "$wine/include" \
    -o "$work/native_fakes_wine.o" "$here/native_fakes_wine.c"
$cxx -std=c++20 -O0 -c -Wall -Wextra -Werror $defines -Wno-unused-parameter $incs \
    -include "$src/util/util_ios_wow_native.h" -o "$work/test_native_seam.o" "$here/test_native_seam.cpp"
# --whole-archive: every object of the library is checked, not only those the test reaches
if ! $cxx -o "$work/test_native_seam" "$work/test_native_seam.o" "$work/native_fakes_wine.o" \
        -Wl,--whole-archive "$work/libd3d9_ios_wow_native.a" -Wl,--no-whole-archive -pthread > "$work/link.log" 2>&1; then
    echo "compile-native-linux: the link failed; functions the native build calls that the app does not provide:"
    grep -E 'undefined reference|undefined symbol' "$work/link.log" | sed 's/.*undefined [a-z]* `\{0,1\}//' | sort -u | head -n 80
    exit 1
fi
"$work/test_native_seam"
