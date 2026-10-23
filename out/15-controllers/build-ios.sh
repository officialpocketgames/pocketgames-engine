#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 15: builds the unix-side parts for iOS arm64 as three static libraries.
#
#   build-ios.sh <wine-11.4 tree> <wine build dir with generated headers> <out dir>
#
#   libios_hidpad.a              Part A, the B4/B5 request rules and the device-change events
#                                of clarification 6 (pure C, no Wine headers),
#                                needed by both others
#   libios_controllers_server.a  Part B: the device and Wine's hidparse (wineserver side)
#   libios_controllers_ntdll.a   Part C: the registry entries (ntdll's unix side)
#
# The wineserver (spec 08) and ntdll (spec 09) call ios_hidpad_server_init and
# ios_hidpad_publish through weak references, which never pull a member out of an archive:
# the app must link the last two with -Wl,-force_load,<lib> (QUESTIONS 15-12).
#
# The server objects are compiled with the wineserver's headers as spec 08 builds them:
# upstream server/*.h overlaid with spec 08's modified headers (SPEC08_SERVER, default this
# repository's out/08-wineserver-ios/server), and its ios_config.h when present.
# CC, LIBTOOL and NM default to the iPhoneOS SDK's tools.

set -eu

if [ $# -ne 3 ]; then
    echo "usage: $0 <wine-11.4 tree> <wine build dir> <out dir>" >&2
    exit 2
fi

WINE_SRC=$(cd "$1" && pwd)
WINE_BUILD=$(cd "$2" && pwd)
mkdir -p "$3"
OUT=$(cd "$3" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SPEC08_SERVER=${SPEC08_SERVER:-"$REPO/out/08-wineserver-ios/server"}

# refs/ is read-only: the Wine tree is only read; nothing is written below refs/
case "$OUT/" in
    "$REPO/refs/"*) echo "error: the out dir $OUT is inside refs/ (read-only)" >&2; exit 1 ;;
esac

MIN_IOS=17.0
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=${CC:-$(xcrun --sdk iphoneos -f clang)}
LIBTOOL=${LIBTOOL:-$(xcrun --sdk iphoneos -f libtool)}
NM=${NM:-$(xcrun --sdk iphoneos -f nm)}

[ -f "$WINE_SRC/server/Makefile.in" ] || { echo "error: $WINE_SRC is not a Wine tree" >&2; exit 1; }
[ -f "$WINE_SRC/dlls/hidparse.sys/main.c" ] || { echo "error: $WINE_SRC/dlls/hidparse.sys/main.c not found" >&2; exit 1; }
[ -f "$WINE_BUILD/include/config.h" ] || { echo "error: $WINE_BUILD/include/config.h not found (run a host configure first)" >&2; exit 1; }

OBJ="$OUT/obj"
STAGE="$OUT/stage-server"
rm -rf "$OBJ" "$STAGE"
mkdir -p "$OBJ" "$STAGE"
cp "$WINE_SRC"/server/*.h "$STAGE"/
if [ -d "$SPEC08_SERVER" ]; then cp "$SPEC08_SERVER"/*.h "$STAGE"/ 2>/dev/null || true; fi
# spec 11 Part A2's shared syscall-frame header, staged as spec 08's build stages it: spec 08's
# headers (ios_suspend.h -> ios_thread_layout.h) include it
SHARED_FRAME="$REPO/out/09-ntdll-processes/ntdll-unix/ios_syscall_frame.h"
[ -f "$SHARED_FRAME" ] || { echo "error: $SHARED_FRAME not found (spec 11 Part A2)" >&2; exit 1; }
cp "$SHARED_FRAME" "$STAGE"/

BASE="-arch arm64 -isysroot $SDK -miphoneos-version-min=$MIN_IOS -O2 -std=gnu11 -pipe"
BASE="$BASE -fno-strict-aliasing -fno-common -Wall -Wno-unused-function -Wno-pointer-sign"
# pe/dlls/sechost: ios_pnp.h, the record layout the server and sechost share (clarification 6)
COMMON="-I$HERE/hidpad -I$HERE/pe/dlls/sechost -I$REPO/interfaces"
# spec 17 Part A1: the shared header overrides (wine/unixlib.h, winternl.h) before the Wine
# tree's include/
OVERRIDE_INC="-I$REPO/out/09-ntdll-processes/include"

PURE_CFLAGS="$BASE $COMMON"
SERVER_CFLAGS="$BASE -DWINE_IOS=1 -D__WINESRC__ -D_REENTRANT"
if [ -f "$STAGE/ios_config.h" ]; then SERVER_CFLAGS="$SERVER_CFLAGS -include $STAGE/ios_config.h"; fi
# the same spec 16 settings spec 08 compiles the server with (its ios_config.h reads them)
SYNC_INC="-I$REPO/out/16-sync-ios/include"
SERVER_CFLAGS="$SERVER_CFLAGS -DHAVE_LINUX_NTSYNC_H=1 -DIOS_USYNC=1"
SERVER_CFLAGS="$SERVER_CFLAGS -I$STAGE -I$HERE/server $COMMON $SYNC_INC $OVERRIDE_INC -I$WINE_BUILD/include -I$WINE_SRC/include"
NTDLL_CFLAGS="$BASE -DWINE_IOS=1 -DWINE_UNIX_LIB -D__WINESRC__ -D_NTSYSTEM_ -D_REENTRANT"
NTDLL_CFLAGS="$NTDLL_CFLAGS -I$HERE/ntdll $COMMON $OVERRIDE_INC -I$WINE_BUILD/include -I$WINE_SRC/include"

compile() {   # compile <flags> <source> [extra flags...]
    flags=$1
    src=$2
    shift 2
    obj="$OBJ/$(basename "${src%.c}").o"
    log="$OBJ/$(basename "${src%.c}").log"
    printf '  CC  %s\n' "${src#$HERE/}"
    # shellcheck disable=SC2086
    if ! $CC $flags "$@" -c "$src" -o "$obj" >"$log" 2>&1; then
        echo "error: $src failed to compile:" >&2
        cat "$log" >&2
        exit 1
    fi
    LAST_OBJ=$obj
}

archive() {   # archive <lib> <objects...>
    lib="$OUT/$1"
    shift
    rm -f "$lib"
    "$LIBTOOL" -static -o "$lib" "$@"
    echo "  AR  $(basename "$lib")"
}

need_symbol() {   # need_symbol <lib> <symbol>
    if ! "$NM" -g "$OUT/$1" | grep -q " T _$2\$"; then
        echo "error: $1 does not define $2" >&2
        exit 1
    fi
}

echo "spec 15 controllers for iOS arm64 (minimum iOS $MIN_IOS)"

compile "$PURE_CFLAGS" "$HERE/hidpad/ios_hidpad.c"
pure_objs=$LAST_OBJ
# clarification 8: the XInput slots' HID devices (identities, descriptor, reports)
compile "$PURE_CFLAGS" "$HERE/hidpad/ios_hidpad_xinput.c"
pure_objs="$pure_objs $LAST_OBJ"
# the request rules of B4/B5 (plain C; ios_hidparse.h is a plain header)
compile "$PURE_CFLAGS -I$HERE/server" "$HERE/hidpad/ios_hidpad_request.c"
pure_objs="$pure_objs $LAST_OBJ"
# clarification 6: the device-change events and the channel's queue (plain C)
compile "$PURE_CFLAGS" "$HERE/hidpad/ios_hidpad_pnp.c"
pure_objs="$pure_objs $LAST_OBJ"
# shellcheck disable=SC2086
archive libios_hidpad.a $pure_objs

compile "$SERVER_CFLAGS" "$HERE/server/ios_hidpad_server.c"
server_objs=$LAST_OBJ
compile "$SERVER_CFLAGS" "$HERE/server/ios_hidparse.c" \
    -DIOS_HIDPARSE_MAIN_C="\"$WINE_SRC/dlls/hidparse.sys/main.c\"" -Wno-unused-variable -Wno-unused-but-set-variable
server_objs="$server_objs $LAST_OBJ"
# shellcheck disable=SC2086
archive libios_controllers_server.a $server_objs
need_symbol libios_controllers_server.a ios_hidpad_server_init

ntdll_objs=""
for src in "$HERE"/ntdll/*.c; do
    [ -f "$src" ] || continue
    compile "$NTDLL_CFLAGS" "$src"
    ntdll_objs="$ntdll_objs $LAST_OBJ"
done
# shellcheck disable=SC2086
archive libios_controllers_ntdll.a $ntdll_objs
need_symbol libios_controllers_ntdll.a ios_hidpad_publish
