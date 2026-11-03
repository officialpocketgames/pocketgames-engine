#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part B2: two decisions of build-moltenvk.sh, kept apart so the host tests can make them on
# fake trees and archives (tests/test-moltenvk-checks.sh).
#
#   moltenvk-lib.sh slice <dir> <name>.xcframework <lib> <slice>
#       Prints the one <lib> of the <slice> slice (ios-arm64, macos-arm64) of a <name>.xcframework
#       under <dir>. The slice folders are lower case (ios-arm64, macos-arm64_x86_64), matched
#       without regard to case; simulator and Mac Catalyst slices never count; links aren't
#       followed (Package/Latest). None, or more than one, fails and lists what there is.
#   moltenvk-lib.sh spirv-cross <archive>
#       Prints "inside" when SPIRV-Cross's code is in the archive, as global or local symbols
#       (MoltenVK prelinks its shader converter, with SPIRV-Cross, into one object whose symbols are
#       hidden by default, so `nm -g` doesn't show them); "merge" when the archive refers to
#       SPIRV-Cross symbols no member defines globally; "absent" when it neither has nor refers to
#       any (MoltenVK can't work without it).
#   moltenvk-lib.sh spirv-tools <archive>
#       Prints every SPIRV-Tools symbol the archive defines (globally or locally) or refers to:
#       its C API (spv followed by a capital: spvContextCreate, spvBinaryToText, ...) and its C++
#       namespace (spvtools). Nothing printed means none (B3 keeps SPIRV-Tools out; an
#       unresolved spv* reference would also fail the app's link).
#
# Environment: NM (default: xcrun nm on macOS, nm elsewhere).

set -eu
die() { echo "moltenvk-lib: $*" >&2; exit 1; }
[ $# -ge 1 ] || die "usage: $0 slice <dir> <name>.xcframework <lib> <slice> | spirv-cross <archive> | spirv-tools <archive>"
if [ "$(uname -s)" = Darwin ]; then NM=${NM:-"xcrun nm"}; else NM=${NM:-nm}; fi

case $1 in
    slice)
        [ $# -eq 5 ] || die "usage: $0 slice <dir> <name>.xcframework <lib> <slice>"
        [ -d "$2" ] || die "no directory $2"
        found=$(find "$2" -ipath "*/$3/$5*" -name "$4" ! -ipath '*simulator*' ! -ipath '*maccatalyst*' 2>/dev/null |
                LC_ALL=C sort)
        n=$(printf '%s\n' "$found" | grep -c . || true)
        if [ "$n" -eq 1 ]; then echo "$found"; exit 0; fi
        if [ "$n" -eq 0 ]; then
            echo "moltenvk-lib: no $5 $4 in a $3 under $2; the archives there:" >&2
            find "$2" -name '*.a' 2>/dev/null | head -n 20 >&2
        else
            echo "moltenvk-lib: $n candidates for the $5 $4 of $3 under $2:" >&2
            printf '%s\n' "$found" >&2
        fi
        exit 1
        ;;
    spirv-cross)
        [ $# -eq 2 ] || die "usage: $0 spirv-cross <archive>"
        [ -f "$2" ] || die "no archive $2"
        t=$(mktemp -d)
        trap 'rm -rf "$t"' EXIT
        # shellcheck disable=SC2086
        $NM "$2" 2>/dev/null > "$t/all" || true
        # defined anywhere (local symbols too: lower-case types), and defined globally
        awk 'NF >= 3 && $(NF-1) ~ /^[TtDdBbSsRrCVvWw]$/ { print $NF }' "$t/all" | grep 'spirv_cross' | sort -u > "$t/defined" || true
        awk 'NF >= 3 && $(NF-1) ~ /^[TDBSRCVW]$/ { print $NF }' "$t/all" | sort -u > "$t/global"
        # shellcheck disable=SC2086
        $NM -u "$2" 2>/dev/null | awk 'NF { print $NF }' | grep -v ':$' | grep 'spirv_cross' | sort -u > "$t/wanted" || true
        if [ -n "$(comm -23 "$t/wanted" "$t/global")" ]; then echo merge
        elif [ -s "$t/defined" ]; then echo inside
        else echo absent
        fi
        ;;
    spirv-tools)
        [ $# -eq 2 ] || die "usage: $0 spirv-tools <archive>"
        [ -f "$2" ] || die "no archive $2"
        # defined (three fields) and undefined (two) alike; Mach-O's C names have a leading _
        # shellcheck disable=SC2086
        $NM "$2" 2>/dev/null | awk 'NF >= 2 { print $NF }' | grep -v ':$' | grep -E 'spvtools|^_?spv[A-Z]' | sort -u || true
        ;;
    *) die "unknown command $1" ;;
esac
