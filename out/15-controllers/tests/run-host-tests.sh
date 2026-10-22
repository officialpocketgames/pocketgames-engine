#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 15 Part F: builds and runs the host unit tests with any C11 compiler.
#   run-host-tests.sh [build-dir]
# CC selects the compiler (default cc). WINE_SRC points at an upstream Wine 11.4 tree
# (default: refs/wine-11.4 of this repository); the tests that compile Wine's own code
# (hidparse, dinput's axis scaling) need it and are skipped with a message when it is absent.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
repo=$(cd "$top/../.." && pwd)
build=${1:-"$here/build"}
cc=${CC:-cc}
wine=${WINE_SRC:-"$repo/refs/wine-11.4"}
flags="-std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -I$top/hidpad -I$top/pe/dlls/sechost -I$repo/interfaces -I$here"

mkdir -p "$build"
status=0

run() {
    name=$1
    shift
    if "$@"; then :; else
        echo "BUILD FAILED: $name" >&2
        status=1
        return 0
    fi
    "$build/$name" || status=1
}

run test_hidpad $cc $flags -o "$build/test_hidpad" "$here/test_hidpad.c" "$top/hidpad/ios_hidpad.c"
run test_regplan $cc $flags -I"$top/ntdll" -o "$build/test_regplan" "$here/test_regplan.c" \
    "$top/ntdll/ios_hidpad_regplan.c" "$top/hidpad/ios_hidpad.c"
run test_dinput_ios $cc $flags -I"$top/pe/dlls/dinput" -o "$build/test_dinput_ios" "$here/test_dinput_ios.c" -lm

if [ -f "$wine/dlls/hidparse.sys/main.c" ] && [ -f "$wine/include/wine/hid.h" ]; then
    # Wine's headers, compiled by a native compiler as Wine's own unix-side code is
    wflags="$flags -std=gnu11 -I$top/server -I$wine/include -D__WINESRC__ -DWINE_UNIX_LIB -Wno-sign-compare -Wno-unused-but-set-variable -Wno-unused-variable"
    $cc $flags -c -o "$build/ios_hidpad.o" "$top/hidpad/ios_hidpad.c" || status=1
    $cc $flags -I"$top/server" -c -o "$build/ios_hidpad_request.o" "$top/hidpad/ios_hidpad_request.c" || status=1
    $cc $wflags -DIOS_HIDPARSE_MAIN_C="\"$wine/dlls/hidparse.sys/main.c\"" -c -o "$build/ios_hidparse.o" "$top/server/ios_hidparse.c" ||
        { echo "BUILD FAILED: ios_hidparse.o" >&2; status=1; }
    run test_hidparse $cc $wflags -o "$build/test_hidparse" "$here/test_hidparse.c" "$build/ios_hidparse.o" \
        "$build/ios_hidpad_request.o" "$build/ios_hidpad.o"
    run test_keystroke $cc $wflags -I"$top/pe/dlls/xinput1_3" -o "$build/test_keystroke" "$here/test_keystroke.c"
    run test_sdl_sequence $cc $wflags -I"$top/ntdll" -o "$build/test_sdl_sequence" "$here/test_sdl_sequence.c" \
        "$build/ios_hidparse.o" "$build/ios_hidpad_request.o" "$build/ios_hidpad.o" "$top/ntdll/ios_hidpad_regplan.c"
    run test_xinput_sdl $cc $wflags -I"$top/pe/dlls/xinput1_3" -o "$build/test_xinput_sdl" "$here/test_xinput_sdl.c"
    run test_pnp $cc $wflags -o "$build/test_pnp" "$here/test_pnp.c" "$top/hidpad/ios_hidpad_pnp.c" "$build/ios_hidpad.o"
else
    echo "SKIPPED: test_hidparse, test_keystroke, test_sdl_sequence, test_xinput_sdl, test_pnp (no Wine 11.4 tree at $wine; set WINE_SRC)"
fi

# spec 00 F: the App Store check on this part's sources
if [ -f "$repo/check-app-store.sh" ]; then
    sh "$repo/check-app-store.sh" "$top" || status=1
fi

exit $status
