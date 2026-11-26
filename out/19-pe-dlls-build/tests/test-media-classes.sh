#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 A21: every media class the registry names has its DLL in the farm. In Wine each module
# registers its own classes (an IDL file marked "#pragma makedep register", or a .rgs file), and
# their InprocServer32 names the module itself; the registry has them for every module of the
# tree, so a class whose module a farm lacks fails with "load_dll failed ... c0000135".
#   1. Media modules: modules of Wine's dlls/ that register classes and import or delay-import
#      mfplat, mfuuid, strmbase, strmiids, wmcodecdspuuid, dmoguids or msdmo (and winegstreamer).
#      Each must be in farms/arm64ec-windows.txt and not left out of i386-windows by
#      farms/i386-skip.txt, unless farms/media-skip.txt leaves it out of that farm with a reason
#      or calls it not a media module.
#   2. Classes made from another module: a CLSID_* GUID defined in a media module's C files that
#      another module registers (msvproc.dll's video processor is winegstreamer.dll's
#      CLSID_wg_video_processor) must have that module in the same farm.
#   3. Every media-skip.txt line names a module of rule 1, a known farm and a reason, and the
#      module really is missing from that farm (no stale lines).
#   test-media-classes.sh [wine-src]

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
wine=${1:-${WINE_SRC:-"$top/../../refs/wine-11.4"}}
farms="$top/farms"
skip="$farms/media-skip.txt"
failures=0

bad() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

if [ ! -d "$wine/dlls" ]; then
    echo "SKIPPED: test-media-classes.sh (no $wine/dlls)"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

lower() { tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz'; }
names() { sed -e 's/#.*//' "$1" | lower | awk 'NF { print $1 }'; }
names "$farms/arm64ec-windows.txt" > "$tmp/arm64ec"
names "$farms/i386-skip.txt" > "$tmp/i386-skip"
sed -e 's/#.*//' "$skip" | awk 'NF' > "$tmp/skip"

# <farm> <dll>: does media-skip.txt leave it out of <farm> (with a reason)?
skipped() {
    awk -v f="$1" -v d="$2" 'NF >= 3 && $1 == f && tolower($2) == d { found = 1 } END { exit !found }' "$tmp/skip"
}
i386_skipped() {
    while read -r pat; do
        # shellcheck disable=SC2254
        case "$1" in $pat) return 0 ;; esac
    done < "$tmp/i386-skip"
    return 1
}
# <farm> <dll>: is it in the farm?
in_farm() {
    case "$1" in
        arm64ec-windows) grep -qxF "$2" "$tmp/arm64ec" ;;
        i386-windows) ! i386_skipped "$2" ;;
    esac
}

# the folders of modules that register classes, read in one pass each
grep -l 'makedep register' "$wine"/dlls/*/*.idl 2>/dev/null > "$tmp/reg-idl" || true
ls "$wine"/dlls/*/*.rgs 2>/dev/null > "$tmp/reg-rgs" || true
cat "$tmp/reg-idl" "$tmp/reg-rgs" | sed 's|/[^/]*$||' | sort -u > "$tmp/reg-dirs"

# uuid -> registering module, from the registering IDL files
: > "$tmp/uuids"
while read -r idl; do
    mod=$(sed -n 's/^MODULE *= *//p' "$(dirname "$idl")/Makefile.in" | lower)
    sed -n 's/.*uuid(\([0-9a-fA-F-]*\)).*/\1/p' "$idl" | lower | sed "s/\$/ $mod/" >> "$tmp/uuids"
done < "$tmp/reg-idl"

# rule 1: the media modules
: > "$tmp/media"
while read -r dir; do
    mk="$dir/Makefile.in"
    [ -f "$mk" ] || continue
    mod=$(sed -n 's/^MODULE *= *//p' "$mk" | lower)
    [ -n "$mod" ] || continue
    imports=$(grep -E '^(IMPORTS|DELAYIMPORTS)' "$mk" | tr '\n' ' ')
    case " $imports " in
        *[\ =]mfplat\ *|*[\ =]mfuuid\ *|*[\ =]strmbase\ *|*[\ =]strmiids\ *|*[\ =]wmcodecdspuuid\ *|*[\ =]dmoguids\ *|*[\ =]msdmo\ *) ;;
        *) [ "$mod" = winegstreamer.dll ] || continue ;;
    esac
    echo "$mod $dir" >> "$tmp/media"
done < "$tmp/reg-dirs"
[ -s "$tmp/media" ] || bad "no media module found in $wine/dlls"

checked=0 skipped_count=0 cross=0
while read -r mod dir; do
    if skipped not-media "$mod"; then
        skipped_count=$((skipped_count + 1))
        continue
    fi
    checked=$((checked + 1))
    for farm in arm64ec-windows i386-windows; do
        if in_farm "$farm" "$mod"; then
            skipped "$farm" "$mod" && bad "$mod is in $farm and also left out of it by media-skip.txt"
        elif ! skipped "$farm" "$mod"; then
            bad "$mod registers media classes but isn't in $farm, and media-skip.txt gives no reason"
            continue
        else
            continue
        fi
        # rule 2: CLSID_* GUIDs defined in its C files and registered by another module
        for c in "$dir"/*.c; do
            [ -f "$c" ] || continue
            grep -E 'GUID +CLSID_[A-Za-z0-9_]+ *= *\{' "$c" | while read -r line; do
                set -- $(echo "$line" | grep -oE '0x[0-9a-fA-F]+')
                [ $# -eq 11 ] || continue
                printf '%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x\n' \
                    "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$8" "$9" "${10}" "${11}"
            done
        done | sort -u > "$tmp/clsids"
        while read -r uuid; do
            owner=$(awk -v u="$uuid" '$1 == u { print $2; exit }' "$tmp/uuids")
            [ -n "$owner" ] && [ "$owner" != "$mod" ] || continue
            [ "$farm" = arm64ec-windows ] && cross=$((cross + 1))
            in_farm "$farm" "$owner" ||
                bad "$mod ($farm) makes class {$uuid}, registered by $owner, which isn't in $farm"
        done < "$tmp/clsids"
    done
done < "$tmp/media"

# rule 3: no stale or malformed skip lines
while read -r farm dll reason; do
    dll=$(echo "$dll" | lower)
    case "$farm" in
        arm64ec-windows|i386-windows|not-media) ;;
        *) bad "media-skip.txt: unknown farm $farm" ;;
    esac
    [ -n "$reason" ] || bad "media-skip.txt: $dll has no reason"
    awk -v d="$dll" '$1 == d { found = 1 } END { exit !found }' "$tmp/media" ||
        bad "media-skip.txt: $dll isn't a module that registers media classes (stale line)"
    case "$farm" in
        arm64ec-windows|i386-windows)
            if in_farm "$farm" "$dll"; then bad "media-skip.txt: $dll is in $farm after all (stale line)"; fi ;;
    esac
done < "$tmp/skip"

if [ $failures -ne 0 ]; then
    echo "test-media-classes.sh: $failures failure(s)" >&2
    exit 1
fi
echo "test-media-classes.sh: $checked media modules in both farms or left out with a reason ($skipped_count not media); $cross classes made from another module resolve in the ARM64EC farm"
