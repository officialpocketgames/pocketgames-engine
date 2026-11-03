#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# regen.sh <wine-11.4 tree> <work dir> [<registry dir> [<EGL registry dir>]]
#
# Spec 35 Part A3: one run of this tree's make_opengl (pe/dlls/opengl32/), with Perl (WSL), for
# both halves of opengl32. Needs Perl with XML::LibXML (Ubuntu: libxml-libxml-perl).
#
# 1. The four Khronos registry files at the commits make_opengl pins, put into make_opengl's
#    cache (<work dir>/cache/wine/, under the names it expects, so it downloads nothing): from
#    the registry dirs when given, each file under its upstream name (gl.xml, wgl.xml,
#    registry.py; egl.xml, in the EGL dir or else the first) or its cache name (gl-<c>.xml,
#    wgl-<c>.xml, registry-<c>.py, egl-<c2>.xml), else downloaded with curl. Never into refs/
#    (refs/khronos-gl-0b449b9 and refs/khronos-egl-3ae2b7c are read only). Their SHA-256 is
#    printed (PROVENANCE.md records it).
# 2. Wine 11.4's own make_opengl on a copy of dlls/opengl32 and include/wine: its outputs must
#    equal the Wine tree's files byte for byte, which proves the registry files are the ones Wine
#    11.4 generated from.
# 3. This tree's make_opengl (with make_opengl_ios.pm) on a second copy: thunks.c, unixlib.h,
#    unix_thunks.h, opengl32.spec and include/wine/wgl.h must equal Wine's (in this step only the
#    unix side's WoW64 thunks change, Part G1); unix_thunks.c is copied here (opengl32-unix/),
#    where build-unix.sh compiles it.
# Nothing is written outside <work dir> but opengl32-unix/unix_thunks.c.

set -eu

OGL_COMMIT=0b449b97cdf1043eef5e1f0e235cbbab6ec10c86
EGL_COMMIT=3ae2b7c48690d2ce13cc6db3db02dfc0572be65e
OGL_URL=https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry
EGL_URL=https://raw.githubusercontent.com/KhronosGroup/EGL-Registry

[ $# -ge 2 ] || { echo "usage: $0 <wine-11.4 tree> <work dir> [<registry dir> [<EGL registry dir>]]" >&2; exit 2; }
GLDIR=${3:-}
EGLDIR=${4:-${3:-}}
HERE=$(cd "$(dirname "$0")" && pwd)
PART=$(cd "$HERE/.." && pwd)
REPO=$(cd "$PART/../.." && pwd)
WINE=$(cd "$1" && pwd)

REFS=$(cd "$REPO/refs" 2>/dev/null && pwd -P) || REFS=""
if [ -n "$REFS" ]; then
  p=$2; rest=""
  while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
  case "$(cd "$p" && pwd -P)$rest/" in
    "$REFS/"*) echo "$0: refusing a work dir inside refs/: $2" >&2; exit 1 ;;
  esac
fi
mkdir -p "$2"
WORK=$(cd "$2" && pwd)
CACHE="$WORK/cache/wine"
mkdir -p "$CACHE"

perl -MXML::LibXML -e 1 2>/dev/null || { echo "$0: Perl's XML::LibXML is missing (libxml-libxml-perl)" >&2; exit 1; }

# ---- 1. the registry
for f in "gl-$OGL_COMMIT.xml:gl.xml:$GLDIR:$OGL_URL/$OGL_COMMIT/xml/gl.xml" \
         "wgl-$OGL_COMMIT.xml:wgl.xml:$GLDIR:$OGL_URL/$OGL_COMMIT/xml/wgl.xml" \
         "registry-$OGL_COMMIT.py:registry.py:$GLDIR:$OGL_URL/$OGL_COMMIT/extensions/registry.py" \
         "egl-$EGL_COMMIT.xml:egl.xml:$EGLDIR:$EGL_URL/$EGL_COMMIT/api/egl.xml"; do
  name=${f%%:*}; f=${f#*:}
  plain=${f%%:*}; f=${f#*:}
  dir=${f%%:*}
  url=${f#*:}
  if [ -n "$dir" ] && [ -f "$dir/$name" ]; then cp "$dir/$name" "$CACHE/$name"
  elif [ -n "$dir" ] && [ -f "$dir/$plain" ]; then cp "$dir/$plain" "$CACHE/$name"
  elif [ ! -s "$CACHE/$name" ]; then curl -fsSL -o "$CACHE/$name" "$url"
  fi
  echo "registry: $(sha256sum "$CACHE/$name" 2>/dev/null || shasum -a 256 "$CACHE/$name") ($plain)"
done

# a copy of what make_opengl reads and writes: dlls/opengl32 and include/wine
copy_tree() {   # copy_tree <dir>
  rm -rf "$1"
  mkdir -p "$1/dlls" "$1/include"
  cp -R "$WINE/dlls/opengl32" "$1/dlls/"
  cp -R "$WINE/include/wine" "$1/include/"
}
compare() {   # compare <tree> <file...>: each must equal the Wine tree's
  tree=$1
  shift
  for f in "$@"; do
    case "$f" in
      include/*) w="$WINE/$f"; t="$tree/$f" ;;
      *) w="$WINE/dlls/opengl32/$f"; t="$tree/dlls/opengl32/$f" ;;
    esac
    if ! cmp -s "$w" "$t"; then echo "$0: $t differs from Wine 11.4's $f" >&2; diff "$w" "$t" | head -20 >&2; exit 1; fi
  done
}
OUTPUTS="thunks.c unixlib.h unix_thunks.h opengl32.spec include/wine/wgl.h"

# ---- 2. upstream's generator reproduces Wine 11.4
TREE="$WORK/upstream"
copy_tree "$TREE"
(cd "$TREE/dlls/opengl32" && XDG_CACHE_HOME="$WORK/cache" perl ./make_opengl)
# shellcheck disable=SC2086
compare "$TREE" $OUTPUTS unix_thunks.c
echo "regen: Wine 11.4's make_opengl reproduces Wine 11.4's files from these registry files"

# ---- 3. this tree's generator
TREE="$WORK/ios"
copy_tree "$TREE"
cp "$PART/pe/dlls/opengl32/make_opengl" "$PART/pe/dlls/opengl32/make_opengl_ios.pm" "$TREE/dlls/opengl32/"
(cd "$TREE/dlls/opengl32" && XDG_CACHE_HOME="$WORK/cache" perl ./make_opengl)
# shellcheck disable=SC2086
compare "$TREE" $OUTPUTS
cp "$TREE/dlls/opengl32/unix_thunks.c" "$HERE/unix_thunks.c"
echo "regen: $HERE/unix_thunks.c written ($(wc -l < "$HERE/unix_thunks.c") lines); the PE side is Wine 11.4's"
grep -c 'WOW_HOST\|WOW_GUEST\|WOW_HANDLE' "$HERE/unix_thunks.c" | sed 's/^/regen: guest-window conversions: /'
