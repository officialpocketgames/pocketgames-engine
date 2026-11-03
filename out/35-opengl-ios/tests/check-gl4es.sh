#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# check-gl4es.sh <gl4es source> <work dir>
#
# Spec 35 C3, on the host: copies gl4es at the pinned commit (a read-only tree, refs/) into
# <work dir>, lays this tree's changed files (gl4es/) over it as build-gl4es.sh does, and
# compiles every source of the C2 build (src/CMakeLists.txt's GL_SRC and glx/hardext.c) with
# build-gl4es.sh's options and -fsyntax-only. It proves that the changes compile and that the
# printf redirection of gl4es_ios_config.h goes into every file; the iOS build itself (and its
# export, exit and print checks) runs in the CI. Linux is not Darwin: the __APPLE__ branches
# are not taken here. Environment: CC (default cc).

set -eu
[ $# -eq 2 ] || { echo "usage: $0 <gl4es source> <work dir>" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
PART=$(cd "$HERE/.." && pwd)
CC=${CC:-cc}
src=$(cd "$1" && pwd)
work=$2

[ -f "$src/src/gl/glstate.c" ] || { echo "check-gl4es: $src is not gl4es" >&2; exit 2; }
rm -rf "$work"
mkdir -p "$work"
cp -R "$src/." "$work/"
chmod -R u+w "$work"
(cd "$PART/gl4es" && find . -type f) | while read -r f; do
  mkdir -p "$work/$(dirname "$f")"
  cp "$PART/gl4es/$f" "$work/$f"
done

files=$(sed -n '/^SET(GL_SRC/,/^)/p' "$work/src/CMakeLists.txt" | grep -o 'gl/[A-Za-z0-9_/]*\.c')
files="$files glx/hardext.c"
echo "$files" | grep -q 'gl/gl4es_ios.c' || { echo "FAIL check-gl4es: gl4es_ios.c is not in GL_SRC"; exit 1; }

n=0
for f in $files; do
  if $CC -std=gnu11 -fsyntax-only -w -DGL4ES_IOS -include "$work/src/gl/gl4es_ios_config.h" \
      -DNOX11 -DNOEGL -DNO_LOADER -DNO_GBM -DSTATICLIB -DNO_INIT_CONSTRUCTOR -DDEFAULT_ES=2 -DNDEBUG \
      -I"$work/include" -I"$work/src/util" -I"$work/src/glx" -I"$work/src/gl" \
      "$work/src/$f" 2> "$work/syntax.log"; then
    n=$((n + 1))
  else
    cat "$work/syntax.log"
    echo "FAIL check-gl4es: src/$f"
    exit 1
  fi
done

# the redirection reaches every file: no gl4es source may print once preprocessed (printf,
# fprintf to stdout/stderr, puts, vprintf)
for f in $files; do
  $CC -std=gnu11 -E -w -DGL4ES_IOS -include "$work/src/gl/gl4es_ios_config.h" \
      -DNOX11 -DNOEGL -DNO_LOADER -DNO_GBM -DSTATICLIB -DNO_INIT_CONSTRUCTOR -DDEFAULT_ES=2 -DNDEBUG \
      -I"$work/include" -I"$work/src/util" -I"$work/src/glx" -I"$work/src/gl" \
      "$work/src/$f" 2>/dev/null | grep -v '^#' > "$work/pp.c"
  [ "$f" = gl/gl4es_ios.c ] && continue
  # declarations from stdio.h are "int printf(" etc.; a call is the name, "(" and not a type before it
  if grep -nE '(^|[^A-Za-z0-9_])(printf|puts|vprintf)[[:space:]]*\(' "$work/pp.c" \
      | grep -vE '(int|extern)[[:space:]]+(printf|puts|vprintf)[[:space:]]*\(' > "$work/calls.txt"; then
    head -n 5 "$work/calls.txt"
    echo "FAIL check-gl4es: src/$f still prints after preprocessing"
    exit 1
  fi
done
echo "PASS check-gl4es ($n sources compile with the C3 changes; none prints)"
