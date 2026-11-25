#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# build-openal.sh <openal-soft-1.24.3.tar.bz2> <prefix>
#
# Spec 14 round 4. Builds OpenAL Soft 1.24.3, unmodified (no patches), as a dynamic library for
# iOS arm64 with its CoreAudio backend only, and installs under <prefix>:
#   lib/libopenal.1.dylib        install name @rpath/libopenal.1.dylib
#   OpenALSoft.framework/        the same library wrapped as a framework, install name
#                                @rpath/OpenALSoft.framework/OpenALSoft: an iOS app may not
#                                embed a loose dylib, so this is what the app links and embeds
#   include/AL/                  OpenAL Soft's headers
#   NOTICES.txt                  its licence texts and source location (the app's acknowledgements)
#   openal-relink-kit.tar.gz     the unmodified tarball, this script and openal/README.md (the
#                                LGPL relinking kit; gzip, no liblzma)
#
# Licence: OpenAL Soft is LGPL-2.1-or-later and linked dynamically, never statically. Its
# utilities (alsoft-config) are not built. The checks at the end refuse a library that depends
# on anything but Apple's system libraries, or that lacks an export openal32.dll forwards.
#
# Environment:
#   SDK            iphoneos (default)
#   WORKDIR        work folder (default: <prefix>/../openal-build); unpacked into <WORKDIR>/src
#   JOBS           parallel build jobs (default: 8)
#   BUNDLE_ID      the framework's CFBundleIdentifier (default: org.openal-soft.OpenALSoft)
#
# refs/ is read-only: the prefix and the work folder may not resolve into it.

set -u

VERSION=1.24.3
URL="https://openal-soft.org/openal-releases/openal-soft-$VERSION.tar.bz2"
# The SHA-256 of $URL. Not yet pinned (it could not be downloaded where this was written):
# openal/README.md, "CI", says how to pin it. The script refuses to build until it is set.
EXPECTED_SHA256=

if [ $# -ne 2 ]; then
  echo "usage: $0 <openal-soft-$VERSION.tar.bz2> <prefix>" >&2
  exit 2
fi

HERE=$(cd "$(dirname "$0")" && pwd -P)
PART=$(cd "$HERE/.." && pwd -P)
REFS=$(cd "$HERE/../../../refs" 2> /dev/null && pwd -P)

resolve() {   # the absolute physical path, also of a path that doesn't exist yet
  p=$1; rest=""
  while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
  echo "$(cd "$p" && pwd -P)$rest"
}
refuse_refs() {   # refuse_refs <what> <path>
  [ -n "$REFS" ] || return 0
  case "$(resolve "$2")/" in
    "$REFS"/*) echo "refusing $1 inside refs/ ($2): refs/ is read-only" >&2; exit 1 ;;
  esac
}

TARBALL=$(resolve "$1")
refuse_refs "the prefix" "$2"
mkdir -p "$2" || exit 1
PREFIX=$(cd "$2" && pwd -P) || exit 1
WORKDIR=${WORKDIR:-"$PREFIX/../openal-build"}
refuse_refs "the work folder" "$WORKDIR"
mkdir -p "$WORKDIR" || exit 1
WORKDIR=$(cd "$WORKDIR" && pwd -P) || exit 1
JOBS=${JOBS:-8}
SDK=${SDK:-iphoneos}

# ---------------------------------------------------------------- 1. the tarball's checksum
if [ -z "$EXPECTED_SHA256" ]; then
  echo "EXPECTED_SHA256 is not pinned in $0: see openal/README.md, \"CI\"" >&2
  exit 1
fi
if command -v shasum > /dev/null 2>&1; then
  ACTUAL=$(shasum -a 256 "$TARBALL" | awk '{print $1}')
else
  ACTUAL=$(sha256sum "$TARBALL" | awk '{print $1}')
fi
if [ "$ACTUAL" != "$EXPECTED_SHA256" ]; then
  echo "SHA-256 mismatch for $TARBALL: expected $EXPECTED_SHA256, got $ACTUAL" >&2
  exit 1
fi
echo "OpenAL Soft tarball SHA-256 OK ($ACTUAL)"

# ---------------------------------------------------------------- 2. unpack (no patches)
SRC="$WORKDIR/src"
BLD="$WORKDIR/build"
rm -rf "$SRC" "$BLD"
mkdir -p "$SRC" "$BLD" || exit 1
tar -xjf "$TARBALL" -C "$SRC" --strip-components=1 || exit 1
[ -f "$SRC/CMakeLists.txt" ] || { echo "no CMakeLists.txt in $TARBALL" >&2; exit 1; }

# ---------------------------------------------------------------- 3. configure and build
# CoreAudio is the one backend (required); every other backend, the dlopen of backend
# libraries, the utilities, examples and tests are off.
OFF=""
for b in WAVE SDL2 SDL3 PORTAUDIO PIPEWIRE PULSEAUDIO JACK ALSA OSS SOLARIS SNDIO OPENSL OBOE \
         WINMM DSOUND WASAPI OTHERIO; do
  OFF="$OFF -DALSOFT_BACKEND_$b=OFF"
done
# shellcheck disable=SC2086
cmake -S "$SRC" -B "$BLD" -G "Unix Makefiles" \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT="$SDK" -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_NAME_DIR=@rpath \
  -DLIBTYPE=SHARED -DALSOFT_DLOPEN=OFF \
  -DALSOFT_UTILS=OFF -DALSOFT_NO_CONFIG_UTIL=ON -DALSOFT_EXAMPLES=OFF -DALSOFT_TESTS=OFF \
  -DALSOFT_INSTALL_CONFIG=OFF -DALSOFT_INSTALL_HRTF_DATA=OFF -DALSOFT_INSTALL_AMBDEC_PRESETS=OFF \
  -DALSOFT_INSTALL_EXAMPLES=OFF -DALSOFT_INSTALL_UTILS=OFF \
  -DALSOFT_REQUIRE_COREAUDIO=ON $OFF \
  > "$WORKDIR/configure.log" 2>&1 || { tail -40 "$WORKDIR/configure.log" >&2; exit 1; }
cmake --build "$BLD" -j "$JOBS" > "$WORKDIR/build.log" 2>&1 || { tail -40 "$WORKDIR/build.log" >&2; exit 1; }
cmake --install "$BLD" > "$WORKDIR/install.log" 2>&1 || { tail -20 "$WORKDIR/install.log" >&2; exit 1; }

DYLIB="$PREFIX/lib/libopenal.1.dylib"
[ -f "$DYLIB" ] || { echo "missing $DYLIB" >&2; exit 1; }

# ---------------------------------------------------------------- 4. checks
fail=0
id=$(xcrun otool -D "$DYLIB" | sed -n 2p)
[ "$id" = "@rpath/libopenal.1.dylib" ] || { echo "install name is '$id'" >&2; fail=1; }

# Apple's system libraries only (no liblzma, nothing from outside the SDK)
xcrun otool -L "$DYLIB" | sed -n '2,$p' | awk '{print $1}' | while read -r dep; do
  case "$dep" in
    @rpath/libopenal.1.dylib|/System/Library/Frameworks/*|/usr/lib/libSystem.B.dylib|/usr/lib/libc++.1.dylib) ;;
    *) echo "unexpected dependency: $dep" ;;
  esac
done > "$WORKDIR/deps.txt"
if [ -s "$WORKDIR/deps.txt" ]; then cat "$WORKDIR/deps.txt" >&2; fail=1; fi
grep -q CoreAudio "$WORKDIR/configure.log" || { echo "CoreAudio backend not configured" >&2; fail=1; }
if xcrun nm -u "$DYLIB" | grep -q '^_dlopen$'; then
  echo "note: the library imports dlopen (ALSOFT_DLOPEN is off; see openal/README.md)"
fi

# every export openal32.dll forwards
xcrun nm -gU "$DYLIB" | awk '{print $3}' | sed 's/^_//' | sort -u > "$WORKDIR/exports.txt"
FUNCS="$PART/pe/dlls/openal32/openal32_funcs.h"
[ -f "$HERE/openal32_funcs.h" ] && FUNCS="$HERE/openal32_funcs.h"   # in the relinking kit
sed -n 's/^ *OA(\([A-Za-z0-9_]*\),.*/\1/p' "$FUNCS" | sort -u > "$WORKDIR/needed.txt"
missing=$(comm -23 "$WORKDIR/needed.txt" "$WORKDIR/exports.txt" | tr '\n' ' ')
[ -z "$missing" ] || { echo "OpenAL Soft lacks: $missing" >&2; fail=1; }
[ $fail -eq 0 ] || exit 1
echo "libopenal.1.dylib: system dependencies only, $(wc -l < "$WORKDIR/needed.txt") exports present"

# ---------------------------------------------------------------- 5. the framework
FW="$PREFIX/OpenALSoft.framework"
rm -rf "$FW"
mkdir -p "$FW" || exit 1
cp "$DYLIB" "$FW/OpenALSoft" || exit 1
xcrun install_name_tool -id @rpath/OpenALSoft.framework/OpenALSoft "$FW/OpenALSoft" || exit 1
xcrun codesign -f -s - "$FW/OpenALSoft" > /dev/null 2>&1 || exit 1   # the app's embed step re-signs
cat > "$FW/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>OpenALSoft</string>
  <key>CFBundleIdentifier</key><string>${BUNDLE_ID:-org.openal-soft.OpenALSoft}</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>OpenALSoft</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
  <key>MinimumOSVersion</key><string>17.0</string>
</dict>
</plist>
EOF
echo "built $FW"

# ---------------------------------------------------------------- 6. notices and the relinking kit
{
  echo "OpenAL Soft $VERSION"
  echo "Licence: GNU Library General Public License, version 2 or later (LGPL-2.1-or-later),"
  echo "with the notices of the parts it includes, below. Used unmodified, linked dynamically."
  echo "Source: $URL (SHA-256 $EXPECTED_SHA256)"
  echo "Project: https://openal-soft.org/  The app's relinking kit has the tarball and build script."
  (cd "$SRC" && find . -type f \( -iname 'COPYING*' -o -iname 'LICENSE*' -o -iname 'NOTICE*' \) | sort) |
  while read -r f; do
    printf '\n==== %s\n\n' "${f#./}"
    cat "$SRC/$f"
  done
} > "$PREFIX/NOTICES.txt"

KIT="$WORKDIR/kit/openal-relink-kit"
rm -rf "$WORKDIR/kit"
mkdir -p "$KIT" || exit 1
cp "$TARBALL" "$KIT/openal-soft-$VERSION.tar.bz2" && cp "$HERE/build-openal.sh" "$HERE/README.md" "$KIT/" &&
  cp "$FUNCS" "$KIT/" || exit 1
tar -czf "$PREFIX/openal-relink-kit.tar.gz" -C "$WORKDIR/kit" openal-relink-kit || exit 1
echo "wrote $PREFIX/NOTICES.txt and $PREFIX/openal-relink-kit.tar.gz"
