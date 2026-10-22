#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 Part C: no unix call table without its DLL. Every DLL of spec 09's unixlibs.txt (the
# unix libraries linked into the app, found by name) must be in each farm: listed in
# farms/arm64ec-windows.txt and farms/aarch64-windows.txt, and not skipped by
# farms/i386-skip.txt; unless farms/unixlibs-skip.txt names it for that farm (or *) with a reason.
# A skip line must name a DLL of unixlibs.txt that its farm really lacks, so it can't go stale.
#   test-unixlibs.sh [unixlibs.txt]

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
unixlibs=${1:-"$top/../09-ntdll-processes/unixlibs.txt"}
farms="$top/farms"
skip="$farms/unixlibs-skip.txt"
failures=0

bad() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

if [ ! -f "$unixlibs" ]; then
    echo "SKIPPED: test-unixlibs.sh (no $unixlibs)"
    exit 0
fi

lower() { tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz'; }
# names of a list file: first word of each line, comments removed, lower case
names() { sed -e 's/#.*//' "$1" | lower | awk 'NF { print $1 }'; }

# is <dll> skipped for <farm> by unixlibs-skip.txt? (the reason must not be empty)
skipped() {
    sed -e 's/^#.*//' "$skip" | awk -v f="$1" -v d="$2" '
        NF >= 3 && ($1 == f || $1 == "*") && tolower($2) == d { found = 1 }
        END { exit !found }'
}

# is <dll> skipped by the i386 skip list (names or shell patterns)?
i386_skipped() {
    names "$farms/i386-skip.txt" > "$tmp/i386-skip"
    while read -r pat; do
        # shellcheck disable=SC2254
        case "$1" in $pat) return 0 ;; esac
    done < "$tmp/i386-skip"
    return 1
}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
names "$unixlibs" > "$tmp/dlls"
[ -s "$tmp/dlls" ] || bad "no DLL names in $unixlibs"

checked=0
while read -r dll; do
    checked=$((checked + 1))
    for farm in arm64ec-windows aarch64-windows; do
        if names "$farms/$farm.txt" | grep -qxF "$dll"; then
            if skipped "$farm" "$dll"; then bad "$dll is in farms/$farm.txt and also skipped for it in unixlibs-skip.txt"; fi
        elif ! skipped "$farm" "$dll"; then
            bad "$dll has a unix table (unixlibs.txt) but isn't in farms/$farm.txt nor skipped for it in farms/unixlibs-skip.txt"
        fi
    done
    if i386_skipped "$dll" && ! skipped i386-windows "$dll"; then
        bad "$dll has a unix table (unixlibs.txt) but farms/i386-skip.txt leaves it out of i386-windows, and unixlibs-skip.txt gives no reason"
    fi
done < "$tmp/dlls"

# every skip line names a DLL of unixlibs.txt, a known farm and a reason
sed -e 's/^#.*//' "$skip" | awk 'NF' > "$tmp/skip-lines"
while read -r farm dll reason; do
    dll=$(echo "$dll" | lower)
    case "$farm" in
        '*'|arm64ec-windows|aarch64-windows|i386-windows) ;;
        *) bad "unixlibs-skip.txt: unknown farm $farm" ;;
    esac
    [ -n "$reason" ] || bad "unixlibs-skip.txt: $dll has no reason"
    grep -qxF "$dll" "$tmp/dlls" || bad "unixlibs-skip.txt: $dll is not in unixlibs.txt (stale line)"
done < "$tmp/skip-lines"

if [ $failures -gt 0 ]; then
    echo "test-unixlibs.sh: $failures failure(s)" >&2
    exit 1
fi
echo "test-unixlibs.sh: all $checked unix-table DLLs are in every farm or skipped with a reason"
