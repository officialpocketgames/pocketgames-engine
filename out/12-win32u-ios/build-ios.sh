#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# build-ios.sh <wine-11.4 tree> <wine build dir> <out dir>
#
# Builds win32u's unix side for iOS arm64 (spec 12 Part A) as one static library,
# <out dir>/libwin32u_unix.a, with FreeType 2.13.3 built unchanged and merged into it.
#
# The upstream sources are never copied into this repository. This script copies
# dlls/win32u/ (minus main.c, the Windows half) and include/ntuser.h from the given Wine tree
# into <out dir>/work/tree, applies patches/*.patch to that copy in name order (A-* before B-*
# and so on, one or more per spec Part), then adds the files of win32u-unix/ (the new iOS code,
# the wrapper freetype.c and the *_ios.h / *_ios.inc files the patches include) and compiles
# the result. Upstream freetype.c is kept unchanged as freetype_upstream.inc and compiled through
# the wrapper win32u-unix/freetype.c.
#
# Environment:
#   CC, AR, LIBTOOL     compilers/tools (default: xcrun -sdk iphoneos clang / ar / libtool)
#   FREETYPE_SRC        FreeType source tree (default: <repo>/refs/freetype-2.13.3)
#   EXTRA_CFLAGS        appended to every compile

set -u

if [ $# -ne 3 ]; then
  echo "usage: $0 <wine-11.4 tree> <wine build dir> <out dir>" >&2
  exit 2
fi

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

# refs/ is read only: refuse an out dir that is, or would be created, inside it (checked on the
# nearest existing parent, links resolved, before anything is created)
REFS=$(cd "$REPO/refs" 2>/dev/null && pwd -P) || REFS=""
if [ -n "$REFS" ]; then
  p=$3; rest=""
  while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
  case "$(cd "$p" && pwd -P)$rest/" in
    "$REFS/"*) echo "$0: refusing an out dir inside refs/: $3" >&2; exit 1 ;;
  esac
fi

WINE=$(cd "$1" && pwd) || exit 1
BUILD=$(cd "$2" && pwd) || exit 1
mkdir -p "$3" || exit 1
OUT=$(cd "$3" && pwd) || exit 1
MINE="$HERE/win32u-unix"
FT=${FREETYPE_SRC:-"$REPO/refs/freetype-2.13.3"}

WORK="$OUT/work"
TREE="$WORK/tree"
SRC="$TREE/dlls/win32u"
INC="$TREE/include"
OBJ="$OUT/obj"
LOG="$OUT/log"

rm -rf "$WORK" "$OBJ" "$LOG"
mkdir -p "$SRC/dibdrv" "$INC" "$OBJ/win32u" "$OBJ/freetype" "$LOG" || exit 1

# ---------------------------------------------------------------- 1. sources
cp "$WINE"/dlls/win32u/*.c "$WINE"/dlls/win32u/*.h "$SRC"/ || exit 1
cp "$WINE"/dlls/win32u/dibdrv/*.c "$WINE"/dlls/win32u/dibdrv/*.h "$SRC"/dibdrv/ || exit 1
rm -f "$SRC/main.c"                                    # the Windows half
cp "$WINE/include/ntuser.h" "$INC/ntuser.h" || exit 1  # patched for Part F

for p in "$HERE"/patches/*.patch; do
  [ -f "$p" ] || continue
  if patch -p1 -N -s -d "$TREE" < "$p" > "$LOG/patch-$(basename "$p").log" 2>&1; then
    echo "patch $(basename "$p") OK"
  else
    echo "patch $(basename "$p") FAILED"
    cat "$LOG/patch-$(basename "$p").log"
    exit 1
  fi
done

# upstream freetype.c stays unchanged; the wrapper of the same name includes it (Part A4/A5)
mv "$SRC/freetype.c" "$SRC/freetype_upstream.inc" || exit 1
for f in "$MINE"/*; do
  [ -f "$f" ] && { cp "$f" "$SRC"/ || exit 1; }
done

# ---------------------------------------------------------------- 2. tools and flags
CC=${CC:-"xcrun -sdk iphoneos clang"}
AR=${AR:-"xcrun -sdk iphoneos ar"}
LIBTOOL=${LIBTOOL:-"xcrun -sdk iphoneos libtool"}

COMMON="-arch arm64 -miphoneos-version-min=17.0 -O2 -std=gnu11 -fPIC -fno-strict-aliasing \
 -fasynchronous-unwind-tables -fno-omit-frame-pointer -ffixed-x18 ${EXTRA_CFLAGS:-}"
DEFS="-DWINE_IOS=1 -DWINE_UNIX_LIB -D__WINESRC__ -D_WIN32U_ -D_REENTRANT"
WARN="-Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
 -Wno-pragma-pack -Wno-deprecated-declarations -Wno-unknown-pragmas"
# $INC first: the patched ntuser.h replaces the tree's
# spec 17 Part A1 (spec 12 clarification 1): the shared header overrides (wine/unixlib.h,
# winternl.h) before the Wine tree's include/
INCS="-I$SRC -I$INC -I$REPO/out/09-ntdll-processes/include -I$BUILD/include -I$WINE/include -I$FT/include -I$REPO/interfaces"
# w32ios_config.h: the build's config.h, then the optional libraries undefined (Part A3);
# w32ios_rename.h: link-time renames (Part A2) and the per-thread allocation ceiling (Part B1)
FORCE="-include $SRC/w32ios_config.h -include $SRC/w32ios_rename.h"

failed=""

compile() {   # compile <source> <object> <log name> [extra flags...]
  c=$1; o=$2; l=$3; shift 3
  # shellcheck disable=SC2086
  if $CC $COMMON "$@" -c "$c" -o "$o" > "$LOG/$l.log" 2>&1; then
    echo "$l OK"
  else
    echo "$l FAILED"
    failed="$failed $l"
  fi
}

# ---------------------------------------------------------------- 3. FreeType (Part A4)
# Default modules (include/freetype/config/ftmodule.h) and FreeType's default options: zlib is its
# own bundled copy, bzip2/png/harfbuzz/brotli are off. No changes to its sources.
FT_SOURCES="base/ftsystem base/ftinit base/ftdebug base/ftbase base/ftbbox base/ftbdf
 base/ftbitmap base/ftcid base/ftfstype base/ftgasp base/ftglyph base/ftgxval base/ftmm
 base/ftotval base/ftpatent base/ftpfr base/ftstroke base/ftsynth base/fttype1 base/ftwinfnt
 autofit/autofit bdf/bdf bzip2/ftbzip2 cache/ftcache cff/cff cid/type1cid gzip/ftgzip
 lzw/ftlzw pcf/pcf pfr/pfr psaux/psaux pshinter/pshinter psnames/psnames raster/raster
 sdf/sdf sfnt/sfnt smooth/smooth svg/svg truetype/truetype type1/type1 type42/type42
 winfonts/winfnt"
for s in $FT_SOURCES; do
  n="ft-$(echo "$s" | tr '/' '-')"
  compile "$FT/src/$s.c" "$OBJ/freetype/$n.o" "$n" -DFT2_BUILD_LIBRARY "-I$FT/include" -w
done
rm -f "$OUT/libfreetype.a"
# shellcheck disable=SC2086
$AR rcs "$OUT/libfreetype.a" "$OBJ"/freetype/*.o || exit 1

# ---------------------------------------------------------------- 4. win32u
# dibdrv/ has files named like files one level up (bitblt.c, dc.c, objects.c, ...): its objects
# get a "dibdrv-" prefix (Part A1).
for c in "$SRC"/*.c; do
  n=$(basename "$c" .c)
  # shellcheck disable=SC2086
  compile "$c" "$OBJ/win32u/$n.o" "$n" $DEFS $WARN $INCS $FORCE
done
for c in "$SRC"/dibdrv/*.c; do
  n="dibdrv-$(basename "$c" .c)"
  # shellcheck disable=SC2086
  compile "$c" "$OBJ/win32u/$n.o" "$n" $DEFS $WARN $INCS $FORCE
done

# ---------------------------------------------------------------- 4b. stand-in unix tables
# Clarifications 8 and 9: the unix call tables of the libraries in unix-tables/tables.txt
# (opengl32, winspool, netapi32, kerberos, msv1_0), whose upstream unix sides need what iOS
# doesn't have (OpenGL, CUPS, Samba, Kerberos, ntlm_auth) and aren't built. Only their setup
# entries succeed. Each entry list is generated from the given tree's header; the objects go
# into the same library as win32u's.
UT="$HERE/unix-tables"
UT_DEFS="-DWINE_IOS=1 -DWINE_UNIX_LIB -D__WINESRC__ -D_REENTRANT"
UT_INCS="-I$WORK/unix-tables -I$UT -I$REPO/out/09-ntdll-processes/include -I$BUILD/include -I$WINE/include"
mkdir -p "$WORK/unix-tables" || exit 1
while read -r name header enum count setup; do
  case "$name" in ''|'#'*) continue ;; esac
  # shellcheck disable=SC2086
  sh "$UT/gen-entries.sh" "$WINE/$header" "$enum" "$count" "$WORK/unix-tables/${name}_ios_entries.h" $setup || exit 1
  # the library's own directory first: several of them name their header unixlib.h
  # shellcheck disable=SC2086
  compile "$UT/${name}_ios.c" "$OBJ/win32u/${name}_ios.o" "${name}_ios" $UT_DEFS $WARN \
    "-I$WINE/$(dirname "$header")" $UT_INCS < /dev/null
done < "$UT/tables.txt"
for n in unix_tables_log unix_tables_pure; do
  # shellcheck disable=SC2086
  compile "$UT/$n.c" "$OBJ/win32u/$n.o" "$n" $UT_DEFS $WARN $UT_INCS
done

if [ -n "$failed" ]; then
  echo "FAILED:$failed"
  for f in $failed; do echo "==== $f"; head -60 "$LOG/$f.log"; done
  exit 1
fi

# ---------------------------------------------------------------- 5. one library
rm -f "$OUT/libwin32u_unix.a"
# shellcheck disable=SC2086
if ! $LIBTOOL -static -o "$OUT/libwin32u_unix.a" "$OBJ"/win32u/*.o "$OUT/libfreetype.a" 2> "$LOG/libtool.log"; then
  echo "libtool failed, merging with ar"
  # shellcheck disable=SC2086
  $AR rcs "$OUT/libwin32u_unix.a" "$OBJ"/win32u/*.o "$OBJ"/freetype/*.o || exit 1
fi
echo "built $OUT/libwin32u_unix.a"
