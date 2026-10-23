#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# CI (macOS): link every iOS static library of ci/ios-part.sh into one arm64 iOS executable with
# a tiny main and do-nothing definitions of what the app provides (ci/link-check/app_stubs.c,
# interfaces/wine_unix_contract.h Section B), report duplicate and undefined symbols, then check
# the imports of the result (`nm -u`) against ci/forbidden-imports.txt (the App Store rules).
#
#   ci/link-check.sh <dist dir> <report dir>
#
# <dist dir>/<part>/ as ci/ios-part.sh leaves it. This repository's own archives are
# force-loaded (all their code is meant to be in the app, and specs 14 and 15 require it); the
# third-party archives (BearSSL, LibTomCrypt, FFmpeg) are linked normally, as their parts' notes
# say. Spec 13's LINK.txt contributes its -Wl,-u, -l and -framework lines.
#
# Pass 1 links for real and reports what the linker says. LLVM's own symbols (libdxmt_unix.a's
# airconv uses LLVM 15.0.7, which the app's build links and this CI doesn't build) are reported
# but expected. Pass 2 adds a stub for every symbol still undefined, so there is an executable
# for `nm -u`; if that link fails too, the imports are taken from the archives instead (and the
# report says so).
#
# Fails on: a part without libraries, a duplicate symbol, an undefined symbol other than
# LLVM's, a "fail" import of ci/forbidden-imports.txt. Report: <report dir>/link-report.md.

set -u

[ $# -eq 2 ] || { echo "usage: $0 <dist dir> <report dir>" >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
dist=$(cd "$1" && pwd)
mkdir -p "$2"
rep=$(cd "$2" && pwd)
report="$rep/link-report.md"
: > "$report"

SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="$(xcrun --sdk iphoneos -f clang) -arch arm64 -isysroot $SDK -miphoneos-version-min=17.0"
CXX="$(xcrun --sdk iphoneos -f clang++) -arch arm64 -isysroot $SDK -miphoneos-version-min=17.0"
NM=$(xcrun --sdk iphoneos -f nm)

note() { echo "$*"; echo "$*" >> "$report"; }
failed=0

note "# Link check"
note ""
note "SDK: iPhoneOS $(xcrun --sdk iphoneos --show-sdk-version), $(xcodebuild -version | head -n 1)"
note ""

# ---------------------------------------------------------------- the archives
parts="05-dxmt-ios 08-wineserver-ios 09-ntdll-processes 12-win32u-ios 13-net-crypto-ios 14-media-audio-ios 15-controllers"
third_party_re='^lib(bearssl|tomcrypt_unix|avformat|avcodec|swresample|avutil)\.a$'
ours=""
missing=""
for p in $parts; do
    found=0
    for a in "$dist/$p"/*.a; do
        [ -f "$a" ] || continue
        if ! basename "$a" | grep -qE "$third_party_re"; then
            ours="$ours $a"; found=1
        fi
    done
    [ $found -eq 1 ] || missing="$missing $p"
done
# third-party archives in link order: spec 13's (LINK.txt order), then FFmpeg's
third=""
for n in libbearssl.a libtomcrypt_unix.a; do
    [ -f "$dist/13-net-crypto-ios/$n" ] && third="$third $dist/13-net-crypto-ios/$n"
done
for n in libavformat.a libavcodec.a libswresample.a libavutil.a; do
    [ -f "$dist/14-media-audio-ios/ffmpeg/$n" ] && third="$third $dist/14-media-audio-ios/ffmpeg/$n"
done
if [ -n "$missing" ]; then
    note "**Parts without libraries (their ios-libs build failed):**$missing — the symbols they define show up as undefined below."
    note ""
    failed=1
fi
note "Force-loaded:"
for a in $ours; do note "- ${a#"$dist"/}"; done
note "Linked normally:"
for a in $third; do note "- ${a#"$dist"/}"; done
note ""

# LINK.txt (spec 13 A5): the flag of each -Wl,-u, -l and -framework line (a line may go on with
# a comment after the flag)
extra=""
if [ -f "$dist/13-net-crypto-ios/LINK.txt" ]; then
    extra=$(awk '$1 == "-framework" { print $1, $2; next } $1 ~ /^-(Wl,-u,|l)/ { print $1 }' \
        "$dist/13-net-crypto-ios/LINK.txt" | tr '\n' ' ')
fi
frameworks="-framework Foundation -framework CoreFoundation -framework Metal -framework QuartzCore \
 -framework IOSurface -framework CoreGraphics -framework CoreText -framework UIKit \
 -framework GameController -framework CoreHaptics -framework AudioToolbox -framework AVFoundation \
 -framework CoreAudio -framework CoreMedia -framework CoreVideo -framework VideoToolbox \
 -framework Security -framework Network -framework SystemConfiguration -framework CFNetwork \
 -framework MetalFX -lresolv -lobjc -lz -liconv -lbz2 -lsqlite3 -lc++"

w="$rep/work"
rm -rf "$w"
mkdir -p "$w"
# shellcheck disable=SC2086
$CC -std=gnu11 -O1 -Wall -Wextra -Werror -c "$here/link-check/main.c" -o "$w/main.o" || exit 1
# shellcheck disable=SC2086
$CC -std=gnu11 -O1 -Wall -Wextra -Werror -I"$repo/interfaces" -c "$here/link-check/app_stubs.c" -o "$w/app_stubs.o" || exit 1

force=""
for a in $ours; do force="$force -Wl,-force_load,$a"; done

# link <output> <extra objects...>: the link line, the linker's messages in <output>.log
link() {
    o=$1; shift
    # shellcheck disable=SC2086
    $CXX -o "$o" "$w/main.o" "$w/app_stubs.o" "$@" $force $third $extra $frameworks > "$o.log" 2>&1
}

# ---------------------------------------------------------------- pass 1
link "$w/pass1"
pass1=$?
# The linker names C symbols as they are ("_foo") and C++ ones demangled ("llvm::Foo::bar()").
# The mangled names (for stubs and the LLVM test) come from the archives' own undefined
# references: each is demangled with c++filt and matched to the linker's name.
# undefined-pairs.txt: <mangled> TAB <the linker's name>
sed -n 's/^ *"\(.*\)", referenced from:$/\1/p' "$w/pass1.log" | sort -u > "$w/undefined-ld.txt"
sed -n "s/^duplicate symbol '\(.*\)' in:\$/\1/p" "$w/pass1.log" | sort -u > "$w/duplicates.txt"
# shellcheck disable=SC2086
$NM -u -j $ours $third "$w/main.o" "$w/app_stubs.o" 2>/dev/null | grep -v ':$' | grep . | sort -u > "$w/referenced.txt"
xcrun c++filt -_ < "$w/referenced.txt" > "$w/referenced-demangled.txt" 2>/dev/null ||
    cp "$w/referenced.txt" "$w/referenced-demangled.txt"
paste "$w/referenced.txt" "$w/referenced-demangled.txt" |
    awk -F'\t' '{ if ($1 ~ /^__Z/) print $2 "\t" $1; else print $1 "\t" $1 }' > "$w/name-map.txt"
awk -F'\t' 'NR == FNR { m[$1] = $2; next } { print (($0 in m) ? m[$0] : $0) "\t" $0 }' \
    "$w/name-map.txt" "$w/undefined-ld.txt" > "$w/undefined-pairs.txt"
cut -f1 "$w/undefined-pairs.txt" | sort -u > "$w/undefined.txt"
llvm_re='^(_+ZN?K?4llvm|_LLVM|__ZTVN4llvm|__ZTIN4llvm|__ZTSN4llvm)|	llvm::|	(vtable|typeinfo|typeinfo name) for llvm::'
grep -E "$llvm_re" "$w/undefined-pairs.txt" > "$w/undefined-llvm.txt" || true
grep -vE "$llvm_re" "$w/undefined-pairs.txt" > "$w/undefined-other.txt" || true

note "## Pass 1: the real link"
note ""
if [ $pass1 -eq 0 ]; then
    note "Linked with no errors."
else
    note "The link failed: $(wc -l < "$w/duplicates.txt" | tr -d ' ') duplicate, $(wc -l < "$w/undefined-other.txt" | tr -d ' ') undefined (not LLVM), $(wc -l < "$w/undefined-llvm.txt" | tr -d ' ') undefined LLVM symbols."
fi
note ""

# for a symbol, the archive members that reference it (pass 1's "referenced from" lines)
referrers() {
    awk -v s="$1" '
        /^ *".*", referenced from:$/ { cur = $0; sub(/^ *"/, "", cur); sub(/", referenced from:$/, "", cur); next }
        /^ +[^ ].* in / && cur == s { line = $0; sub(/^ +/, "", line); print "    - " line; n++; if (n >= 3) exit }
        /^[^ ]/ { cur = "" }
    ' "$w/pass1.log"
}

if [ -s "$w/duplicates.txt" ]; then
    failed=1
    note "### Duplicate symbols ($(wc -l < "$w/duplicates.txt" | tr -d ' '))"
    note ""
    awk '
        /^duplicate symbol / { if (sym != "") print ""; sym = $0; sub(/^duplicate symbol /, "", sym); sub(/ in:$/, "", sym); printf "- %s:", sym; next }
        sym != "" && /^ +[^ ]/ { line = $0; sub(/^ +/, "", line); n = split(line, parts, "/"); printf " %s;", parts[n]; next }
        { if (sym != "") { print ""; sym = "" } }
        END { if (sym != "") print "" }
    ' "$w/pass1.log" | head -n 200 | tee -a "$report"
    note ""
fi
if [ -s "$w/undefined-other.txt" ]; then
    failed=1
    note "### Undefined symbols, not LLVM ($(wc -l < "$w/undefined-other.txt" | tr -d ' '))"
    note ""
    tab=$(printf '\t')
    while IFS="$tab" read -r mangled s; do
        if [ "$mangled" = "$s" ]; then note "- \`$s\`"; else note "- \`$s\` (\`$mangled\`)"; fi
        referrers "$s" | tee -a "$report"
    done < "$w/undefined-other.txt"
    note ""
fi
if [ -s "$w/undefined-llvm.txt" ]; then
    note "### Undefined LLVM 15.0.7 symbols ($(wc -l < "$w/undefined-llvm.txt" | tr -d ' ')): expected, the app's build links LLVM"
    note ""
fi
if [ $pass1 -ne 0 ] && [ ! -s "$w/duplicates.txt" ] && [ ! -s "$w/undefined.txt" ]; then
    failed=1
    note "### Other linker errors"
    note ""
    note '```'
    grep -E 'error|ld:' "$w/pass1.log" | head -n 40 | tee -a "$report"
    note '```'
fi

# ---------------------------------------------------------------- pass 2: an executable for nm -u
exe=""
if [ $pass1 -eq 0 ]; then
    exe="$w/pass1"
elif [ ! -s "$w/duplicates.txt" ] && [ -s "$w/undefined.txt" ]; then
    {
        echo "/* generated by ci/link-check.sh: a stub for each symbol pass 1 left undefined */"
        echo ".text"
        while IFS= read -r s; do
            printf '.globl "%s"\n.p2align 2\n"%s":\n    ret\n' "$s" "$s"
        done < "$w/undefined.txt"
    } > "$w/undefined_stubs.s"
    # shellcheck disable=SC2086
    if $CC -c "$w/undefined_stubs.s" -o "$w/undefined_stubs.o" > "$w/stubs.log" 2>&1 && link "$w/pass2" "$w/undefined_stubs.o"; then
        exe="$w/pass2"
    fi
fi

note "## Imports"
note ""
if [ -n "$exe" ]; then
    note "From \`nm -u\` of the linked executable ($(basename "$exe"))."
    $NM -u "$exe" | sort -u > "$w/imports.txt"
else
    note "**No executable could be linked** (see pass 1, and $(basename "$w")/pass2.log); the imports below are every symbol the archives reference and none of them define, an over-estimate."
    # shellcheck disable=SC2086
    $NM -g -U -j $ours $third "$w/app_stubs.o" 2>/dev/null | grep -v ':$' | sort -u > "$w/defined.txt"
    # shellcheck disable=SC2086
    $NM -u -j $ours $third 2>/dev/null | grep -v ':$' | sort -u | comm -23 - "$w/defined.txt" > "$w/imports.txt"
fi
note "$(wc -l < "$w/imports.txt" | tr -d ' ') imported symbols."
note ""

# ---------------------------------------------------------------- forbidden imports
hits_fail=""
hits_review=""
while read -r name kind; do
    case "$name" in ''|'#'*) continue ;; esac
    case "$name" in
        *'*') pre=${name%'*'}; matches=$(awk -v p="$pre" 'index($0, p) == 1' "$w/imports.txt") ;;
        *) matches=$(grep -xF "$name" "$w/imports.txt" || true) ;;
    esac
    for m in $matches; do
        if [ "$kind" = fail ]; then hits_fail="$hits_fail $m"; else hits_review="$hits_review $m"; fi
    done
done < "$here/forbidden-imports.txt"

# which archive members import a symbol
importers() {
    # shellcheck disable=SC2086
    $NM -A -u $ours $third 2>/dev/null | awk -v s="$1" '$NF == s { sub(/: *$/, "", $1); print "    - " $1 }' |
        sed "s|$dist/||" | sort -u | head -n 10
}

note "## Forbidden imports (the App Store rules)"
note ""
if [ -n "$hits_fail" ]; then
    failed=1
    for s in $hits_fail; do
        note "- **\`$s\`**, imported by:"
        importers "$s" | tee -a "$report"
    done
else
    note "None."
fi
note ""
if [ -n "$hits_review" ]; then
    note "### To review (allowed API, JIT only in combination, or possibly not in the iPhoneOS SDK)"
    note ""
    # an importer listed with the symbol in ci/import-review-ok.txt was reviewed: shown with
    # the reason, and the symbol is "to review" only while another importer is left
    for s in $hits_review; do
        importers "$s" > "$w/importers.txt"
        left=""
        while IFS= read -r line; do
            member=${line#    - }
            reason=$(awk -v s="$s" -v m="$member" '$1 == s && $2 == m { $1 = ""; $2 = ""; sub(/^ +/, ""); print; exit }' \
                "$here/import-review-ok.txt" 2>/dev/null)
            if [ -n "$reason" ]; then
                note "- \`$s\` in $member: reviewed, allowed ($reason)"
            else
                left="$left$line
"
            fi
        done < "$w/importers.txt"
        if [ -n "$left" ]; then
            note "- \`$s\`, **to review**, imported by:"
            printf '%s' "$left" | tee -a "$report"
        fi
    done
    note ""
fi

cp "$w/pass1.log" "$w/undefined.txt" "$w/undefined-pairs.txt" "$w/duplicates.txt" "$w/imports.txt" "$rep/" 2>/dev/null
[ -f "$w/pass2.log" ] && cp "$w/pass2.log" "$rep/"
[ -z "${GITHUB_STEP_SUMMARY:-}" ] || head -c 900000 "$report" >> "$GITHUB_STEP_SUMMARY"
if [ $failed -ne 0 ]; then
    echo "::error title=link-check::see link-report.md (duplicates, undefined symbols, missing parts or forbidden imports)"
fi
exit $failed
