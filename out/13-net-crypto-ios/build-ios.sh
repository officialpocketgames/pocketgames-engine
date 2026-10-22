#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 13 Part A2-A5 and clarification 7.2: compile the unix sides of ws2_32, dnsapi, nsi, bcrypt,
# secur32 and crypt32 for iOS arm64 into one static library, <out dir>/libnetcrypto_unix.a, and
# copy BearSSL's and LibTomCrypt's archives (build-crypto-deps-ios.sh) next to it, unmerged.
#
# usage: build-ios.sh <wine-11.4 tree> <wine build dir> <out dir> <deps prefix>
#
#   <wine-11.4 tree>  upstream source tree (refs/wine-11.4)
#   <wine build dir>  a host (macOS) configure + "make include/all" of that tree: it holds
#                     include/config.h and the generated headers
#   <out dir>         receives obj/, gen/, logs/, libnetcrypto_unix.a, libbearssl.a,
#                     libtomcrypt_unix.a and LINK.txt
#   <deps prefix>     the prefix build-crypto-deps-ios.sh installed into (lib/, include/bearssl/,
#                     include/tomcrypt/)
#
# Environment:
#   CC, AR, NM   tools (default: xcrun -sdk iphoneos clang / ar / nm)
#   NOEXIT_H     spec 09's no-exit header (default: out/09-ntdll-processes/ntdll-unix/ios_noexit.h)
#   EXTRA_CFLAGS appended to every compile
#
# Which file gets which per-file configuration header (Part A6) and unix-call table names
# (Part A3) is listed in compile_all below. Prints "<name> OK" / "<name> FAILED" per file, then
# the errors of the failed ones, and exits non-zero if any failed.
#
# ntdll's socket.c (Part G3) is not built here: spec 09's build-ios.sh takes ntdll-unix/socket.c
# and ios_afd.h itself, in place of upstream's socket.c in libntdll_unix.a (spec 13
# clarification 4; CHANGES.md, "Integration of G3").

set -u

if [ $# -ne 4 ]; then
  echo "usage: $0 <wine-11.4 tree> <wine build dir> <out dir> <deps prefix>" >&2
  exit 2
fi

abspath() { (cd "$1" 2>/dev/null && pwd) || { echo "no such directory: $1" >&2; exit 2; }; }

WINE=$(abspath "$1") || exit 2
BUILD=$(abspath "$2") || exit 2
mkdir -p "$3" || exit 2
OUT=$(abspath "$3") || exit 2
DEPS=$(abspath "$4") || exit 2
HERE=$(abspath "$(dirname "$0")") || exit 2
REPO=$(abspath "$HERE/../..") || exit 2
NOEXIT_H=${NOEXIT_H:-"$REPO/out/09-ntdll-processes/ntdll-unix/ios_noexit.h"}

[ -f "$WINE/dlls/ws2_32/unixlib.c" ] || { echo "not a wine tree: $WINE" >&2; exit 2; }
[ -f "$BUILD/include/config.h" ] || { echo "no include/config.h in $BUILD" >&2; exit 2; }
for f in lib/libbearssl.a lib/libtomcrypt_unix.a include/bearssl/bearssl.h include/tomcrypt/tomcrypt.h; do
  [ -f "$DEPS/$f" ] || { echo "no $f in $DEPS (run build-crypto-deps-ios.sh)" >&2; exit 2; }
done
[ -f "$NOEXIT_H" ] || { echo "no-exit header not found: $NOEXIT_H (set NOEXIT_H)" >&2; exit 2; }

CC=${CC:-"xcrun -sdk iphoneos clang"}
AR=${AR:-"xcrun -sdk iphoneos ar"}
NM=${NM:-"xcrun -sdk iphoneos nm"}

OBJ="$OUT/obj"
GEN="$OUT/gen"
LOG="$OUT/logs"
rm -rf "$OBJ" "$GEN" "$LOG"
mkdir -p "$OBJ" "$GEN" "$LOG" || exit 1

COMMON="-arch arm64 -miphoneos-version-min=17.0 -O2 -std=gnu11 -fPIC -fno-strict-aliasing \
 -fasynchronous-unwind-tables -fno-omit-frame-pointer -ffixed-x18 ${EXTRA_CFLAGS:-}"
DEFS="-DWINE_IOS=1 -DWINE_UNIX_LIB -D__WINESRC__ -D_REENTRANT"
WARN="-Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
 -Wno-pragma-pack -Wno-deprecated-declarations -Wno-unknown-pragmas"
# spec 17 Part A1 (spec 13 clarification 1): the shared header overrides (wine/unixlib.h with
# ios_wow_host_ptr / ios_wow_guest_ptr32, winternl.h) before the Wine tree's include/
BASEINC="-I$HERE/include -I$HERE/../09-ntdll-processes/include -I$BUILD/include -I$WINE/include"
# config.h first (it has an include guard, so the files' own #include "config.h" is a no-op),
# then spec 09's no-exit header, then the per-file header
FIRST="-include $BUILD/include/config.h -include $NOEXIT_H"

# A3: the two unix-call tables of DLL $1 get their by-name symbols
tables() {
  echo "-D__wine_unix_call_funcs=${1}_unix_call_funcs -D__wine_unix_call_wow64_funcs=${1}_unix_call_wow64_funcs"
}

failed=""
# compile <object name> <source> <extra flags...>
compile() {
  name=$1; src=$2; shift 2
  # shellcheck disable=SC2086
  if $CC $COMMON $DEFS $WARN $BASEINC "$@" -c "$src" -o "$OBJ/$name.o" > "$LOG/$name.log" 2>&1; then
    echo "$name OK"
  else
    echo "$name FAILED"
    failed="$failed $name"
  fi
}

# ---------------------------------------------------------------- compile
# clarifications 7-13: BearSSL's headers as "bearssl/bearssl.h"; LibTomCrypt with Wine's defines
# (build-crypto-deps-ios.sh's TOMCRYPT_DEFS) and the A4 rename header
BEAR="-I$DEPS/include"
LTC="-I$DEPS/include/tomcrypt -include $HERE/include/ios_tomcrypt_rename.h \
 -DLTC_NO_PROTOTYPES -DLTC_SOURCE -DLTC_NO_TEST -DMP_32BIT="
CFG_DNS="-include $HERE/include/ios_cfg_dnsapi.h"
CFG_NSI="-include $HERE/include/ios_cfg_nsi.h"
NSI_INC="-I$WINE/dlls/nsiproxy.sys"

# shellcheck disable=SC2046
{
compile ws2_32_unixlib   "$HERE/ws2_32/unixlib.c"            $FIRST -I$WINE/dlls/ws2_32 $(tables ws2_32)
compile dnsapi_libresolv "$HERE/dnsapi/libresolv.c"          $FIRST $CFG_DNS -I$WINE/dlls/dnsapi $(tables dnsapi)
compile ios_resolv       "$HERE/common/ios_resolv.c"         $FIRST $CFG_DNS
compile bcrypt_unixlib   "$HERE/bcrypt/unixlib_ios.c"        $FIRST $BEAR -I$WINE/dlls/bcrypt $(tables bcrypt)
compile secur32_schannel "$HERE/secur32/schannel_bearssl.c"  $FIRST $BEAR -I$WINE/dlls/secur32 $(tables secur32)
compile crypt32_unixlib  "$HERE/crypt32/unixlib.c"           $FIRST $BEAR -I$WINE/dlls/crypt32 $(tables crypt32)
compile ios_rootenum     "$HERE/common/ios_rootenum.c"
compile ios_applecrypto  "$HERE/common/ios_applecrypto.c"
compile ios_der          "$HERE/common/ios_der.c"
compile ios_ltc          "$HERE/common/ios_ltc.c"            $LTC
compile nsi_unix        "$HERE/nsi/nsi_unix.c"              $FIRST $CFG_NSI $NSI_INC $(tables nsi)
compile nsi_tcp          "$HERE/nsi/nsi_tcp.c"               $FIRST $CFG_NSI $NSI_INC
compile nsi_ip6scope     "$HERE/nsi/nsi_ip6scope.c"          $FIRST $CFG_NSI $NSI_INC
compile nsi_ndis         "$HERE/nsi/ndis.c"                  $FIRST $CFG_NSI $NSI_INC
compile nsi_ip           "$WINE/dlls/nsiproxy.sys/ip.c"      $FIRST -DIOS_NSI_IP_C $CFG_NSI $NSI_INC
compile ios_nsi_pure     "$HERE/common/ios_nsi_pure.c"
}

if [ -n "$failed" ]; then
  for name in $failed; do
    echo "===== $name ====="
    cat "$LOG/$name.log"
  done
  exit 1
fi

rm -f "$OUT/libnetcrypto_unix.a"
# shellcheck disable=SC2086
$AR rcs "$OUT/libnetcrypto_unix.a" "$OBJ"/*.o || exit 1
echo "wrote $OUT/libnetcrypto_unix.a"

# clarification 7.2: BearSSL's and LibTomCrypt's archives next to it, unmerged
for a in libbearssl.a libtomcrypt_unix.a; do
  cp "$DEPS/lib/$a" "$OUT/$a" || exit 1
  echo "copied $OUT/$a"
done

# clarification 14: neither third-party archive may import a process or loader function
NM="$NM" sh "$HERE/check-symbols.sh" imports "$OUT/libbearssl.a" "$OUT/libtomcrypt_unix.a" || exit 1

# ---------------------------------------------------------------- A5: LINK.txt
# Undefined symbols of the three archives that none of them defines, sorted by who provides
# them. The twelve unix-call tables are only looked up by name (dlsym), so nothing references
# them: the link must name them with -u, or their objects are never loaded from the archive.
ARCHIVES="$OUT/libnetcrypto_unix.a $OUT/libbearssl.a $OUT/libtomcrypt_unix.a"
# shellcheck disable=SC2086
$NM -g -U $ARCHIVES 2>/dev/null | awk 'NF >= 3 { print $3 }' | sort -u > "$GEN/defined.txt"
# shellcheck disable=SC2086
$NM -g -u $ARCHIVES 2>/dev/null | awk 'NF == 1 && $1 !~ /:$/ { print $1 }' | sort -u > "$GEN/undefined-all.txt"
comm -23 "$GEN/undefined-all.txt" "$GEN/defined.txt" > "$GEN/undefined.txt"

{
  echo "# LINK.txt: what the final app link needs for libnetcrypto_unix.a, libbearssl.a and"
  echo "# libtomcrypt_unix.a (generated by build-ios.sh from nm; see gen/undefined.txt for every symbol)"
  echo
  echo "# archives, in this order"
  echo "libnetcrypto_unix.a libbearssl.a libtomcrypt_unix.a"
  echo
  echo "# the twelve unix-call tables (spec 09 Part A finds them with dlsym; keep and load them)"
  for d in ws2_32 dnsapi bcrypt secur32 crypt32 nsi; do
    echo "-Wl,-u,_${d}_unix_call_funcs"
    echo "-Wl,-u,_${d}_unix_call_wow64_funcs"
  done
  echo
  echo "# system libraries and frameworks (spec 13 clarification 7.3; CommonCrypto is in libSystem)"
  echo "-framework Security"
  echo "-framework CoreFoundation"
  echo "-lobjc      (objc_autoreleasePoolPush/Pop, ws2_32 Part G2)"
  echo "-lresolv    (dnsapi's res_ninit/res_nquery/res_ndestroy, clarification 3; linked, never opened by path)"
  awk '
    /^_res_|^_res_9_|^_objc_|^_(k)?Sec|^_(k)?CF/ { next }   # above
    /^_(lib)?iconv/                           { need["-liconv"] = 1; next }
    /^_(deflate|inflate|compress|uncompress|crc32|adler32|zlibVersion)/ { need["-lz"] = 1; next }
    { other[$1] = 1 }
    END {
      for (n in need) print n
      print ""
      print "# provided by the other unix libraries of the app (ntdll: spec 09/10) or libSystem"
      for (s in other) print "#   " s
    }' "$GEN/undefined.txt"
} > "$OUT/LINK.txt"
echo "wrote $OUT/LINK.txt"
echo "note: link the app with LINK.txt's archives, -u flags, frameworks and libraries"
