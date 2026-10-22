#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# build-host-ffmpeg.sh <ffmpeg-7.1.1 tarball> <prefix>
#
# Spec 14 Part F. A host (Linux or macOS) FFmpeg from the same tarball as the app's, for the host
# tests: the components of build-ffmpeg.sh (A2) plus, for making test data only, the wmav1, wmav2
# and mp2 encoders and the mp4/mov muxer. All LGPL-2.1-or-later. Built in <prefix>/src (the
# WMA reference test reads FFmpeg's internal libavcodec/wma.h from there) and installed into
# <prefix>.

set -u

EXPECTED_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1

if [ $# -ne 2 ]; then
  echo "usage: $0 <ffmpeg-7.1.1 tarball or unpacked tree (refs/ffmpeg-7.1.1)> <prefix>" >&2
  exit 2
fi
SOURCE=$1
# refs/ is read-only (it may be the source, which is only copied): the prefix may not be in it
REFS=$(cd "$(dirname "$0")/../../../refs" 2> /dev/null && pwd -P)
p=$2; rest=""
while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
case "$(cd "$p" && pwd -P)$rest/" in
  "${REFS:-/nonexistent}"/*) echo "refusing a prefix inside refs/ ($2): refs/ is read-only" >&2; exit 1 ;;
esac
mkdir -p "$2" || exit 1
PREFIX=$(cd "$2" && pwd) || exit 1
JOBS=${JOBS:-$(nproc 2> /dev/null || echo 8)}

rm -rf "$PREFIX/src"
mkdir -p "$PREFIX/src" || exit 1
if [ -d "$SOURCE" ]; then
  # an unpacked tree (the repository's refs/ffmpeg-7.1.1, itself from the checked tarball)
  [ "$(cat "$SOURCE/VERSION" 2> /dev/null)" = "7.1.1" ] || { echo "$SOURCE is not FFmpeg 7.1.1" >&2; exit 1; }
  cp -R "$SOURCE"/. "$PREFIX/src"/ || exit 1
else
  if command -v shasum > /dev/null 2>&1; then
    ACTUAL=$(shasum -a 256 "$SOURCE" | awk '{print $1}')
  else
    ACTUAL=$(sha256sum "$SOURCE" | awk '{print $1}')
  fi
  [ "$ACTUAL" = "$EXPECTED_SHA256" ] || { echo "SHA-256 mismatch for $SOURCE: $ACTUAL" >&2; exit 1; }
  tar -xf "$SOURCE" -C "$PREFIX/src" --strip-components=1 || exit 1
fi
cd "$PREFIX/src" || exit 1

./configure \
  --prefix="$PREFIX" \
  --cc="${CC:-cc}" \
  --disable-gpl --disable-nonfree --disable-version3 \
  --disable-everything \
  --enable-decoder=wmav1,wmav2,wmapro,wmalossless,xma1,xma2,mp1,mp2,mp3,pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le \
  --enable-demuxer=mp3,wav,mov \
  --enable-parser=mpegaudio \
  --enable-encoder=wmav1,wmav2,mp2 \
  --enable-muxer=mp4,mov \
  --disable-protocols --disable-network \
  --disable-programs --disable-doc \
  --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
  --enable-avformat --enable-avcodec --enable-avutil --enable-swresample \
  --disable-autodetect --disable-asm \
  --enable-static --disable-shared --enable-pic \
  || { echo "configure failed; see $PREFIX/src/ffbuild/config.log" >&2; exit 1; }
grep -q '^#define FFMPEG_LICENSE "LGPL version 2.1 or later"' config.h || { echo "not LGPL-2.1+" >&2; exit 1; }
make -j"$JOBS" || exit 1
make install || exit 1
echo "host FFmpeg in $PREFIX (sources and config.h in $PREFIX/src)"
