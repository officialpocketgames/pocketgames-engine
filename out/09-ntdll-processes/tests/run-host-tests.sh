#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Specs 09 and 10 Part G, spec 11 Part E: builds and runs the host tests of the pure parts (any host with a C11
# compiler and pthreads; CC overrides the compiler).

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC="$HERE/../ntdll-unix"
CC=${CC:-cc}
OUT=${TMPDIR:-/tmp}/test_ios_pure.$$
status=0

run() {
    name=$1; shift
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -pthread -I"$SRC" "$@" -o "$OUT.$name"
    echo "== $name"
    "$OUT.$name" || status=$?
    rm -f "$OUT.$name"
}

run test_ios_pure    "$SRC/ios_pure.c"    "$HERE/test_ios_pure.c"
run test_ios_vm_pure "$SRC/ios_vm_pure.c" "$HERE/test_ios_vm_pure.c"
# spec 22 Part C: later programs in a kept session
run test_session_pure "$SRC/ios_pure.c" "$SRC/ios_sig_pure.c" "$SRC/ios_session_pure.c" "$HERE/test_session_pure.c"
# spec 09 Parts A and C2: boot failures come back by longjmp, as build-ios.sh compiles ntdll
run test_noexit -DWINE_IOS -include "$SRC/ios_noexit.h" "$SRC/ios_pure.c" "$HERE/test_noexit.c"

# spec 09 clarification 6: build-ios.sh's exports.txt (write-exports.sh) names exactly what the
# loader looks up (ios_unixlib_symbol)
echo "== exports.txt"
$CC -std=c11 -O2 -Wall -Wextra -Werror -I"$SRC" "$SRC/ios_pure.c" "$HERE/print_unixlib_names.c" -o "$OUT.names"
"$OUT.names" "$HERE/../unixlibs.txt" > "$OUT.loader"
sh "$HERE/../write-exports.sh" "$HERE/../unixlibs.txt" > "$OUT.script"
if [ -s "$OUT.loader" ] && cmp -s "$OUT.loader" "$OUT.script"; then
    echo "$(wc -l < "$OUT.script") names, write-exports.sh matches the loader's rule"
else
    echo "FAIL: write-exports.sh and ios_unixlib_symbol disagree:"
    diff "$OUT.loader" "$OUT.script" || true
    status=1
fi
rm -f "$OUT.names" "$OUT.loader" "$OUT.script"
# spec 11 Part E
for t in test_sig_ctx test_sig_exc test_sig_storm test_sig_threadreg test_sig_a64; do
    run $t "$SRC/ios_sig_pure.c" "$HERE/$t.c"
done

# spec 17 Part J: these include Wine's headers, this tree's overrides first (Part A1).
# WINE_SRC overrides the upstream tree (default refs/wine-11.4 of this repository).
WINE_SRC=${WINE_SRC:-"$HERE/../../../refs/wine-11.4"}
WINE_DEFS="-DWINE_UNIX_LIB -D__WINESRC__ -I$HERE/../include -I$WINE_SRC/include"
if [ -f "$WINE_SRC/include/winternl.h" ]; then
    # shellcheck disable=SC2086
    run test_s17_headers_linked $WINE_DEFS -DWINE_IOS -DTEST_LINKED -std=gnu11 "$HERE/test_s17_headers.c"
    # shellcheck disable=SC2086
    run test_s17_headers_weak   $WINE_DEFS -DWINE_IOS -std=gnu11 "$HERE/test_s17_headers.c"
    # shellcheck disable=SC2086
    run test_s17_headers_plain  $WINE_DEFS -std=gnu11 "$HERE/test_s17_headers.c"
    # shellcheck disable=SC2086
    run test_s17 $WINE_DEFS -DWINE_IOS -std=gnu11 "$SRC/ios_s17_pure.c" "$SRC/ios_s17.c" "$HERE/test_s17.c"
    # the override on its own, as the server includes it (no WINE_UNIX_LIB)
    run test_s17_winternl_alone -D__WINESRC__ -I"$HERE/../include" -I"$WINE_SRC/include" -std=gnu11 \
        "$HERE/test_s17_winternl_alone.c"
else
    echo "FAIL: no Wine tree at $WINE_SRC (set WINE_SRC)"
    status=1
fi
exit $status
