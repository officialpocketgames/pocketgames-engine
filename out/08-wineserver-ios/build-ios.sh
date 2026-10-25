#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 08 Part A: build upstream Wine 11.4's server/ as a static library for iOS arm64.
#
#   build-ios.sh <wine-11.4 tree> <wine build dir with generated headers> <out dir>
#
# The build dir comes from a host (macOS) configure of the same tree; it provides
# include/config.h and the other generated files. Every .c file listed in SOURCES of the
# tree's server/Makefile.in is compiled; where this folder's server/ has a file of the same
# name (a modified copy), that one is used. The new iOS files (server/ios_*.c) are added.
# Output: <out dir>/libwineserver.a. One line is printed per file; the first compiler failure
# prints the compiler's messages and stops the build.

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
OURS="$HERE/server"

MIN_IOS=17.0
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
LIBTOOL=$(xcrun --sdk iphoneos -f libtool)
NM=$(xcrun --sdk iphoneos -f nm)

# --- checks ----------------------------------------------------------------------------
[ -f "$WINE_SRC/server/Makefile.in" ] || { echo "error: $WINE_SRC/server/Makefile.in not found" >&2; exit 1; }
[ -f "$WINE_BUILD/include/config.h" ] || { echo "error: $WINE_BUILD/include/config.h not found (run a host configure first)" >&2; exit 1; }
if [ ! -f "$WINE_BUILD/include/wine/server_protocol.h" ] && [ ! -f "$WINE_SRC/include/wine/server_protocol.h" ]; then
    echo "error: include/wine/server_protocol.h not found in the build dir or the tree" >&2
    exit 1
fi

# --- staging: upstream server/ overlaid with this folder's modified and new files -------
# Quote includes are searched in the including file's own directory first, so the modified
# headers only win if they sit next to the sources. Hence one staging directory.
STAGE="$OUT/stage"
OBJ="$OUT/obj"
rm -rf "$STAGE" "$OBJ"
mkdir -p "$STAGE" "$OBJ"
cp "$WINE_SRC"/server/*.c "$WINE_SRC"/server/*.h "$STAGE"/
cp "$OURS"/*.c "$OURS"/*.h "$STAGE"/ 2>/dev/null || true
# spec 11 Part A2's shared syscall-frame header: Part E takes its TEB offsets from it
SHARED_FRAME="$HERE/../09-ntdll-processes/ntdll-unix/ios_syscall_frame.h"
[ -f "$SHARED_FRAME" ] || { echo "error: $SHARED_FRAME not found (spec 11 Part A2)" >&2; exit 1; }
cp "$SHARED_FRAME" "$STAGE"/

# --- the file list -----------------------------------------------------------------------
UPSTREAM_SOURCES=$(awk '
    /^SOURCES[ \t]*=/ { in_list = 1; sub(/^SOURCES[ \t]*=/, "") }
    in_list {
        line = $0
        cont = (line ~ /\\[ \t]*$/)
        sub(/\\[ \t]*$/, "", line)
        n = split(line, words, /[ \t]+/)
        for (i = 1; i <= n; i++) if (words[i] ~ /\.c$/) print words[i]
        if (!cont) in_list = 0
    }' "$WINE_SRC/server/Makefile.in")

IOS_SOURCES=""
for f in "$OURS"/ios_*.c; do
    [ -f "$f" ] && IOS_SOURCES="$IOS_SOURCES $(basename "$f")"
done

# --- flags ----------------------------------------------------------------------------------
CFLAGS="-arch arm64 -isysroot $SDK -miphoneos-version-min=$MIN_IOS -O2 -std=gnu11 -pipe"
CFLAGS="$CFLAGS -fno-strict-aliasing -fno-common -Wall -Wno-unused-function -Wno-pointer-sign"
CFLAGS="$CFLAGS -DWINE_IOS=1 -D__WINESRC__ -D_REENTRANT"
CFLAGS="$CFLAGS -include $STAGE/ios_config.h"
# spec 16 (clarification 8): the userspace ntsync header and the fastsync cells; the table and
# the library come from libntdll_unix.a (spec 09) in the same app binary
SYNC_INC=$(cd "$HERE/../16-sync-ios/include" && pwd)
CFLAGS="$CFLAGS -DHAVE_LINUX_NTSYNC_H=1 -DIOS_USYNC=1"
# spec 17 Part A1 (spec 08 clarification 7): the shared header overrides (wine/unixlib.h,
# winternl.h) come before the Wine tree's include/
OVERRIDE_INC=$(cd "$HERE/../09-ntdll-processes/include" && pwd)
# spec 15 clarification 10b: the raw input sink's contract header (ios_hidpad_rawinput.h)
RAWINPUT_INC=$(cd "$HERE/../15-controllers/server" && pwd)
CFLAGS="$CFLAGS -I$STAGE -I$SYNC_INC -I$OVERRIDE_INC -I$RAWINPUT_INC -I$WINE_BUILD/include -I$WINE_SRC/include"

echo "wineserver for iOS arm64 (minimum iOS $MIN_IOS)"
echo "  tree:  $WINE_SRC"
echo "  build: $WINE_BUILD"
echo "  out:   $OUT"

# (CFLAGS is split on spaces below: the tree, build and out paths must not contain spaces.)
OBJECTS=""
for src in $UPSTREAM_SOURCES $IOS_SOURCES; do
    if [ -f "$OURS/$src" ]; then origin="ios"; else origin="upstream"; fi
    printf '  CC  %-22s (%s)\n' "$src" "$origin"
    obj="$OBJ/${src%.c}.o"
    log="$OBJ/${src%.c}.log"
    # unicode.c needs upstream's unicode_EXTRADEFS; the paths are not used on iOS (Part B5)
    if [ "$src" = unicode.c ]; then
        set -- '-DBINDIR="/usr/local/bin"' '-DDATADIR="/usr/local/share"'
    else
        set --
    fi
    # shellcheck disable=SC2086
    if ! "$CC" $CFLAGS "$@" -c "$STAGE/$src" -o "$obj" >"$log" 2>&1; then
        echo "error: $src failed to compile:" >&2
        cat "$log" >&2
        exit 1
    fi
    OBJECTS="$OBJECTS $obj"
done

rm -f "$OUT/libwineserver.a"
# shellcheck disable=SC2086
"$LIBTOOL" -static -o "$OUT/libwineserver.a" $OBJECTS

# The app links this next to the rest of Wine's unix side: there must be no main().
if "$NM" -g "$OUT/libwineserver.a" | grep -q ' T _main$'; then
    echo "error: libwineserver.a still defines main" >&2
    exit 1
fi
if ! "$NM" -g "$OUT/libwineserver.a" | grep -q ' T _wineserver_main$'; then
    echo "error: libwineserver.a does not define wineserver_main" >&2
    exit 1
fi
echo "  AR  libwineserver.a"
