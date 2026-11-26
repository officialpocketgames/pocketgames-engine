#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 A18: API sets games import resolve in the ARM64EC and i386 farms. For each pair below,
# Wine's apisetschema.spec maps the API set to the DLL, and the DLL and apisetschema.dll are in
# farms/arm64ec-windows.txt and not left out of i386-windows by farms/i386-skip.txt. Also the
# modules A18 adds are in both farms.
#   test-apisets.sh [wine-src]

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
wine=${1:-${WINE_SRC:-"$top/../../refs/wine-11.4"}}
spec="$wine/dlls/apisetschema/apisetschema.spec"
farms="$top/farms"
failures=0

# api set, the DLL implementing it
pairs='api-ms-win-core-winrt-robuffer-l1-1-0 wintypes.dll'
# modules a farm must hold (A18)
modules='mfmp4srcsnk.dll mferror.dll wintypes.dll'

bad() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

if [ ! -f "$spec" ]; then
    echo "SKIPPED: test-apisets.sh (no $spec)"
    exit 0
fi

lower() { tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz'; }
names() { sed -e 's/#.*//' "$1" | lower | awk 'NF { print $1 }'; }
i386_skipped() {
    names "$farms/i386-skip.txt" | while read -r pat; do
        # shellcheck disable=SC2254
        case "$1" in $pat) echo yes; break ;; esac
    done | grep -q yes
}
in_farms() {
    names "$farms/arm64ec-windows.txt" | grep -qxF "$1" || bad "$1 isn't in farms/arm64ec-windows.txt ($2)"
    if i386_skipped "$1"; then bad "$1 is left out of i386-windows by farms/i386-skip.txt ($2)"; fi
}

in_farms apisetschema.dll "it resolves API sets"
# shellcheck disable=SC2086
set -- $pairs
while [ $# -ge 2 ]; do
    mapped=$(sed -n "s/^apiset $1 = \\([^ ]*\\).*/\\1/p" "$spec" | lower)
    [ "$mapped" = "$2" ] || bad "apisetschema.spec maps $1 to '${mapped:-nothing}', not $2"
    in_farms "$2" "$1 resolves to it"
    shift 2
done
for m in $modules; do
    in_farms "$m" "A18"
done

if [ $failures -ne 0 ]; then
    echo "test-apisets.sh: $failures failure(s)" >&2
    exit 1
fi
echo "test-apisets.sh: the API sets resolve and the A18 modules are in the ARM64EC and i386 farms"
