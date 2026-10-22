#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 14 Part F: builds and runs the host tests (Linux or macOS; here: WSL) under ASan and UBSan
# with -Wall -Wextra -Werror. Exits non-zero on any build or test failure.
#
#   run-host-tests.sh [<configured Wine 11.4 build dir>]
#
# Groups:
#   pure      always: test_audio_pure, test_wg_pure
#   ffmpeg    test_wma_params (against FFmpeg's own decoder): needs a host FFmpeg, which is
#             built once from refs/ffmpeg-7.1.1 by build-host-ffmpeg.sh into $FFMPEG_HOST
#             (default ~/.cache/myiosdeck/ffmpeg-7.1.1-host)
#   wine      test_wma_e2e, test_parser, test_audio_wine: compile the Wine-side files, which need
#             Wine's generated headers (mfobjects.h, audioclient.h, ...) from a configured Wine
#             build dir ('make include', which needs widl, i.e. bison and flex). Given as the
#             argument or $WINE_BUILD; skipped with a message when absent.
# Environment: CC (default cc), WINE_SRC (default refs/wine-11.4), FFMPEG_HOST, FFMPEG_SRC
# (default refs/ffmpeg-7.1.1), TEST_OUT (default tests/build), TEST_VERBOSE=1 echoes the logs.

set -u

T=$(cd "$(dirname "$0")" && pwd)
H=$(cd "$T/.." && pwd)
REPO=$(cd "$H/../.." && pwd)
OUT=${TEST_OUT:-"$T/build"}
WINE=${WINE_SRC:-"$REPO/refs/wine-11.4"}
WBUILD=${1:-${WINE_BUILD:-}}
# this machine's WSL keeps a configured host Wine 11.4 build in ~/wine-build
if [ -z "$WBUILD" ] && [ -f "$HOME/wine-build/include/mfobjects.h" ]; then WBUILD="$HOME/wine-build"; fi
FF=${FFMPEG_HOST:-"$HOME/.cache/myiosdeck/ffmpeg-7.1.1-host"}
CC=${CC:-cc}

# refs/ is read-only: no build or work folder of this script may resolve into it
REFS=$(cd "$REPO/refs" 2> /dev/null && pwd -P)
refuse_refs() {   # refuse_refs <what> <path>
  p=$2; rest=""
  while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
  case "$(cd "$p" && pwd -P)$rest/" in
    "${REFS:-/nonexistent}"/*) echo "refusing $1 inside refs/ ($2): refs/ is read-only" >&2; exit 1 ;;
  esac
}
refuse_refs "TEST_OUT" "$OUT"
refuse_refs "FFMPEG_HOST" "$FF"
mkdir -p "$OUT" || exit 1

SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
CFLAGS="-std=gnu11 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter -D_GNU_SOURCE $SAN"
LIBS="-lpthread -lm"
INC="-I$T -I$H/common -I$H/winegstreamer-unix -I$H/audio-unix"

status=0
skipped=""

fail() { echo "FAILED: $1"; status=1; }

# compile <object> <source> [flags...]
compile() {
  o=$1; c=$2; shift 2
  # shellcheck disable=SC2086
  $CC $CFLAGS "$@" -c "$c" -o "$OUT/$o.o" 2> "$OUT/$o.log" || { cat "$OUT/$o.log"; fail "build $o"; return 1; }
}

# test <name> <extra libs> <objects...>: link and run
run_test() {
  t=$1; extra=$2; shift 2
  objs=""
  for o in "$@"; do objs="$objs $OUT/$o.o"; done
  # shellcheck disable=SC2086
  if ! $CC $CFLAGS -o "$OUT/$t" $objs $extra $LIBS 2> "$OUT/$t.link.log"; then
    cat "$OUT/$t.link.log"
    fail "link $t"
    return 1
  fi
  echo "==== $t"
  if ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 "$OUT/$t"; then :; else fail "$t"; fi
}

# ---------------------------------------------------------------- pure
compile media_log "$H/common/media_log.c" $INC
compile stubs_common "$T/stubs_common.c" $INC
compile ios_audio_pure "$H/audio-unix/ios_audio_pure.c" $INC
for n in wma_params wg_handles wg_parser_pure wg_platform_pure; do
  compile "$n" "$H/winegstreamer-unix/$n.c" $INC
done
compile test_audio_pure "$T/test_audio_pure.c" $INC && run_test test_audio_pure "" test_audio_pure ios_audio_pure
compile test_wg_pure "$T/test_wg_pure.c" $INC && \
  run_test test_wg_pure "" test_wg_pure wg_handles wg_parser_pure wg_platform_pure media_log stubs_common

# ---------------------------------------------------------------- ffmpeg
if [ ! -f "$FF/lib/libavcodec.a" ]; then
  echo "building the host FFmpeg into $FF (once)"
  sh "$T/build-host-ffmpeg.sh" "${FFMPEG_SRC:-$REPO/refs/ffmpeg-7.1.1}" "$FF" > "$OUT/ffmpeg-host.log" 2>&1 || \
    { tail -20 "$OUT/ffmpeg-host.log"; fail "host FFmpeg build"; }
fi
if [ -f "$FF/lib/libavcodec.a" ]; then
  FFLIBS="$FF/lib/libavformat.a $FF/lib/libavcodec.a $FF/lib/libswresample.a $FF/lib/libavutil.a"
  FFINC="-isystem $FF/include"
  # the reference reads FFmpeg's internal libavcodec/wma.h (and its build's config.h)
  if $CC -std=gnu11 -g -O1 -w -isystem "$FF/src" -I"$H/winegstreamer-unix" -c "$T/wma_reference.c" \
       -o "$OUT/wma_reference.o" 2> "$OUT/wma_reference.log"; then
    compile test_wma_params "$T/test_wma_params.c" $INC && \
      run_test test_wma_params "$FFLIBS" test_wma_params wma_params wma_reference
  else
    cat "$OUT/wma_reference.log"
    fail "build wma_reference"
  fi
else
  skipped="$skipped ffmpeg"
fi

# ---------------------------------------------------------------- wine
# Without a build dir, make Wine's generated headers once when bison and flex are there (widl
# needs them): ~/.cache/myiosdeck/wine-11.4-headers.
if [ -z "$WBUILD" ] && command -v bison > /dev/null 2>&1 && command -v flex > /dev/null 2>&1; then
  WBUILD="$HOME/.cache/myiosdeck/wine-11.4-headers"
  if [ ! -f "$WBUILD/include/mfobjects.h" ]; then
    echo "configuring Wine 11.4 for its generated headers in $WBUILD (once)"
    mkdir -p "$WBUILD"
    ( cd "$WBUILD" && "$WINE/configure" --enable-win64 --without-mingw --without-x --without-freetype \
        --without-gstreamer --disable-tests > "$OUT/wine-configure.log" 2>&1 && \
      make -j"$(nproc 2> /dev/null || echo 4)" include/all > "$OUT/wine-headers.log" 2>&1 ) || \
      { tail -20 "$OUT/wine-configure.log" "$OUT/wine-headers.log" 2> /dev/null; echo "no generated Wine headers"; }
  fi
fi
if [ -n "$WBUILD" ] && [ -f "$WBUILD/include/mfobjects.h" ] && [ -f "$WBUILD/include/audioclient.h" ] && \
   [ -f "$FF/lib/libavcodec.a" ]; then
  WINEFLAGS="-DWINE_UNIX_LIB -D__WINESRC__ -DWINE_IOS=1 -D_REENTRANT -fno-strict-aliasing \
 -isystem $WBUILD/include -isystem $WINE/include"
  WG="$WINEFLAGS -I$WINE/dlls/winegstreamer $INC $FFINC"
  AU="$WINEFLAGS -I$WINE/dlls/mmdevapi $INC"
  for n in wma_core wg_avlog; do compile "$n" "$H/winegstreamer-unix/$n.c" $INC $FFINC; done
  compile stubs_platform "$T/stubs_platform.c" $INC
  compile stubs_audio "$T/stubs_audio.c" $INC
  for n in wg_ios_main wg_transform_ios wg_parser_ios; do compile "$n" "$H/winegstreamer-unix/$n.c" $WG; done
  compile ios_audio "$H/audio-unix/ios_audio.c" $AU
  compile stubs_wine "$T/stubs_wine.c" $AU
  WGOBJS="wg_ios_main wg_transform_ios wg_parser_ios wma_core wma_params wg_avlog wg_handles wg_parser_pure wg_platform_pure media_log stubs_common stubs_platform"
  # shellcheck disable=SC2086
  compile test_wma_e2e "$T/test_wma_e2e.c" $WG && run_test test_wma_e2e "$FFLIBS" test_wma_e2e $WGOBJS
  # shellcheck disable=SC2086
  compile test_parser "$T/test_parser.c" $WG && run_test test_parser "$FFLIBS" test_parser $WGOBJS
  compile test_audio_wine "$T/test_audio_wine.c" $AU && \
    run_test test_audio_wine "" test_audio_wine ios_audio ios_audio_pure media_log stubs_common stubs_audio stubs_wine
else
  skipped="$skipped wine"
  # the Wine-free files of those groups still have to build cleanly
  if [ -f "$FF/lib/libavcodec.a" ]; then
    for n in wma_core wg_avlog; do compile "$n" "$H/winegstreamer-unix/$n.c" $INC $FFINC; done
  fi
  compile stubs_platform "$T/stubs_platform.c" $INC
  compile stubs_audio "$T/stubs_audio.c" $INC
fi

# ---------------------------------------------------------------- App Store rules (spec 00 Part F)
if [ -f "$REPO/check-app-store.sh" ]; then
  sh "$REPO/check-app-store.sh" "$H" || fail "check-app-store"
fi

if [ -n "$skipped" ]; then
  echo "SKIPPED:$skipped (wine: pass a configured Wine 11.4 build dir with generated headers)"
fi
if [ $status -ne 0 ]; then
  echo "spec 14 host tests FAILED"
  exit 1
fi
echo "spec 14 host tests passed"
