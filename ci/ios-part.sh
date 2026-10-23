#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# CI (macOS, Xcode's iPhoneOS SDK): build one part's iOS static libraries with its own
# build-ios.sh, as its TESTS.md / CHANGES.md describe, and collect them.
#
#   ci/ios-part.sh <part> <work dir> <dist dir>
#
# <part> is a folder name under out/ that has a build-ios.sh: 05-dxmt-ios (also specs 06 and 07,
# whose code is in its tree), 08-wineserver-ios, 09-ntdll-processes (also specs 10 and 11, and
# spec 16's src/*.c and spec 13's socket.c, which its build-ios.sh takes itself),
# 12-win32u-ios, 13-net-crypto-ios, 14-media-audio-ios, 15-controllers. Spec 16 and spec 17
# have no build-ios.sh of their own: their unix-side code is compiled by 08, 09, 12-15's.
# <work dir> must hold:
#   wine-host/   ci/wine-host-build.sh's Wine build dir (macOS configure + make include/all)
#   dl/          the reference tarballs of ci/fetch-refs.sh (14 takes FFmpeg's from there)
# and keeps per-part prerequisites, which CI caches: llvm/ (05), cryptodeps/ (13), ffmpeg/ (14).
# <dist dir>/<part>/ receives the static libraries, LINK.txt / exports.txt where the part
# writes them, build.log and the part's own logs. Exits with the build's status.

set -u

# keep_going <build script>: prints the script to run. A build-ios.sh that stops at its first
# file that fails to compile (an `exit 1` within two lines after a "failed to compile" message:
# specs 08 and 15) is run as a copy beside it in which that exit records the failure and goes on
# with the next file (`continue` in a loop, `return 0` in a function), and an EXIT trap fails the
# script at the end; so one CI run lists every file that fails. Other scripts run as they are.
keep_going() {
    copy="$(dirname "$1")/.ci-keep-going-$(basename "$1")"
    if awk '
        /^[A-Za-z_][A-Za-z0-9_]*\(\) *\{/ { infn = 1 }
        /^\}/ { infn = 0 }
        /failed to compile/ { pending = 3 }
        pending > 0 && /^[ \t]*exit 1[ \t]*$/ {
            ind = $0; sub(/exit 1.*/, "", ind)
            print ind "CI_FAILED=1; echo \"ci keep-going: continuing with the next file\" >&2; " (infn ? "return 0" : "continue")
            pending = 0; n++; next
        }
        pending > 0 { pending-- }
        { print }
        /^set -e/ && !trapped {
            print "trap '\''[ -z \"${CI_FAILED:-}\" ] || { echo \"ci keep-going: files failed to compile (errors above)\" >&2; exit 1; }'\'' EXIT"
            trapped = 1
        }
        END { if (n == 0 || !trapped) exit 3 }
    ' "$1" > "$copy" && sh -n "$copy"; then
        echo "$copy"
    else
        rm -f "$copy"
        echo "$1"
    fi
}

# ci/ios-part.sh --keep-going-copy <build script>: only make the copy and print its path
# (ci/check-ci.sh checks the rewrite on specs 08 and 15)
if [ "${1:-}" = --keep-going-copy ]; then
    keep_going "$2"
    exit 0
fi

[ $# -eq 3 ] || { echo "usage: $0 <part> <work dir> <dist dir>" >&2; exit 2; }
part=$1
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
mkdir -p "$2" "$3/$part"
work=$(cd "$2" && pwd)
dist=$(cd "$3/$part" && pwd)
wine="$repo/refs/wine-11.4"
wbuild="$work/wine-host"
out="$work/out-$part"
src="$repo/out/$part"
log="$dist/build.log"
: > "$log"

say() { echo "$*" | tee -a "$log"; }
# run <what> <command...>: the command's output goes to the job log and build.log
run() {
    what=$1; shift
    say "== $what: $*"
    "$@" > "$dist/.step.log" 2>&1
    rc=$?
    cat "$dist/.step.log"
    cat "$dist/.step.log" >> "$log"
    rm -f "$dist/.step.log"
    say "== $what: exit $rc"
    return $rc
}


say "ci/ios-part.sh $part on $(sw_vers -productVersion 2>/dev/null || uname -sr), $(xcodebuild -version 2>/dev/null | tr '\n' ' ')"
say "iPhoneOS SDK: $(xcrun --sdk iphoneos --show-sdk-version 2>/dev/null || echo none) at $(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null)"
[ -f "$wbuild/include/config.h" ] || { say "::error::ios-part: no Wine host build at $wbuild"; exit 1; }
rm -rf "$out"
mkdir -p "$out"

status=0
case "$part" in
    05-dxmt-ios)
        run "LLVM 15.0.7 headers" sh "$here/llvm-headers.sh" "$work/llvm" || exit 1
        run "build-ios.sh" sh "$src/build-ios.sh" "$src/dxmt" "$work/llvm/llvm-build" "$work/llvm/llvm-src" "$out" || status=1
        ;;
    08-wineserver-ios|09-ntdll-processes|12-win32u-ios|15-controllers)
        script=$(keep_going "$src/build-ios.sh")
        [ "$script" = "$src/build-ios.sh" ] || say "== build-ios.sh stops at its first failing file: running $(basename "$script"), which goes on"
        run "build-ios.sh" sh "$script" "$wine" "$wbuild" "$out" || status=1
        [ "$script" = "$src/build-ios.sh" ] || rm -f "$script"
        ;;
    13-net-crypto-ios)
        # build-crypto-deps-ios.sh (spec 13 clarification 7.1): BearSSL cloned at its pinned
        # commit and LibTomCrypt copied out of the Wine tree, built into <prefix>; a library
        # whose stamp is there (the cached cryptodeps/) is not built again
        if run "build-crypto-deps-ios.sh" sh "$src/build-crypto-deps-ios.sh" "$work/cryptodeps" "$wine"; then
            run "build-ios.sh" sh "$src/build-ios.sh" "$wine" "$wbuild" "$out" "$work/cryptodeps" || status=1
        else
            status=1
            mkdir -p "$dist/cryptodeps-logs"
            cp "$work/cryptodeps/.build/"*.log "$dist/cryptodeps-logs/" 2>/dev/null || true
        fi
        ;;
    14-media-audio-ios)
        if [ -f "$work/ffmpeg/lib/libavcodec.a" ] && [ -f "$work/ffmpeg/.ci-done" ] &&
           [ "$(cat "$work/ffmpeg/.ci-done")" = "$(cksum < "$src/build-ffmpeg.sh")" ]; then
            say "== build-ffmpeg.sh: $work/ffmpeg already built by this build-ffmpeg.sh (cache)"
        else
            # WORKDIR: build-ffmpeg.sh finds the unpacked tree with find <WORKDIR> -name 'ffmpeg-*', which
            # also matches a WORKDIR named ffmpeg-* (its default, ffmpeg-build), so the name differs
            rm -rf "$work/ffmpeg"
            if run "build-ffmpeg.sh" env WORKDIR="$work/ff-unpack" sh "$src/build-ffmpeg.sh" "$work/dl/ffmpeg-7.1.1.tar.xz" "$work/ffmpeg"; then
                cksum < "$src/build-ffmpeg.sh" > "$work/ffmpeg/.ci-done"
            else
                status=1
                [ -f "$work/ff-unpack/ffmpeg-7.1.1/ffbuild/config.log" ] &&
                    tail -n 200 "$work/ff-unpack/ffmpeg-7.1.1/ffbuild/config.log" > "$dist/ffmpeg-config.log.tail"
            fi
        fi
        if [ $status -eq 0 ]; then
            run "build-ios.sh" sh "$src/build-ios.sh" "$wine" "$wbuild" "$work/ffmpeg" "$out" || status=1
            mkdir -p "$dist/ffmpeg"
            cp "$work/ffmpeg/lib/"*.a "$dist/ffmpeg/" 2>/dev/null || true
        fi
        ;;
    *)
        say "::error::ios-part: unknown part $part"
        exit 2
        ;;
esac

# collect: the archives of a part that built (FreeType's own libfreetype.a stays out: it is
# merged into libwin32u_unix.a and must not be linked twice; a failed part's partial archives
# stay out too, so link-check reports the part as missing), the link notes and every log
if [ $status -eq 0 ]; then
    find "$out" -maxdepth 1 -name '*.a' ! -name libfreetype.a -exec cp {} "$dist/" \;
fi
for f in LINK.txt exports.txt; do [ -f "$out/$f" ] && cp "$out/$f" "$dist/"; done
for d in logs log gen; do [ -d "$out/$d" ] && cp -R "$out/$d" "$dist/$d"; done

say "== collected in $dist:"
ls -l "$dist" | tee -a "$log"
if [ $status -ne 0 ]; then
    # the first compiler error, for the job summary
    first=$(grep -m 1 -E 'error:|FAILED|Error [0-9]' "$log" || true)
    echo "::error title=ios-libs $part::${first:-build failed, see build.log}"
fi
exit $status
