#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 13 Part A3/A4 and clarifications 7.4 and 14: checks for CI.
#
# usage: check-symbols.sh clashes <libnetcrypto_unix.a> <other archive>...
#          Lists every external symbol that libnetcrypto_unix.a, libbearssl.a or
#          libtomcrypt_unix.a (the two found next to the first) defines and another archive given
#          (ntdll's, win32u's, the server's) also defines. Duplicates inside one archive are not
#          reported. Exits 1 if there is any clash; rename the symbol on our side with a macro
#          (LibTomCrypt's in include/ios_tomcrypt_rename.h).
#
#        check-symbols.sh tables <app binary>
#          Checks that the twelve unix-call tables are in the final binary's symbol table.
#
#        check-symbols.sh imports <archive>...
#          Fails if an archive references fork, an exec* function, posix_spawn*, system, popen or
#          dlopen (`nm -u`; clarification 14, for libbearssl.a and libtomcrypt_unix.a).
#
# Environment: NM (default: xcrun -sdk iphoneos nm)

set -u
NM=${NM:-"xcrun -sdk iphoneos nm"}

defined() {
  # one "symbol<TAB>archive" line per external defined symbol of archive $1
  # shellcheck disable=SC2086
  $NM -g -U "$1" 2>/dev/null | awk -v a="$(basename "$1")" 'NF >= 3 { print $3 "\t" a }' | sort -u
}

case "${1:-}" in
clashes)
  shift
  [ $# -ge 2 ] || { echo "usage: $0 clashes <libnetcrypto_unix.a> <other archive>..." >&2; exit 2; }
  ours=$1; shift
  dir=$(dirname "$ours")
  tmp=${TMPDIR:-/tmp}/check-symbols.$$
  mkdir -p "$tmp" || exit 1
  : > "$tmp/ours"
  for a in "$ours" "$dir/libbearssl.a" "$dir/libtomcrypt_unix.a"; do
    [ -f "$a" ] && defined "$a" >> "$tmp/ours"
  done
  : > "$tmp/theirs"
  for a in "$@"; do defined "$a" >> "$tmp/theirs"; done
  cut -f1 "$tmp/ours" | sort -u > "$tmp/ours.names"
  cut -f1 "$tmp/theirs" | sort -u > "$tmp/theirs.names"
  comm -12 "$tmp/ours.names" "$tmp/theirs.names" > "$tmp/clash"
  if [ -s "$tmp/clash" ]; then
    echo "symbols defined on both sides:"
    while read -r s; do
      printf '  %s  (%s | %s)\n' "$s" \
        "$(awk -v s="$s" -F'\t' '$1 == s { print $2 }' "$tmp/ours" | tr '\n' ' ')" \
        "$(awk -v s="$s" -F'\t' '$1 == s { print $2 }' "$tmp/theirs" | tr '\n' ' ')"
    done < "$tmp/clash"
    rm -rf "$tmp"
    exit 1
  fi
  rm -rf "$tmp"
  echo "no clashes"
  ;;
tables)
  [ $# -eq 2 ] || { echo "usage: $0 tables <app binary>" >&2; exit 2; }
  # shellcheck disable=SC2086
  syms=$($NM -g "$2" 2>/dev/null | awk 'NF >= 3 { print $3 }')
  missing=0
  for d in ws2_32 dnsapi bcrypt secur32 crypt32 nsi; do
    for t in unix_call_funcs unix_call_wow64_funcs; do
      if printf '%s\n' "$syms" | grep -qx "_${d}_${t}"; then
        echo "found   ${d}_${t}"
      else
        echo "MISSING ${d}_${t}"
        missing=1
      fi
    done
  done
  exit $missing
  ;;
imports)
  shift
  [ $# -ge 1 ] || { echo "usage: $0 imports <archive>..." >&2; exit 2; }
  bad=0
  for a in "$@"; do
    [ -f "$a" ] || { echo "no such archive: $a" >&2; exit 2; }
    # shellcheck disable=SC2086
    # Apple's nm -u prints the name alone, GNU nm "U name"
    hits=$($NM -u "$a" 2>/dev/null | awk 'NF == 1 && $1 !~ /:$/ { print $1 } NF == 2 && $1 == "U" { print $2 }' | sed 's/^_//' |
           grep -xE 'fork|vfork|exec[lv]p?e?|execvP|fexecve|posix_spawnp?|posix_spawn_.*|system|popen|dlopen' |
           sort -u | tr '\n' ' ')
    if [ -n "$hits" ]; then
      echo "$(basename "$a") imports: $hits"
      bad=1
    else
      echo "$(basename "$a"): no process or loader imports"
    fi
  done
  exit $bad
  ;;
*)
  echo "usage: $0 clashes <libnetcrypto_unix.a> <other archive>... | $0 tables <app binary> | $0 imports <archive>..." >&2
  exit 2
  ;;
esac
