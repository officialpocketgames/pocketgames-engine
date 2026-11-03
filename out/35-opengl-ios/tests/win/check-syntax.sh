#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# check-syntax.sh <wine-11.4 tree> <work dir>
#
# Spec 35 Part J4, on the host: -fsyntax-only (-Wall -Wextra -Werror) of every Windows test
# program with clang for i686-w64-mingw32 and x86_64-w64-mingw32, against Wine 11.4's Windows
# and C runtime headers. The CI builds and links them with llvm-mingw (build.sh); nothing here
# runs them. Wine declares the GL 1.1 API in wine/wgl.h: <work dir>/GL/gl.h includes it.
# Environment: CLANG (default clang), WINE_BUILD (a host Wine build, ci/wine-host-build.sh: its
# include/ has the headers widl generates, such as wtypes.h; skipped without it).

set -eu
[ $# -eq 2 ] || { echo "usage: $0 <wine-11.4 tree> <work dir>" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
WINE=$(cd "$1" && pwd)
CLANG=${CLANG:-clang}
command -v "$CLANG" >/dev/null 2>&1 || { echo "SKIPPED check-syntax (no clang)"; exit 0; }
[ -n "${WINE_BUILD:-}" ] && [ -f "$WINE_BUILD/include/wtypes.h" ] || { echo "SKIPPED check-syntax (no WINE_BUILD)"; exit 0; }
rm -rf "$2"
mkdir -p "$2/GL"
WORK=$(cd "$2" && pwd)
printf '%s\n' '/* check-syntax.sh: Wine 11.4 declares the GL 1.1 API here */' '#include <wine/wgl.h>' > "$WORK/GL/gl.h"
RES=$("$CLANG" -print-resource-dir)

n=0
for t in i686-w64-mingw32 x86_64-w64-mingw32; do
  for f in "$HERE"/gltest.c "$HERE"/gl-*.c; do
    if "$CLANG" --target=$t -fsyntax-only -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter \
        -nostdinc -isystem "$RES/include" -I"$WORK" -I"$WINE/include/msvcrt" -I"$WINE_BUILD/include" -I"$WINE/include" \
        "$f" 2> "$WORK/log"; then
      n=$((n + 1))
    else
      cat "$WORK/log"
      echo "FAIL check-syntax: $(basename "$f") for $t"
      exit 1
    fi
  done
done
echo "PASS check-syntax (J4: $n compilations, i686 and x86_64, against Wine 11.4's headers)"
