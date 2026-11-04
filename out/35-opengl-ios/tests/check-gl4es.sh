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
# export, exit and print checks) runs in the CI. Linux is not Darwin: with clang, a second pass
# for a Darwin target (the __APPLE__ branches, and what Darwin rejects) uses the host's C headers,
# so it proves no more than that. Environment: CC (default cc).

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

# C2.1: the functions the pre-link keeps global (gl4esinit.h's four, gl4es_ios.h's) are defined
# once and not hidden, compiled as build-gl4es.sh compiles them (-fvisibility=hidden); all of
# them checked before failing, as ld -r on macOS stops at the first hidden one
exports="initialize_gl4es set_getprocaddress set_getmainfbsize gl4es_GetProcAddress
$(grep -oE 'gl4es_ios_[A-Za-z0-9_]+ *\(' "$PART/gl4es/include/gl4es_ios.h" | tr -d ' (' | sort -u)"
if command -v readelf > /dev/null 2>&1; then
  mkdir -p "$work/objs"
  for f in $files; do
    if ! $CC -std=gnu11 -c -O0 -w -fvisibility=hidden -DGL4ES_IOS -include "$work/src/gl/gl4es_ios_config.h" \
        -DNOX11 -DNOEGL -DNO_LOADER -DNO_GBM -DSTATICLIB -DNO_INIT_CONSTRUCTOR -DDEFAULT_ES=2 -DNDEBUG \
        -I"$work/include" -I"$work/src/util" -I"$work/src/glx" -I"$work/src/gl" \
        -o "$work/objs/$(echo "$f" | tr '/' '_').o" "$work/src/$f" 2> "$work/compile.log"; then
      cat "$work/compile.log"
      echo "FAIL check-gl4es: src/$f doesn't compile to an object"
      exit 1
    fi
  done
  bad=0
  nexp=0
  for s in $exports; do
    nexp=$((nexp + 1))
    vis=$(for o in "$work"/objs/*.o; do readelf -sW "$o" | awk -v s="$s" '$8 == s && $7 != "UND" { print $6 }'; done)
    case "$vis" in
      DEFAULT) ;;
      "") echo "FAIL check-gl4es: $s is not defined (C2.1)"; bad=1 ;;
      *"
"*) echo "FAIL check-gl4es: $s is defined more than once (C2.1)"; bad=1 ;;
      *) echo "FAIL check-gl4es: $s is $vis; the pre-link can't export it (C2.1)"; bad=1 ;;
    esac
  done
  [ $bad -eq 0 ] || exit 1
  exported="; the $nexp exported functions visible"
else
  exported=""
  echo "SKIPPED check-gl4es export visibility (no readelf)"
fi

# Darwin rejects some constructs whatever the SDK (C2: __attribute__((alias)), "aliases are not
# supported on darwin"). With clang, every source again for a Darwin target of the host's
# architecture, against the host's C headers; only errors in gl4es's own files count (glibc's
# headers aren't made for a Darwin target: its nullability macros clash, __float128 fails).
darwin=""
if $CC --version 2>/dev/null | grep -qi clang; then
  arch=$(uname -m)
  case "$arch" in aarch64) darch=arm64 ;; *) darch=$arch ;; esac
  res=$($CC -print-resource-dir)/include
  # build-gl4es.sh's definitions: no NO_LOADER, which loader.h defines itself on Apple targets
  dflags="-DGL4ES_IOS -DNOX11 -DNOEGL -DNO_GBM -DSTATICLIB -DNO_INIT_CONSTRUCTOR -DDEFAULT_ES=2 -DNDEBUG"
  for f in $files; do
    # shellcheck disable=SC2086
    $CC --target="$darch-apple-macos13" -U__nonnull -U__nullable -nostdinc -isystem "$res" \
        -isystem "/usr/include/$arch-linux-gnu" -isystem /usr/include \
        -std=gnu11 -fsyntax-only -w $dflags -include "$work/src/gl/gl4es_ios_config.h" \
        -I"$work/include" -I"$work/src/util" -I"$work/src/glx" -I"$work/src/gl" \
        "$work/src/$f" > "$work/darwin.log" 2>&1 || true
    if grep -E "^$work/.*error" "$work/darwin.log" > "$work/darwin-errors.txt"; then
      head -n 5 "$work/darwin-errors.txt"
      echo "FAIL check-gl4es: src/$f doesn't compile for a Darwin target"
      exit 1
    fi
  done
  # and the loader is off all the same (loader.c's NO_LOADER branch: no library handles)
  # shellcheck disable=SC2086
  if ! $CC --target="$darch-apple-macos13" -U__nonnull -U__nullable -nostdinc -isystem "$res" \
        -isystem "/usr/include/$arch-linux-gnu" -isystem /usr/include \
        -std=gnu11 -E -w $dflags -include "$work/src/gl/gl4es_ios_config.h" \
        -I"$work/include" -I"$work/src/util" -I"$work/src/glx" -I"$work/src/gl" \
        "$work/src/gl/loader.c" 2>/dev/null | grep -qF 'void *gles = (void*)(~(uintptr_t)0)'; then
    echo "FAIL check-gl4es: without NO_LOADER the loader is on for a Darwin target (loader.c)"
    exit 1
  fi
  darwin="; for $darch-apple-macos13 too"
else
  echo "SKIPPED check-gl4es Darwin pass (needs clang; CC=$CC)"
fi

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
echo "PASS check-gl4es ($n sources compile with the C3 changes$darwin$exported; none prints)"
