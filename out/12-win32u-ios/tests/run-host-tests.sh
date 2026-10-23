#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 12 Part G: builds and runs the host tests of the pure parts (any C11 compiler, no iOS SDK;
# only the unix-tables tests use Wine headers, refs/wine-11.4's), then the App Store check of spec 00
# Part F on this part's sources.
# CC overrides the compiler. Exits non-zero on any failure.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PART="$HERE/.."
SRC="$PART/win32u-unix"
REPO="$PART/../.."
INTERFACES="$REPO/interfaces"
OUT=${TMPDIR:-/tmp}/w32ios-host-tests.$$
CC=${CC:-cc}

$CC -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g \
    -I"$SRC" -I"$INTERFACES" \
    "$HERE/test_w32ios.c" "$SRC/w32ios_pure.c" "$SRC/w32ios_kmt_pure.c" \
    -o "$OUT"
if "$OUT"; then status=0; else status=$?; fi
rm -f "$OUT"
[ $status -eq 0 ] || exit $status

# spec 17 Part J (Parts F and G2)
$CC -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g -I"$SRC" \
    "$HERE/test_w32ios_s17.c" "$SRC/w32ios_s17_pure.c" -o "$OUT.s17"
if "$OUT.s17"; then status=0; else status=$?; fi
rm -f "$OUT.s17"
[ $status -eq 0 ] || exit $status

# spec 22 Part T (Part E): the client-object ownership rule over a fake shared table
$CC -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -O1 -g -I"$SRC" -I"$INTERFACES" \
    "$HERE/test_w32ios_s22.c" "$SRC/w32ios_pure.c" -o "$OUT.s22"
if "$OUT.s22"; then status=0; else status=$?; fi
rm -f "$OUT.s22"
[ $status -eq 0 ] || exit $status

# spec 12 clarifications 8 and 9: the stand-in unix tables of unix-tables/tables.txt, each built
# against its library's real Wine 11.4 header and the Wine headers of refs/ (read only; the
# generated lists go to $OUT.ut), and each listed in spec 09's unixlibs.txt
UT="$PART/unix-tables"
WINE_REF="$REPO/refs/wine-11.4"
UNIXLIBS="$REPO/out/09-ntdll-processes/unixlibs.txt"
mkdir -p "$OUT.ut"
status=0
while read -r name header enum count setup; do
  case "$name" in ''|'#'*) continue ;; esac
  if ! grep -q "^$name\.[a-z]* " "$UNIXLIBS"; then
    echo "$name: not listed in $UNIXLIBS"; status=1; break
  fi
  # shellcheck disable=SC2086
  sh "$UT/gen-entries.sh" "$WINE_REF/$header" "$enum" "$count" "$OUT.ut/${name}_ios_entries.h" $setup
  $CC -std=gnu11 -Wall -Wextra -Werror -O1 -g -DWINE_UNIX_LIB -D__WINESRC__ \
      -DTEST_$(echo "$name" | tr 'a-z' 'A-Z') \
      -I"$OUT.ut" -I"$UT" -I"$WINE_REF/$(dirname "$header")" -I"$REPO/out/09-ntdll-processes/include" \
      -I"$WINE_REF/include" \
      "$HERE/test_unix_tables.c" "$UT/unix_tables_pure.c" "$REPO/out/09-ntdll-processes/ntdll-unix/ios_pure.c" \
      -o "$OUT.ut/test_$name"
  if ! "$OUT.ut/test_$name"; then status=1; break; fi
done < "$UT/tables.txt"
rm -rf "$OUT.ut"
[ $status -eq 0 ] || exit $status

sh "$REPO/check-app-store.sh" "$PART"
