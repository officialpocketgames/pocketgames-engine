#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# build-ios.sh <wine-11.4 tree> <wine build dir> <ffmpeg prefix> <out dir>
#
# Spec 14 Part A1. Builds, for iOS arm64, one static library <out dir>/libwinemedia_unix.a with
#   - winegstreamer's unix side (winegstreamer-unix/: the WMA decoder on libavcodec, the media
#     parser on libavformat, and the platform-decoder object on VideoToolbox/AudioToolbox),
#   - the audio driver, mmdevapi's unix side (audio-unix/),
#   - dwrite's unix side (dwrite-unix/: upstream dlls/dwrite/freetype.c, unchanged, compiled
#     through a wrapper against spec 12's static FreeType),
#   - the shared helpers (common/).
# Upstream sources are never copied into this repository; this script copies the one upstream
# file it compiles (dlls/dwrite/freetype.c) from the given Wine tree into <out dir>/work.
#
# Link the app with (spec 14 Part A1, A3):
#   -Wl,-force_load,<out dir>/libwinemedia_unix.a      (its call tables are reached only by
#                                                       name, through dlsym; nothing references
#                                                       them, so a plain link would drop them)
#   <ffmpeg prefix>/lib/libavformat.a libavcodec.a libswresample.a libavutil.a   (this order)
#   -framework AudioToolbox -framework VideoToolbox -framework CoreMedia -framework CoreVideo
#   -framework CoreFoundation
# together with specs 08-12's libraries (FreeType comes from spec 12's libwin32u_unix.a; never
# link a second copy).
#
# Environment:
#   CC, AR              compiler / archiver (default: xcrun -sdk iphoneos clang / ar)
#   FREETYPE_SRC        FreeType 2.13.3 tree for its headers (default: <repo>/refs/freetype-2.13.3)
#   EXTRA_CFLAGS        appended to every compile

set -u

if [ $# -ne 4 ]; then
  echo "usage: $0 <wine-11.4 tree> <wine build dir> <ffmpeg prefix> <out dir>" >&2
  exit 2
fi

WINE=$(cd "$1" && pwd) || exit 1
BUILD=$(cd "$2" && pwd) || exit 1
FFP=$(cd "$3" && pwd) || exit 1
# refs/ is read-only (the Wine and FreeType trees there are only read): the out dir, where the
# work and object folders go, may not resolve into it
REFS=$(cd "$(dirname "$0")/../../refs" 2> /dev/null && pwd -P)
p=$4; rest=""
while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
case "$(cd "$p" && pwd -P)$rest/" in
  "${REFS:-/nonexistent}"/*) echo "refusing an out dir inside refs/ ($4): refs/ is read-only" >&2; exit 1 ;;
esac
mkdir -p "$4" || exit 1
OUT=$(cd "$4" && pwd) || exit 1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
FT=${FREETYPE_SRC:-"$REPO/refs/freetype-2.13.3"}

WORK="$OUT/work"
OBJ="$OUT/obj"
LOG="$OUT/log"
rm -rf "$WORK" "$OBJ" "$LOG"
mkdir -p "$WORK/dwrite" "$OBJ" "$LOG" || exit 1

for f in "$BUILD/include/mfobjects.h" "$BUILD/include/mftransform.h" "$BUILD/include/dwrite.h" \
         "$BUILD/include/dwrite_3.h" "$BUILD/include/audioclient.h" "$BUILD/include/mmdeviceapi.h" \
         "$BUILD/include/config.h" "$FFP/include/libavcodec/avcodec.h" "$FT/include/ft2build.h"; do
  [ -f "$f" ] || { echo "missing $f (configure and 'make include' the Wine build; build FFmpeg first)" >&2; exit 1; }
done

# upstream freetype.c, unchanged; the wrapper dwrite-unix/dwrite_freetype_ios.c includes it
cp "$WINE/dlls/dwrite/freetype.c" "$WORK/dwrite/freetype_upstream.inc" || exit 1

CC=${CC:-"xcrun -sdk iphoneos clang"}
AR=${AR:-"xcrun -sdk iphoneos ar"}

COMMON="-arch arm64 -miphoneos-version-min=17.0 -O2 -std=gnu11 -fPIC -fvisibility=hidden \
 -fno-strict-aliasing -fasynchronous-unwind-tables -fno-omit-frame-pointer -ffixed-x18 \
 -Wall -Wno-unknown-pragmas -Wno-pragma-pack ${EXTRA_CFLAGS:-}"
WINEDEFS="-DWINE_IOS=1 -DWINE_UNIX_LIB -D__WINESRC__ -D_REENTRANT"
# spec 17 Part A1: the shared header overrides (wine/unixlib.h, winternl.h) before the Wine
# tree's include/
WINEINC="-I$HERE/../09-ntdll-processes/include -I$BUILD/include -I$WINE/include"

# Files that include no Wine header (Part A1: CoreFoundation and winnt.h define clashing names;
# the pure files are shared with the host tests). They get no Wine include path at all.
PLAIN_WG="wma_params wma_core wg_avlog wg_handles wg_parser_pure wg_platform_pure wg_platform_apple"
PLAIN_AUDIO="ios_audio_pure ios_audio_apple"

failed=""
compile() {   # compile <source> <object name> [flags...]
  c=$1; n=$2; shift 2
  # shellcheck disable=SC2086
  if $CC $COMMON "$@" -c "$c" -o "$OBJ/$n.o" > "$LOG/$n.log" 2>&1; then
    echo "$n OK"
  else
    echo "$n FAILED"
    failed="$failed $n"
  fi
}
is_in() { for x in $2; do [ "$x" = "$1" ] && return 0; done; return 1; }

# ---------------------------------------------------------------- common/
for c in "$HERE"/common/*.c; do
  n=$(basename "$c" .c)
  compile "$c" "common-$n" "-I$HERE/common"
done

# ---------------------------------------------------------------- winegstreamer-unix/ (Part B)
for c in "$HERE"/winegstreamer-unix/*.c; do
  n=$(basename "$c" .c)
  if is_in "$n" "$PLAIN_WG"; then
    # shellcheck disable=SC2086
    compile "$c" "wg-$n" "-I$HERE/common" "-I$HERE/winegstreamer-unix" "-I$FFP/include"
  else
    # shellcheck disable=SC2086
    compile "$c" "wg-$n" $WINEDEFS "-I$HERE/common" "-I$HERE/winegstreamer-unix" \
      "-I$WINE/dlls/winegstreamer" $WINEINC "-I$FFP/include"
  fi
done

# ---------------------------------------------------------------- audio-unix/ (Part C)
for c in "$HERE"/audio-unix/*.c; do
  n=$(basename "$c" .c)
  if is_in "$n" "$PLAIN_AUDIO"; then
    compile "$c" "audio-$n" "-I$HERE/common" "-I$HERE/audio-unix"
  else
    # shellcheck disable=SC2086
    compile "$c" "audio-$n" $WINEDEFS "-I$HERE/common" "-I$HERE/audio-unix" \
      "-I$WINE/dlls/mmdevapi" $WINEINC
  fi
done

# ---------------------------------------------------------------- dwrite-unix/ (Part D)
# -I$WORK/dwrite first: freetype_upstream.inc; then dlls/dwrite for its unixlib.h and
# dwrite_private.h; FreeType's headers only (the code is spec 12's static FreeType).
# shellcheck disable=SC2086
compile "$HERE/dwrite-unix/dwrite_freetype_ios.c" "dwrite-freetype" $WINEDEFS \
  "-I$HERE/common" "-I$WORK/dwrite" "-I$WINE/dlls/dwrite" $WINEINC "-I$FT/include" \
  -Wno-unused-function

if [ -n "$failed" ]; then
  echo "FAILED:$failed"
  for f in $failed; do echo "==== $f"; head -60 "$LOG/$f.log"; done
  exit 1
fi

# ---------------------------------------------------------------- one library
rm -f "$OUT/libwinemedia_unix.a"
# shellcheck disable=SC2086
$AR rcs "$OUT/libwinemedia_unix.a" "$OBJ"/*.o || exit 1
echo "built $OUT/libwinemedia_unix.a"

# The tables spec 09's loader looks up by name (Part A3) must be in the library, defined.
NM=${NM:-"xcrun -sdk iphoneos nm"}
missing=""
for s in winegstreamer_unix_call_funcs winegstreamer_unix_call_wow64_funcs winegstreamer_unix_lib_init \
         dwrite_unix_call_funcs dwrite_unix_call_wow64_funcs \
         winecoreaudio_unix_call_funcs winecoreaudio_unix_call_wow64_funcs \
         wineios_unix_call_funcs wineios_unix_call_wow64_funcs; do
  # shellcheck disable=SC2086
  $NM -g "$OUT/libwinemedia_unix.a" 2> /dev/null | grep -q " [DST] _$s\$" || missing="$missing $s"
done
if [ -n "$missing" ]; then
  echo "tables missing from the library:$missing" >&2
  exit 1
fi
echo "unix call tables present"
