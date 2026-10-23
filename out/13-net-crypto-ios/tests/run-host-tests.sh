#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 13 Part K: build and run the host tests on the build machine (Linux/WSL or macOS),
# then check-app-store.sh over the part's sources (spec 00 F).
#
# usage: tests/run-host-tests.sh [<wine-11.4 tree> <wine build dir>]
#
# The pure tests need only a C compiler and build with -Wall -Wextra -Werror. With a Wine tree
# and a configured build dir, the dnsapi and NSI dispatcher tests also compile the modified
# upstream files against the Wine headers (upstream code: warnings not errors there) and check
# them end to end with fakes.
#
# Environment: HOSTCC (default: $CC, else cc), TMPDIR

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
HOSTCC=${HOSTCC:-${CC:-cc}}
WORK=${TMPDIR:-/tmp}/netcrypto-host-tests.$$
mkdir -p "$WORK" || exit 1
trap 'rm -rf "$WORK"' EXIT

CFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-function -I$TOP/include -I$HERE"
failed=0
# dlopen lives in libdl on older glibc; macOS and newer glibc accept -ldl too
DLLIB=-ldl

# run_c <name> <sources...>: compile a C test program and run it
run_c() {
  name=$1; shift
  # shellcheck disable=SC2086
  if ! $HOSTCC $CFLAGS -o "$WORK/$name" "$@" -lpthread $DLLIB > "$WORK/$name.build.log" 2>&1; then
    echo "BUILD FAILED $name"; cat "$WORK/$name.build.log"; failed=1; return
  fi
  if "$WORK/$name"; then echo "PASS $name"; else echo "FAIL $name"; failed=1; fi
}

# run_sh <name> <script>: run a shell test
run_sh() {
  if sh "$2" "$TOP"; then echo "PASS $1"; else echo "FAIL $1"; failed=1; fi
}

run_c test_guest "$HERE/test_guest.c"
run_c test_rootenum "$HERE/test_rootenum.c" "$TOP/common/ios_rootenum.c"
run_c test_afd "-I$TOP/ntdll-unix" "$HERE/test_afd.c"
run_c test_resolv "-DIOS_RESOLV_TEST" "$HERE/test_resolv.c" "$TOP/common/ios_resolv.c"
run_c test_nsi "$HERE/test_nsi.c" "$TOP/common/ios_nsi_pure.c"

# dnsapi end to end: needs the Wine headers and a configured build dir (for config.h)
if [ $# -ge 2 ]; then
  WINE=$(cd "$1" && pwd); BUILD=$(cd "$2" && pwd)
  DNSFLAGS="-std=gnu11 -O1 -g -w -D__WINESRC__ -DWINE_UNIX_LIB -DIOS_RESOLV_TEST \
    -D__wine_unix_call_funcs=dnsapi_unix_call_funcs -D__wine_unix_call_wow64_funcs=dnsapi_unix_call_wow64_funcs \
    -include $BUILD/include/config.h -include $TOP/include/ios_cfg_dnsapi.h \
    -I$TOP/include -I$WINE/dlls/dnsapi -I$BUILD/include -I$WINE/include"
  # shellcheck disable=SC2086
  if $HOSTCC $DNSFLAGS -o "$WORK/test_dnsapi" "$HERE/test_dnsapi.c" "$TOP/dnsapi/libresolv.c" \
       "$TOP/common/ios_resolv.c" -lpthread $DLLIB > "$WORK/test_dnsapi.build.log" 2>&1; then
    if "$WORK/test_dnsapi"; then echo "PASS test_dnsapi"; else echo "FAIL test_dnsapi"; failed=1; fi
    if "$WORK/test_dnsapi" off; then echo "PASS test_dnsapi off"; else echo "FAIL test_dnsapi off"; failed=1; fi
  else
    echo "BUILD FAILED test_dnsapi"; cat "$WORK/test_dnsapi.build.log"; failed=1
  fi

  NSIFLAGS="-std=gnu11 -O1 -g -w -D__WINESRC__ -DWINE_UNIX_LIB \
    -D__wine_unix_call_funcs=nsi_unix_call_funcs -D__wine_unix_call_wow64_funcs=nsi_unix_call_wow64_funcs \
    -include $BUILD/include/config.h -include $TOP/include/ios_cfg_nsi.h \
    -I$TOP/include -I$WINE/dlls/nsiproxy.sys -I$BUILD/include -I$WINE/include"
  # shellcheck disable=SC2086
  if $HOSTCC $NSIFLAGS -o "$WORK/test_nsi_dispatch" "$HERE/test_nsi_dispatch.c" "$TOP/nsi/nsi_unix.c" \
       "$TOP/common/ios_nsi_pure.c" -lpthread > "$WORK/test_nsi_dispatch.build.log" 2>&1; then
    if "$WORK/test_nsi_dispatch"; then echo "PASS test_nsi_dispatch"; else echo "FAIL test_nsi_dispatch"; failed=1; fi
    if "$WORK/test_nsi_dispatch" off; then echo "PASS test_nsi_dispatch off"; else echo "FAIL test_nsi_dispatch off"; failed=1; fi
  else
    echo "BUILD FAILED test_nsi_dispatch"; cat "$WORK/test_nsi_dispatch.build.log"; failed=1
  fi

  # clarification 15: the crypto unix sides on the fake of include/ios_applecrypto.h, with host
  # builds of BearSSL and LibTomCrypt copied out of refs/ (cached between runs)
  DEPS_CACHE=${NETCRYPTO_DEPS_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/myiosdeck-13-host-deps}
  if D=$(sh "$HERE/host-deps.sh" "$DEPS_CACHE" "$HOSTCC" | tail -n 1) && [ -f "$D/lib/libbearssl.a" ]; then
    LTCFLAGS="-I$D/include/tomcrypt -include $TOP/include/ios_tomcrypt_rename.h \
      -DLTC_NO_PROTOTYPES -DLTC_SOURCE -DLTC_NO_TEST -DMP_32BIT="
    CRYPTO_OBJS=""
    for f in tests/fake_applecrypto.c common/ios_der.c common/ios_ltc.c; do
      o="$WORK/$(basename "$f" .c).o"
      # shellcheck disable=SC2086
      if $HOSTCC $CFLAGS -I$D/include -I$D/include/bearssl $LTCFLAGS -c "$TOP/$f" -o "$o" > "$o.log" 2>&1; then
        CRYPTO_OBJS="$CRYPTO_OBJS $o"
      else
        echo "BUILD FAILED $f"; cat "$o.log"; failed=1
      fi
    done
    CRYPTO_LIBS="$D/lib/libbearssl.a $D/lib/libtomcrypt.a"
    WINEFLAGS="-std=gnu11 -O1 -g -w -D__WINESRC__ -DWINE_UNIX_LIB -include $BUILD/include/config.h \
      -I$TOP -I$TOP/include -I$HERE -I$D/include -I$BUILD/include -I$WINE/include"

    # run_wine <name> <dll dir> <test source>: a test that includes a unix-side file
    run_wine() {
      # shellcheck disable=SC2086
      if $HOSTCC $WINEFLAGS -I"$WINE/dlls/$2" -o "$WORK/$1" "$3" $CRYPTO_OBJS $CRYPTO_LIBS -lpthread \
           > "$WORK/$1.build.log" 2>&1; then
        if "$WORK/$1" 2> "$WORK/$1.stderr"; then echo "PASS $1"
        else echo "FAIL $1"; tail -n 20 "$WORK/$1.stderr"; failed=1; fi
      else
        echo "BUILD FAILED $1"; head -n 60 "$WORK/$1.build.log"; failed=1
      fi
    }
    run_wine test_bcrypt bcrypt "$HERE/test_bcrypt.c"
    # clarification 8.6: the attach logs the self-test line
    if grep -q 'bcrypt self-test: 7/7 passed$' "$WORK/test_bcrypt.stderr" 2>/dev/null; then echo "PASS bcrypt self-test line"
    else echo "FAIL bcrypt self-test line"; grep 'self-test' "$WORK/test_bcrypt.stderr"; failed=1; fi
    run_wine test_schannel secur32 "$HERE/test_schannel.c"
    run_wine test_crypt32 crypt32 "$HERE/test_crypt32.c"
  else
    echo "FAIL host-deps (BearSSL / LibTomCrypt for the host)"; failed=1
  fi
else
  echo "SKIP test_dnsapi, test_nsi_dispatch, test_bcrypt (give <wine-11.4 tree> <wine build dir> to run them)"
fi

# spec 00 F: no forbidden API in the part's sources
if sh "$TOP/../../check-app-store.sh" "$TOP"; then echo "PASS check-app-store"; else echo "FAIL check-app-store"; failed=1; fi

exit $failed
