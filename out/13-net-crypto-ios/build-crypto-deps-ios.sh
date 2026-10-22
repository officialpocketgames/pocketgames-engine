#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 13 clarification 7.1: the two third-party static libraries of bcrypt's, secur32's and
# crypt32's unix sides, for iOS arm64 (minimum iOS 17.0, -O2), installed under <prefix>:
#
#   lib/libbearssl.a        BearSSL (MIT) at commit 7bea48e5e850ab4cafbe68d3765cdaba13a86d6f,
#                           headers in include/bearssl/
#   lib/libtomcrypt_unix.a  LibTomCrypt 1.18.2 + LibTomMath (public domain / WTFPL, Unlicense) as
#                           Wine 11.4 bundles them in libs/tomcrypt/: the files DH, DSA and SHA-1
#                           need (the list below), headers in include/tomcrypt/
#
# usage: build-crypto-deps-ios.sh <prefix> [<wine-11.4 tree>]
#
#   <wine-11.4 tree>  default: <repo>/refs/wine-11.4 (only its libs/tomcrypt/ is read)
#
# BearSSL is cloned from https://www.bearssl.org/git/BearSSL into <prefix>/.src/BearSSL (a clone
# already there is reused) and checked out at the commit above; the build refuses unless
# `git rev-parse HEAD` prints exactly that hash. Every src/**/*.c is compiled with -Iinc -Isrc and
# -DBR_USE_GETENTROPY=0 -DBR_USE_URANDOM=0: no system seeder, every engine and DRBG is seeded
# from SecRandomCopyBytes (clarification 8.7). BearSSL's Makefile and T0Comp.exe are not used:
# the generated sources are in its tree.
#
# Nothing is built or written inside refs/ (CLAUDE.md): LibTomCrypt is copied to
# <prefix>/.src/tomcrypt first, and a prefix or work path that resolves into refs/ is refused.
# A finished library is not built again (remove <prefix>/.stamp-<name> to rebuild). Logs:
# <prefix>/.build/*.log.
#
# Environment: CC (default: xcrun -sdk iphoneos clang), AR (default: xcrun -sdk iphoneos ar),
#              BEARSSL_GIT (default: the URL above), JOBS (default: hw.ncpu or 4)

set -u

BEARSSL_COMMIT=7bea48e5e850ab4cafbe68d3765cdaba13a86d6f

# The LibTomCrypt files bcrypt's DH and DSA need (dh_set_pg, dh_set_pg_groupsize, dh_set_key,
# dh_generate_key, dh_export_key, dh_shared_secret, dsa_make_key, dsa_generate_pqg, dsa_set_pqg,
# dsa_set_key, dsa_sign_hash_raw, dsa_verify_hash_raw) and everything they reference: the
# closure computed from Wine's libs/tomcrypt with its defines. hash_memory, which
# dsa_generate_pqg calls and Wine's copy doesn't have, is in common/ios_ltc.c (so it is the one
# name the archive may leave undefined besides libSystem's).
# BEGIN TOMCRYPT FILES
TOMCRYPT_FILES="
mpi.c
src/hashes/sha1.c
src/hashes/sha2/sha256.c
src/hashes/sha2/sha384.c
src/hashes/sha2/sha512.c
src/math/ltm_desc.c
src/math/multi.c
src/math/rand_bn.c
src/math/rand_prime.c
src/misc/crypt/crypt_hash_descriptor.c
src/misc/crypt/crypt_hash_is_valid.c
src/misc/crypt/crypt_ltc_mp_descriptor.c
src/misc/crypt/crypt_prng_descriptor.c
src/misc/crypt/crypt_prng_is_valid.c
src/misc/crypt/crypt_register_hash.c
src/misc/crypt/crypt_register_prng.c
src/misc/crypt/crypt_find_hash.c
src/misc/crypt/crypt_find_prng.c
src/misc/zeromem.c
src/pk/asn1/der/bit/der_decode_bit_string.c
src/pk/asn1/der/bit/der_decode_raw_bit_string.c
src/pk/asn1/der/bit/der_encode_bit_string.c
src/pk/asn1/der/bit/der_encode_raw_bit_string.c
src/pk/asn1/der/bit/der_length_bit_string.c
src/pk/asn1/der/boolean/der_decode_boolean.c
src/pk/asn1/der/boolean/der_encode_boolean.c
src/pk/asn1/der/boolean/der_length_boolean.c
src/pk/asn1/der/choice/der_decode_choice.c
src/pk/asn1/der/generalizedtime/der_decode_generalizedtime.c
src/pk/asn1/der/generalizedtime/der_encode_generalizedtime.c
src/pk/asn1/der/generalizedtime/der_length_generalizedtime.c
src/pk/asn1/der/ia5/der_decode_ia5_string.c
src/pk/asn1/der/ia5/der_encode_ia5_string.c
src/pk/asn1/der/ia5/der_length_ia5_string.c
src/pk/asn1/der/integer/der_decode_integer.c
src/pk/asn1/der/integer/der_encode_integer.c
src/pk/asn1/der/integer/der_length_integer.c
src/pk/asn1/der/object_identifier/der_decode_object_identifier.c
src/pk/asn1/der/object_identifier/der_encode_object_identifier.c
src/pk/asn1/der/object_identifier/der_length_object_identifier.c
src/pk/asn1/der/octet/der_decode_octet_string.c
src/pk/asn1/der/octet/der_encode_octet_string.c
src/pk/asn1/der/octet/der_length_octet_string.c
src/pk/asn1/der/printable_string/der_decode_printable_string.c
src/pk/asn1/der/printable_string/der_encode_printable_string.c
src/pk/asn1/der/printable_string/der_length_printable_string.c
src/pk/asn1/der/sequence/der_decode_sequence_ex.c
src/pk/asn1/der/sequence/der_encode_sequence_ex.c
src/pk/asn1/der/sequence/der_encode_sequence_multi.c
src/pk/asn1/der/sequence/der_length_sequence.c
src/pk/asn1/der/set/der_encode_set.c
src/pk/asn1/der/set/der_encode_setof.c
src/pk/asn1/der/short_integer/der_decode_short_integer.c
src/pk/asn1/der/short_integer/der_encode_short_integer.c
src/pk/asn1/der/short_integer/der_length_short_integer.c
src/pk/asn1/der/teletex_string/der_decode_teletex_string.c
src/pk/asn1/der/teletex_string/der_length_teletex_string.c
src/pk/asn1/der/utctime/der_decode_utctime.c
src/pk/asn1/der/utctime/der_encode_utctime.c
src/pk/asn1/der/utctime/der_length_utctime.c
src/pk/asn1/der/utf8/der_decode_utf8_string.c
src/pk/asn1/der/utf8/der_encode_utf8_string.c
src/pk/asn1/der/utf8/der_length_utf8_string.c
src/pk/dh/dh.c
src/pk/dh/dh_check_pubkey.c
src/pk/dh/dh_export_key.c
src/pk/dh/dh_free.c
src/pk/dh/dh_generate_key.c
src/pk/dh/dh_set.c
src/pk/dh/dh_shared_secret.c
src/pk/dsa/dsa_free.c
src/pk/dsa/dsa_generate_key.c
src/pk/dsa/dsa_generate_pqg.c
src/pk/dsa/dsa_make_key.c
src/pk/dsa/dsa_set.c
src/pk/dsa/dsa_sign_hash.c
src/pk/dsa/dsa_verify_hash.c
src/pk/dsa/dsa_verify_key.c
src/pk/ecc/ltc_ecc_map.c
src/pk/ecc/ltc_ecc_mul2add.c
src/pk/ecc/ltc_ecc_mulmod_timing.c
src/pk/ecc/ltc_ecc_points.c
src/pk/ecc/ltc_ecc_projective_add_point.c
src/pk/ecc/ltc_ecc_projective_dbl_point.c
src/pk/rsa/rsa_exptmod.c
src/pk/rsa/rsa_free.c
src/pk/rsa/rsa_make_key.c
"
# END TOMCRYPT FILES

# Wine's libs/tomcrypt defines (Makefile.in's EXTRADEFS and configure's TOMCRYPT_PE_CFLAGS)
# minus the PE-only -D_ACRTIMP=
TOMCRYPT_DEFS="-DLTC_NO_PROTOTYPES -DLTC_SOURCE -DLTC_NO_TEST -DMP_32BIT="

die() { echo "build-crypto-deps-ios: $*" >&2; exit 1; }

# refuse a path that resolves into refs/ (CLAUDE.md: refs/ is read-only)
not_in_refs() {
  case "$1/" in
    "$REFS"/*) die "refused: $2 resolves into refs/ ($1)" ;;
  esac
}

if [ "${1:-}" = --print-tomcrypt-files ]; then
  # tests/host-deps.sh builds the same list for the host
  printf '%s\n' $TOMCRYPT_FILES
  exit 0
fi

[ $# -ge 1 ] && [ $# -le 2 ] || { echo "usage: $0 <prefix> [<wine-11.4 tree>]" >&2; exit 2; }

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
REFS=$(cd "$REPO/refs" 2>/dev/null && pwd -P) || REFS="$REPO/refs"
mkdir -p "$1" || exit 1
PREFIX=$(cd "$1" && pwd -P)
not_in_refs "$PREFIX" "<prefix>"
WINE=$(cd "${2:-$REPO/refs/wine-11.4}" 2>/dev/null && pwd -P) || die "no wine-11.4 tree"
[ -f "$WINE/libs/tomcrypt/mpi.c" ] || die "no libs/tomcrypt in $WINE"

SRC="$PREFIX/.src"
WORK="$PREFIX/.build"
mkdir -p "$SRC" "$WORK" "$PREFIX/lib" "$PREFIX/include" || exit 1
JOBS=${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
CC=${CC:-"xcrun -sdk iphoneos clang"}
AR=${AR:-"xcrun -sdk iphoneos ar"}
CFLAGS="-arch arm64 -miphoneos-version-min=17.0 -O2 -fPIC"

# compile_all <log> <obj dir> <flags> <src dir> <files...>: a few compiles at a time
compile_all() {
  log=$1; obj=$2; flags=$3; dir=$4; shift 4
  : > "$log"
  n=0
  for f in "$@"; do
    o="$obj/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')"
    # shellcheck disable=SC2086
    ( $CC $CFLAGS $flags -c "$dir/$f" -o "$o" >> "$log" 2>&1 || echo "FAILED $f" >> "$log" ) &
    n=$((n + 1))
    [ $((n % JOBS)) -eq 0 ] && wait
  done
  wait
  ! grep -q '^FAILED' "$log"
}

# ---------------------------------------------------------------- BearSSL
build_bearssl() {
  if [ ! -d "$SRC/BearSSL/.git" ]; then
    git clone --quiet "${BEARSSL_GIT:-https://www.bearssl.org/git/BearSSL}" "$SRC/BearSSL" || return 1
  fi
  ( cd "$SRC/BearSSL" && { git checkout --quiet "$BEARSSL_COMMIT" 2>/dev/null ||
      { git fetch --quiet origin && git checkout --quiet "$BEARSSL_COMMIT"; }; } ) || return 1
  head=$(cd "$SRC/BearSSL" && git rev-parse HEAD)
  [ "$head" = "$BEARSSL_COMMIT" ] || { echo "BearSSL HEAD is $head, not $BEARSSL_COMMIT: refused"; return 1; }
  rm -rf "$WORK/bearssl-obj"; mkdir -p "$WORK/bearssl-obj"
  files=$(cd "$SRC/BearSSL" && find src -name '*.c' | LC_ALL=C sort)
  # shellcheck disable=SC2086
  compile_all "$WORK/bearssl-cc.log" "$WORK/bearssl-obj" \
    "-I$SRC/BearSSL/inc -I$SRC/BearSSL/src -DBR_USE_GETENTROPY=0 -DBR_USE_URANDOM=0" \
    "$SRC/BearSSL" $files || { grep FAILED "$WORK/bearssl-cc.log"; return 1; }
  rm -f "$PREFIX/lib/libbearssl.a"
  # shellcheck disable=SC2086
  $AR rcs "$PREFIX/lib/libbearssl.a" "$WORK/bearssl-obj"/*.o || return 1
  rm -rf "$PREFIX/include/bearssl"; mkdir -p "$PREFIX/include/bearssl"
  cp "$SRC/BearSSL/inc/"*.h "$PREFIX/include/bearssl/" || return 1
  cp "$SRC/BearSSL/LICENSE.txt" "$PREFIX/include/bearssl/LICENSE.txt" || return 1
}

# ---------------------------------------------------------------- LibTomCrypt
build_tomcrypt() {
  rm -rf "$SRC/tomcrypt"
  cp -R "$WINE/libs/tomcrypt" "$SRC/tomcrypt" || return 1
  rm -rf "$WORK/tomcrypt-obj"; mkdir -p "$WORK/tomcrypt-obj"
  # shellcheck disable=SC2086
  compile_all "$WORK/tomcrypt-cc.log" "$WORK/tomcrypt-obj" \
    "-I$SRC/tomcrypt/src/headers -include $HERE/include/ios_tomcrypt_rename.h $TOMCRYPT_DEFS" \
    "$SRC/tomcrypt" $TOMCRYPT_FILES || { grep FAILED "$WORK/tomcrypt-cc.log"; return 1; }
  rm -f "$PREFIX/lib/libtomcrypt_unix.a"
  # shellcheck disable=SC2086
  $AR rcs "$PREFIX/lib/libtomcrypt_unix.a" "$WORK/tomcrypt-obj"/*.o || return 1
  rm -rf "$PREFIX/include/tomcrypt"; mkdir -p "$PREFIX/include/tomcrypt"
  cp "$SRC/tomcrypt/src/headers/"*.h "$SRC/tomcrypt/LICENSE" "$PREFIX/include/tomcrypt/" || return 1
  # the archive must leave no LibTomCrypt / LibTomMath name undefined
  NM=${NM:-"xcrun -sdk iphoneos nm"}
  # shellcheck disable=SC2086
  $NM -g -U "$PREFIX/lib/libtomcrypt_unix.a" 2>/dev/null | awk 'NF >= 3 {print $3}' | sort -u > "$WORK/tomcrypt-defined.txt"
  left=$($NM -g -u "$PREFIX/lib/libtomcrypt_unix.a" 2>/dev/null |
         awk 'NF == 1 && $1 !~ /:$/ {print $1} NF == 2 && $1 == "U" {print $2}' | sort -u |
         comm -23 - "$WORK/tomcrypt-defined.txt" | grep -E '^_?(ltc_|mp_|der_|s_mp_)' || true)
  [ -z "$left" ] || { echo "libtomcrypt_unix.a leaves these undefined: $left"; return 1; }
}

stage() {
  if [ -f "$PREFIX/.stamp-$1" ]; then
    echo "$1: already built (remove $PREFIX/.stamp-$1 to rebuild)"
    return 0
  fi
  echo "$1: building (log: $WORK/$1.log)"
  if "build_$1" > "$WORK/$1.log" 2>&1; then
    touch "$PREFIX/.stamp-$1"
    echo "$1: done"
  else
    tail -n 40 "$WORK/$1.log"
    die "$1 failed; see $WORK/$1.log"
  fi
}

stage bearssl
stage tomcrypt

echo "archives:"
for a in libbearssl.a libtomcrypt_unix.a; do
  [ -f "$PREFIX/lib/$a" ] || die "missing $PREFIX/lib/$a"
  ls -l "$PREFIX/lib/$a"
done
