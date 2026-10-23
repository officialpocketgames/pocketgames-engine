#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 Part C: the host tests, with gcc and clang (-Wall -Wextra -Werror); exits non-zero on
# any failure.
#   run-host-tests.sh [build-dir]
# COMPILERS lists the compilers (default "gcc clang", each skipped when missing). WINE_SRC is the
# Wine 11.4 tree (default refs/wine-11.4), WINE_BUILD a configured Wine build directory for the
# PE syntax check (default ~/wine-build). SLOW=1 also runs test-tree.sh (TREE_SCRATCH: where).

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
build=${1:-"$here/build"}
compilers=${COMPILERS:-"gcc clang"}
pe="$top/pe"
incs="-I$pe/dlls/kernelbase -I$pe/dlls/kernel32 -I$pe/dlls/dbghelp -I$pe/dlls/nsi -I$pe/programs/services -I$pe/dlls/d3dcompiler_43"
flags="-std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"

mkdir -p "$build"
status=0
ran=0

fail() {
    echo "FAILED: $*" >&2
    status=1
}

for cc in $compilers; do
    if ! command -v "$cc" >/dev/null 2>&1; then
        echo "SKIPPED: $cc (not installed)"
        continue
    fi
    ran=$((ran + 1))
    out="$build/$cc"
    mkdir -p "$out"

    # Part A's pure code (A1, A3, A6, A9, A10)
    # shellcheck disable=SC2086
    if $cc $flags $incs -o "$out/test_part_a" "$here/test_part_a.c"; then
        "$out/test_part_a" || fail "test_part_a ($cc)"
    else
        fail "build test_part_a ($cc)"
    fi

    # B5.2: pecheck on synthetic images
    # shellcheck disable=SC2086
    if $cc $flags -o "$out/pecheck" "$top/tools/pecheck.c" && $cc $flags -o "$out/mkpe" "$here/mkpe.c"; then
        sh "$here/test-pecheck.sh" "$out/pecheck" "$out/mkpe" "$out/pecheck-farms" || fail "test-pecheck.sh ($cc)"
    else
        fail "build pecheck / mkpe ($cc)"
    fi
done

[ $ran -gt 0 ] || fail "no compiler found (COMPILERS=$compilers)"

# B4 / spec 09 clarification 6: no unix table without its DLL in every farm; and the check fires
sh "$here/test-unixlibs.sh" || fail "test-unixlibs.sh"
mkdir -p "$build"
printf 'kernel32.dll\nnosuch.dll   # a table whose DLL no farm lists\n' > "$build/unixlibs-fixture.txt"
if sh "$here/test-unixlibs.sh" "$build/unixlibs-fixture.txt" > "$build/unixlibs-fixture.out" 2>&1; then
    fail "test-unixlibs.sh accepted a DLL that no farm lists"
elif ! grep -q "nosuch.dll has a unix table (unixlibs.txt) but isn't in farms/arm64ec-windows.txt" "$build/unixlibs-fixture.out" ||
     ! grep -q "winemetal.dll is not in unixlibs.txt (stale line)" "$build/unixlibs-fixture.out"; then
    fail "test-unixlibs.sh on the fixture: $(cat "$build/unixlibs-fixture.out")"
fi

# B7: build-pe.sh plan, real overlays and fixtures
sh "$here/test-plan.sh" "$build/plan" || fail "test-plan.sh"

# B1-B7: the whole flow on a fake Wine tree and toolchain (mkpe from the last compiler)
if [ -x "$build/$cc/mkpe" ]; then
    # its scratch folder is on the local file system (much faster than /mnt/c in WSL)
    sh "$here/test-build-fake.sh" "$build/$cc/mkpe" "${FAKE_SCRATCH:-${TMPDIR:-/tmp}/build-pe-fake.$$}" ||
        fail "test-build-fake.sh"
else
    fail "test-build-fake.sh: no mkpe"
fi

# B2: the tree step for real (copies the Wine tree three times: minutes, so only with SLOW=1;
# TREE_SCRATCH should be on a Linux file system in WSL)
if [ "${SLOW:-0}" = 1 ]; then
    sh "$here/test-tree.sh" "${TREE_SCRATCH:-$build/tree}" || fail "test-tree.sh"
else
    echo "SKIPPED: test-tree.sh (slow; SLOW=1 runs it)"
fi

# Part A's PE files, compiled for the three farms' targets
sh "$here/pe-syntax-check.sh" || fail "pe-syntax-check.sh"

# spec 00 F: no API an App Store build must not use
repo=$(cd "$top/../.." && pwd)
sh "$repo/check-app-store.sh" "$top" || fail "check-app-store.sh"

if [ $status = 0 ]; then echo "run-host-tests.sh: all passed"; else echo "run-host-tests.sh: FAILURES" >&2; fi
exit $status
