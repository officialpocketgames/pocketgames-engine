#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 Part J1: the host tests (no GL, no GPU: nothing here creates a GL context or touches a
# graphics driver), then the App Store check of spec 00 Part F on this part.
#   1. pgl's pure functions (D2, D3.2, C3.4, D5.4, H2)                 test_pgl_pure.c
#   2. the WoW64 address conversions (G1)                              test_wow.c
#   3. opengl32's wrappers against Wine 11.4's unixlib.h (B1, E, A4)   test_opengl_ios.c
#   4. the generator's type classification (G1), with Perl            test-wow-classify.pl
#      and its buffer-offset table against the registry (GL_REGISTRY)   test-offset-table.pl
#   5. ANGLE's selector check, with Python 3 (C1.3)                    test_selectors.py
#   6. with WINE_BUILD (a host Wine build dir, ci/wine-host-build.sh): -fsyntax-only of the
#      Wine-dependent files (opengl32's unix_wgl.c, the generated unix_thunks.c and
#      opengl_ios*.c; win32u's opengl_ios.c through out/12-win32u-ios/tests/syntax-check-patched.sh)
#   7. with gl4es's source (GL4ES_SRC): the C3 changes compile, nothing prints   check-gl4es.sh
#   8. with WINE_BUILD and clang: J4's Windows test programs compile         win/check-syntax.sh
# Environment: CC (default cc), WINE_SRC (default <repo>/refs/wine-11.4), WINE_BUILD (optional),
# GL4ES_SRC (default <repo>/refs/gl4es-bc4314d8) and GL_REGISTRY (default
# <repo>/refs/khronos-gl-0b449b9/gl.xml): each skipped when absent.
# Exits non-zero on any failure. Never writes into refs/.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PART=$(cd "$HERE/.." && pwd)
REPO=$(cd "$PART/../.." && pwd)
WINE=${WINE_SRC:-"$REPO/refs/wine-11.4"}
CC=${CC:-cc}
OUT=${TMPDIR:-/tmp}/s35-host-tests.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
WFLAGS="-Wall -Wextra -Werror -Wno-unused-parameter"

run() {   # run <name> <binary>
  if "$2"; then echo "PASS $1"; else echo "FAIL $1"; exit 1; fi
}

# 1
$CC -std=c11 $WFLAGS -O1 -g -I"$PART/pgl" "$HERE/test_pgl_pure.c" "$PART/pgl/pgl_pure.c" -o "$OUT/test_pgl_pure"
run test_pgl_pure "$OUT/test_pgl_pure"
# pgl without a backend (the CI's fallback) implements pgl.h
$CC -std=gnu11 $WFLAGS -O1 -I"$PART/pgl" -c "$PART/pgl/pgl_none.c" -o "$OUT/pgl_none.o"
echo "PASS pgl_none.c compiles against pgl.h"

# 2
$CC -std=c11 $WFLAGS -O1 -g -I"$PART/opengl32-unix" "$HERE/test_wow.c" -o "$OUT/test_wow"
run test_wow "$OUT/test_wow"

# 3: the entry list from Wine's unixlib.h, as build-unix.sh writes it
sh "$PART/opengl32-unix/gen-entries.sh" "$WINE/dlls/opengl32/unixlib.h" "$OUT/opengl_ios_entries.h"
grep -q '^OPENGL_IOS_ENTRY( unix_process_attach )$' "$OUT/opengl_ios_entries.h"
$CC -std=gnu11 $WFLAGS -Wno-missing-field-initializers -Wno-sign-compare -O1 -g \
    -DWINE_UNIX_LIB -D__WINESRC__ \
    -I"$OUT" -I"$PART/opengl32-unix" -I"$PART/pgl" -I"$WINE/dlls/opengl32" \
    -I"$REPO/out/09-ntdll-processes/include" -I"$WINE/include" \
    "$HERE/test_opengl_ios.c" "$PART/opengl32-unix/opengl_ios_tables.c" \
    "$PART/opengl32-unix/opengl_ios_call.c" "$PART/opengl32-unix/opengl_ios_pure.c" \
    -o "$OUT/test_opengl_ios"
run test_opengl_ios "$OUT/test_opengl_ios"

# 4
if command -v perl >/dev/null 2>&1; then
  if perl "$HERE/test-wow-classify.pl" "$WINE/dlls/opengl32/unixlib.h"; then echo "PASS test-wow-classify"
  else echo "FAIL test-wow-classify"; exit 1; fi
  # the generator itself compiles (XML::LibXML stood in for: running it needs the registry, regen.sh)
  mkdir -p "$OUT/perl/XML"
  printf 'package XML::LibXML;\n1;\n' > "$OUT/perl/XML/LibXML.pm"
  if perl -I"$OUT/perl" -c "$PART/pe/dlls/opengl32/make_opengl" 2> "$OUT/make_opengl.log"; then echo "PASS make_opengl compiles"
  else cat "$OUT/make_opengl.log"; echo "FAIL make_opengl compiles"; exit 1; fi
  # the buffer-offset table against the registry (clarification 2.3)
  GL_XML=${GL_REGISTRY:-"$REPO/refs/khronos-gl-0b449b9/gl.xml"}
  if [ -f "$GL_XML" ]; then
    if perl "$HERE/test-offset-table.pl" "$GL_XML"; then echo "PASS test-offset-table"
    else echo "FAIL test-offset-table"; exit 1; fi
  else
    echo "SKIPPED test-offset-table (no gl.xml: GL_REGISTRY)"
  fi
else
  echo "SKIPPED test-wow-classify (no perl)"
fi

# 5
if command -v python3 >/dev/null 2>&1; then
  if python3 -I -B "$HERE/test_selectors.py"; then echo "PASS test_selectors"
  else echo "FAIL test_selectors"; exit 1; fi
else
  echo "SKIPPED test_selectors (no python3)"
fi

# 6
if [ -n "${WINE_BUILD:-}" ] && [ -f "$WINE_BUILD/include/config.h" ]; then
  for f in unix_wgl unix_thunks opengl_ios opengl_ios_call opengl_ios_tables; do
    if $CC -std=gnu11 -fsyntax-only -Wall -Wno-unused-function -Wno-unused-variable -Wno-pragma-pack \
        -Wno-unknown-pragmas -DWINE_IOS=1 -DWINE_UNIX_LIB -D__WINESRC__ -D_OPENGL32_ -D_REENTRANT \
        -I"$OUT" -I"$PART/opengl32-unix" -I"$PART/pgl" -I"$WINE/dlls/opengl32" \
        -I"$REPO/out/09-ntdll-processes/include" -I"$WINE_BUILD/include" -I"$WINE/include" \
        "$PART/opengl32-unix/$f.c" 2> "$OUT/$f.syntax.log"; then
      echo "PASS syntax opengl32-unix/$f.c"
    else
      cat "$OUT/$f.syntax.log"; echo "FAIL syntax opengl32-unix/$f.c"; exit 1
    fi
  done
  if sh "$REPO/out/12-win32u-ios/tests/syntax-check-patched.sh" "$WINE_BUILD" opengl_ios.c w32ios_driver.c; then
    echo "PASS syntax win32u opengl_ios.c, w32ios_driver.c"
  else
    echo "FAIL syntax win32u opengl_ios.c"; exit 1
  fi
else
  echo "SKIPPED syntax checks (no WINE_BUILD)"
fi

# 7: gl4es with the C3 changes compiles (GL4ES_SRC: gl4es at the pinned commit, read only)
GL4ES=${GL4ES_SRC:-"$REPO/refs/gl4es-bc4314d8"}
if [ -f "$GL4ES/src/gl/glstate.c" ]; then
  CC="$CC" sh "$HERE/check-gl4es.sh" "$GL4ES" "$OUT/gl4es"
else
  echo "SKIPPED check-gl4es (no gl4es source: GL4ES_SRC)"
fi

# 8: J4's Windows test programs compile (clang, mingw targets, Wine's headers; WINE_BUILD)
WINE_BUILD="${WINE_BUILD:-}" sh "$HERE/win/check-syntax.sh" "$WINE" "$OUT/win"

sh "$REPO/check-app-store.sh" "$PART"
