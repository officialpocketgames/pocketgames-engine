#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# build.sh <llvm-mingw bin dir> <out dir>
#
# Spec 35 Part J4: builds the Windows test programs (gl-*.c with gltest.c) for i686 and x86-64
# with llvm-mingw 20260421 (the toolchain spec 19's build-pe.sh pins and unpacks: `build-pe.sh
# --work <dir> toolchain`). <out dir> gets i386/ and x86_64/ with one .exe per program and
# SHA256SUMS. The programs run on the device only, in a test build (TESTS.md); never here.

set -eu
[ $# -eq 2 ] || { echo "usage: $0 <llvm-mingw bin dir> <out dir>" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../../.." && pwd)
BIN=$(cd "$1" && pwd)

REFS=$(cd "$REPO/refs" 2>/dev/null && pwd -P) || REFS=""
if [ -n "$REFS" ]; then
  p=$2; rest=""
  while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
  case "$(cd "$p" && pwd -P)$rest/" in
    "$REFS/"*) echo "$0: refusing an output dir inside refs/: $2" >&2; exit 1 ;;
  esac
fi
mkdir -p "$2"
OUT=$(cd "$2" && pwd)

PROGRAMS=$(cd "$HERE" && ls gl-*.c | sed 's/\.c$//')
CFLAGS="-std=gnu11 -O2 -g0 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-cast-function-type"

for arch in i686:i386 x86_64:x86_64; do
  triple=${arch%%:*}-w64-mingw32
  dir=$OUT/${arch#*:}
  cc=$BIN/$triple-clang
  [ -x "$cc" ] || { echo "$0: no $cc" >&2; exit 1; }
  mkdir -p "$dir"
  for p in $PROGRAMS; do
    # shellcheck disable=SC2086
    "$cc" $CFLAGS -mconsole -o "$dir/$p.exe" "$HERE/$p.c" "$HERE/gltest.c" -lopengl32 -lgdi32 -luser32
    echo "build: ${arch#*:}/$p.exe"
  done
  (cd "$dir" && sha256sum ./*.exe > SHA256SUMS 2>/dev/null || shasum -a 256 ./*.exe > SHA256SUMS)
done
echo "build: $(echo "$PROGRAMS" | wc -w | tr -d ' ') programs for i386 and x86_64 in $OUT"
