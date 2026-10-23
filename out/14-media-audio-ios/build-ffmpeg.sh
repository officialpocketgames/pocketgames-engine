#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# build-ffmpeg.sh <ffmpeg-7.1.1 tarball> <prefix>
#
# Spec 14 Part A2. Cross-compiles FFmpeg 7.1.1, unchanged (no patches), as static libraries
# for iOS arm64 and installs them under <prefix> (lib/libavformat.a, libavcodec.a,
# libswresample.a, libavutil.a and include/).
#
# Licence: only LGPL-2.1-or-later components are enabled. Never add a GPL, version-3 or
# non-free component here: the app links FFmpeg statically, and any of those would change the
# licence of the whole app. ffmpeg/README.md explains every option.
#
# Environment:
#   CC          the C compiler (default: xcrun -sdk iphoneos clang)
#   SDKROOT     the iPhoneOS SDK (default: xcrun -sdk iphoneos --show-sdk-path)
#   WORKDIR     work folder; the tarball is unpacked into <WORKDIR>/src
#               (default: <prefix>/../ffmpeg-build)
#   JOBS        parallel make jobs (default: 8)
#
# refs/ is read-only: the prefix and the work folder may not resolve into it.

set -u

EXPECTED_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1

if [ $# -ne 2 ]; then
  echo "usage: $0 <ffmpeg-7.1.1 tarball> <prefix>" >&2
  exit 2
fi

HERE=$(cd "$(dirname "$0")" && pwd -P)
REFS=$(cd "$HERE/../../refs" 2> /dev/null && pwd -P)

# resolve <path>: the absolute physical path, also of a path that doesn't exist yet
resolve() {
  p=$1; rest=""
  while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
  echo "$(cd "$p" && pwd -P)$rest"
}
# refuse_refs <what> <path>: stop when the path is inside refs/
refuse_refs() {
  [ -n "$REFS" ] || return 0
  case "$(resolve "$2")/" in
    "$REFS"/*) echo "refusing $1 inside refs/ ($2): refs/ is read-only" >&2; exit 1 ;;
  esac
}

TARBALL=$1
refuse_refs "the prefix" "$2"
mkdir -p "$2" || exit 1
PREFIX=$(cd "$2" && pwd) || exit 1
WORKDIR=${WORKDIR:-"$PREFIX/../ffmpeg-build"}
refuse_refs "the work folder" "$WORKDIR"
JOBS=${JOBS:-8}

# ---------------------------------------------------------------- 1. the tarball's checksum
if command -v shasum > /dev/null 2>&1; then
  ACTUAL=$(shasum -a 256 "$TARBALL" | awk '{print $1}')
else
  ACTUAL=$(sha256sum "$TARBALL" | awk '{print $1}')
fi
if [ "$ACTUAL" != "$EXPECTED_SHA256" ]; then
  echo "SHA-256 mismatch for $TARBALL" >&2
  echo "  expected $EXPECTED_SHA256" >&2
  echo "  got      $ACTUAL" >&2
  exit 1
fi
echo "FFmpeg tarball SHA-256 OK ($ACTUAL)"

# ---------------------------------------------------------------- 2. unpack (no patches)
# Into a fixed subfolder with the tarball's top folder stripped: whatever the work folder is
# called (CI found that a search for "ffmpeg-*" also matched a work folder of that name).
SRC="$WORKDIR/src"
rm -rf "$SRC"
mkdir -p "$SRC" || exit 1
tar -xf "$TARBALL" -C "$SRC" --strip-components=1 || exit 1
[ -x "$SRC/configure" ] || { echo "no configure in $TARBALL" >&2; exit 1; }

# ---------------------------------------------------------------- 3. configure
CC=${CC:-"xcrun -sdk iphoneos clang"}
SDKROOT=${SDKROOT:-$(xcrun -sdk iphoneos --show-sdk-path)}
ARCHFLAGS="-arch arm64 -miphoneos-version-min=17.0"

cd "$SRC" || exit 1

# Decoders: the WMA family for the WMA decoder MFT (Part B2), MPEG audio and PCM for the parser
# (Part B3). Demuxers mp3/wav/mov and the mpegaudio parser the mp3 demuxer needs. No H.264,
# HEVC or AAC decoder or parser: those go to VideoToolbox and AudioToolbox (Part B4), called
# directly by our own code.
DECODERS="wmav1,wmav2,wmapro,wmalossless,xma1,xma2,mp1,mp2,mp3"
DECODERS="$DECODERS,pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le"
DEMUXERS="mp3,wav,mov"
PARSERS="mpegaudio"

./configure \
  --prefix="$PREFIX" \
  --target-os=darwin \
  --arch=aarch64 \
  --enable-cross-compile \
  --cc="$CC" \
  --sysroot="$SDKROOT" \
  --extra-cflags="$ARCHFLAGS -fvisibility=hidden" \
  --extra-ldflags="$ARCHFLAGS" \
  --disable-gpl --disable-nonfree --disable-version3 \
  --disable-everything \
  --enable-decoder="$DECODERS" \
  --enable-demuxer="$DEMUXERS" \
  --enable-parser="$PARSERS" \
  --disable-protocols --disable-muxers --disable-network \
  --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
  --enable-avformat --enable-avcodec --enable-avutil --enable-swresample \
  --disable-autodetect \
  --disable-zlib --disable-bzlib --disable-lzma --disable-iconv --disable-sdl2 \
  --disable-audiotoolbox --disable-videotoolbox --disable-securetransport \
  --disable-appkit --disable-avfoundation --disable-coreimage --disable-metal \
  --disable-asm \
  --enable-static --disable-shared --enable-pic --disable-debug \
  || { echo "configure failed; see $SRC/ffbuild/config.log" >&2; exit 1; }

# The licence the configuration ended up with must be LGPL 2.1 or later.
if ! grep -q '^#define FFMPEG_LICENSE "LGPL version 2.1 or later"' config.h; then
  echo "FFmpeg configuration is not LGPL-2.1-or-later:" >&2
  grep FFMPEG_LICENSE config.h >&2
  exit 1
fi
for bad in CONFIG_GPL CONFIG_NONFREE CONFIG_VERSION3; do
  if grep -q "^#define $bad 1" config.h; then
    echo "$bad is enabled; refusing to build" >&2
    exit 1
  fi
done

# ---------------------------------------------------------------- 4. build and install
make -j"$JOBS" || exit 1
make install || exit 1

for l in libavformat libavcodec libswresample libavutil; do
  [ -f "$PREFIX/lib/$l.a" ] || { echo "missing $PREFIX/lib/$l.a" >&2; exit 1; }
done

# ---------------------------------------------------------------- 5. the component list for CI logs
echo "==== FFmpeg $(cat VERSION 2> /dev/null || echo 7.1.1) components (config_components.h)"
for kind in DECODER DEMUXER PARSER PROTOCOL MUXER; do
  list=$(grep "^#define CONFIG_[A-Z0-9_]*_$kind 1" config_components.h \
         | sed "s/^#define CONFIG_\\([A-Z0-9_]*\\)_$kind 1/\\1/" | tr 'A-Z' 'a-z' | tr '\n' ' ')
  echo "enabled ${kind}S: ${list:-(none)}"
done
echo "installed into $PREFIX (link order: libavformat.a libavcodec.a libswresample.a libavutil.a)"
