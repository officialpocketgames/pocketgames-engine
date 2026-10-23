#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 13 clarification 15: BearSSL and LibTomCrypt built for the host, for the host tests.
#
# usage: tests/host-deps.sh <cache dir> <compiler>
#
# Copies refs/bearssl-7bea48e/ and refs/wine-11.4/libs/tomcrypt/ out of refs/ (which is never
# written: CLAUDE.md) into <cache dir>/<compiler>/src/, compiles BearSSL's src/**/*.c and the
# LibTomCrypt list of build-crypto-deps-ios.sh with Wine's defines, and leaves
#   <cache dir>/<compiler>/lib/libbearssl.a, lib/libtomcrypt.a
#   <cache dir>/<compiler>/include/bearssl/*.h, include/tomcrypt/*.h
# A cache dir already holding both archives for the same compiler and file list is reused.
# Prints the <cache dir>/<compiler> directory on its last line.

set -u
[ $# -eq 2 ] || { echo "usage: $0 <cache dir> <compiler>" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
REPO=$(cd "$TOP/../.." && pwd)
REFS=$(cd "$REPO/refs" && pwd -P) || { echo "no refs/" >&2; exit 1; }
CCH=$2
mkdir -p "$1" || exit 1
CACHE=$(cd "$1" && pwd -P)
case "$CACHE/" in "$REFS"/*) echo "refused: cache dir in refs/" >&2; exit 1 ;; esac
D="$CACHE/$(basename "$CCH")"

list=$(sh "$TOP/build-crypto-deps-ios.sh" --print-tomcrypt-files)
key=$(printf '%s\n%s\n' "$($CCH --version 2>/dev/null | head -n 1)" "$list" | cksum | awk '{print $1}')
if [ -f "$D/lib/libbearssl.a" ] && [ -f "$D/lib/libtomcrypt.a" ] && [ "$(cat "$D/.key" 2>/dev/null)" = "$key" ]; then
  echo "$D"
  exit 0
fi

rm -rf "$D"
mkdir -p "$D/src" "$D/obj/bearssl" "$D/obj/tomcrypt" "$D/lib" "$D/include/bearssl" "$D/include/tomcrypt" || exit 1
cp -R "$REFS/bearssl-7bea48e" "$D/src/bearssl" || exit 1
cp -R "$REFS/wine-11.4/libs/tomcrypt" "$D/src/tomcrypt" || exit 1

FLAGS="-O2 -fPIC -w"
fail=0
( cd "$D/src/bearssl" && find src -name '*.c' | LC_ALL=C sort ) > "$D/bearssl-files.txt"
while read -r f; do
  # shellcheck disable=SC2086
  $CCH $FLAGS -I"$D/src/bearssl/inc" -I"$D/src/bearssl/src" -DBR_USE_GETENTROPY=0 -DBR_USE_URANDOM=0 \
    -c "$D/src/bearssl/$f" -o "$D/obj/bearssl/$(echo "$f" | tr / _ | sed 's/\.c$/.o/')" || { echo "FAILED $f" >&2; fail=1; }
done < "$D/bearssl-files.txt"
for f in $list; do
  # shellcheck disable=SC2086
  $CCH $FLAGS -I"$D/src/tomcrypt/src/headers" -include "$TOP/include/ios_tomcrypt_rename.h" \
    -DLTC_NO_PROTOTYPES -DLTC_SOURCE -DLTC_NO_TEST -DMP_32BIT= \
    -c "$D/src/tomcrypt/$f" -o "$D/obj/tomcrypt/$(echo "$f" | tr / _ | sed 's/\.c$/.o/')" || { echo "FAILED $f" >&2; fail=1; }
done
[ $fail -eq 0 ] || exit 1
ar rcs "$D/lib/libbearssl.a" "$D"/obj/bearssl/*.o || exit 1
ar rcs "$D/lib/libtomcrypt.a" "$D"/obj/tomcrypt/*.o || exit 1
cp "$D/src/bearssl/inc/"*.h "$D/include/bearssl/" && cp "$D/src/tomcrypt/src/headers/"*.h "$D/include/tomcrypt/" || exit 1
echo "$key" > "$D/.key"
echo "$D"
