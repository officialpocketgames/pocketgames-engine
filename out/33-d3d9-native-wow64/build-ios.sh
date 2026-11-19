#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 A3 and clarification 6: builds the native dispatch and the registration unix call as a
# static library for aarch64-apple-ios (xcrun -sdk iphoneos clang -arch arm64, iOS 17.0, -O2).
# Session N's CI part (ci/ios-part.sh, part 33-d3d9-native-wow64) calls it next to the native
# frontend's build; the app links both, so the relinking kit carries them.
#
#   build-ios.sh <wine-11.4 tree> <wine build dir> <out dir>
#
# The Wine build dir is ci/wine-host-build.sh's (configure + make include/all): it gives Wine's
# generated headers (unknwn.h, objidl.h, ...) that d3d9.h needs. Writes <out>/obj/*.o,
# <out>/libd3d9w_dispatch.a and <out>/exports.txt (the unix-call tables the loader looks up by
# name, spec 09 clarification 6). The library's undefined symbols are the native frontend's seam
# (interfaces/d3d9w_native.h), session N's allocator (ios_wow_window_alloc), Wine's ios_wow_base
# and __wine_dbg_output, and the app's weak Section A (interfaces/ios_wow_native.h).
#
# Prints "name OK" / "name FAILED" per file and, at the end, the errors of every failed file;
# exits non-zero if any failed. Environment: CC (default: xcrun -sdk iphoneos clang), AR
# (default: xcrun -sdk iphoneos ar), EXTRA_CFLAGS.

set -u

[ $# -eq 3 ] || { echo "usage: $0 <wine-11.4 tree> <wine build dir> <out dir>" >&2; exit 2; }
part=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$part/../.." && pwd)
wine=$(cd "$1" && pwd) || exit 2
wbuild=$(cd "$2" && pwd) || exit 2
mkdir -p "$3/obj" || exit 2
out=$(cd "$3" && pwd)

refs=$(cd "$repo/refs" 2>/dev/null && pwd -P || echo /nonexistent)
case "$(cd "$out" && pwd -P)/" in
    "$refs"/*) echo "build-ios: refusing an output folder inside refs/ ($out): refs/ is read-only" >&2; exit 1 ;;
esac
[ -f "$wine/include/d3d9.h" ] || { echo "build-ios: no include/d3d9.h in $wine" >&2; exit 2; }
[ -f "$wbuild/include/unknwn.h" ] || { echo "build-ios: $wbuild has no generated Wine headers (make include/all)" >&2; exit 2; }

CC=${CC:-"xcrun -sdk iphoneos clang"}
AR=${AR:-"xcrun -sdk iphoneos ar"}
CFLAGS="-arch arm64 -miphoneos-version-min=17.0 -O2 -std=gnu11 -fno-strict-aliasing \
 -Wall -Wextra -Wno-unused-parameter \
 -DWINE_UNIX_LIB -D_REENTRANT \
 -I$part/native -I$part/include -I$part/gen -I$repo/interfaces \
 -isystem $wbuild/include -isystem $wine/include ${EXTRA_CFLAGS:-}"

# The generated dispatch must match the table and this d3d9.h (spec 33 F2).
if command -v python3 > /dev/null; then
    python3 -I "$part/tools/gen_d3d9w.py" "$wine/include/d3d9.h" --check || exit 1
fi

srcs="native/d3d9w_dispatch.c native/d3d9w_marshal.c native/d3d9w_special.c native/d3d9w_extras.c
native/d3d9w_unix.c gen/d3d9w_dispatch_gen.c"
# Compiled as checks only (static asserts against this d3d9.h; F4 and J1).
checks="native/d3d9w_layout_check.c gen/d3d9w_header_check.c"

failed=""
objs=""
for src in $srcs $checks; do
    name=$(basename "$src" .c)
    # shellcheck disable=SC2086
    if $CC $CFLAGS -c "$part/$src" -o "$out/obj/$name.o" > "$out/obj/$name.log" 2>&1; then
        echo "$name OK"
        case " $checks " in *" $src "*) ;; *) objs="$objs $out/obj/$name.o" ;; esac
    else
        echo "$name FAILED"
        failed="$failed $name"
    fi
done

if [ -n "$failed" ]; then
    for name in $failed; do
        echo "==== $name"
        cat "$out/obj/$name.log"
    done
    exit 1
fi

rm -f "$out/libd3d9w_dispatch.a"
# shellcheck disable=SC2086
$AR rcs "$out/libd3d9w_dispatch.a" $objs || exit 1
printf '%s\n' _d3d9_unix_call_funcs _d3d9_unix_call_wow64_funcs > "$out/exports.txt"
echo "build-ios: wrote $out/libd3d9w_dispatch.a"
