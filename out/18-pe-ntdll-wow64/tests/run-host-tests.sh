#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 18 Part L: host tests of the pure logic of ntdll's Windows side, wow64 and wow64win.
# Builds every test with gcc and clang (-Wall -Wextra -Werror, -O2, ASan/UBSan where available)
# and runs it. Exits non-zero on any failure. WINE_BUILD is accepted for symmetry with the other
# parts' runners; these tests need no Wine build (the pure files include no Windows headers).

set -eu

here=$(cd "$(dirname "$0")" && pwd)
work=${TMPDIR:-/tmp}/myiosdeck-spec18-tests.$$
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT INT TERM

tests="test_arm64ec_loader test_unwind test_misc test_wow64"
compilers=""
for cc in gcc clang; do
    if command -v "$cc" >/dev/null 2>&1; then compilers="$compilers $cc"; fi
done
if [ -z "$compilers" ]; then
    echo "no C compiler found" >&2
    exit 1
fi

failed=0
for cc in $compilers; do
    for t in $tests; do
        for mode in plain sanitize; do
            flags="-std=gnu11 -O2 -Wall -Wextra -Werror"
            if [ "$mode" = sanitize ]; then
                flags="$flags -fsanitize=address,undefined -fno-sanitize-recover=undefined"
            fi
            out="$work/$t-$cc-$mode"
            # shellcheck disable=SC2086
            if ! "$cc" $flags -o "$out" "$here/$t.c"; then
                echo "FAIL: $t ($cc, $mode) did not build" >&2
                failed=1
                continue
            fi
            if "$out" >"$out.log" 2>&1; then
                echo "ok   $t ($cc, $mode): $(tail -n 1 "$out.log")"
            else
                echo "FAIL: $t ($cc, $mode)" >&2
                cat "$out.log" >&2
                failed=1
            fi
        done
    done
done

# spec 00 Part F: no forbidden API in this part's sources
if ! sh "$here/../../../check-app-store.sh" "$here/.."; then
    failed=1
fi

if [ "$failed" -ne 0 ]; then
    echo "spec 18 host tests: FAILED" >&2
    exit 1
fi
echo "spec 18 host tests: all passed"
