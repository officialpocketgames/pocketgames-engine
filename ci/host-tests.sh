#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# CI: run every out/*/tests/run-host-tests.sh once per compiler, then check-app-store.sh on out/.
#
#   ci/host-tests.sh <wine-11.4 tree> <wine build dir> <log dir> <compiler pair>...
#
# A compiler pair is "<cc>:<c++>", e.g. "gcc-14:g++-14" "clang-21:clang++-21". Each run gets
# CC, CXX, HOSTCC and COMPILERS for that compiler, WINE_SRC and WINE_BUILD for the Wine tree,
# and the positional arguments its runner documents (spec 13: the tree and the build dir;
# spec 14: the build dir; specs 05, 08, 15 and 19: a build dir of their own). The runners of
# specs 02 and 18 loop over gcc and clang themselves and ignore CC, so they run once.
# Every run's output goes to <log dir>/<part>-<cc>.log; a summary table is printed (and added to
# $GITHUB_STEP_SUMMARY when set). Exits non-zero if any run or the App Store check failed.

set -u

[ $# -ge 4 ] || { echo "usage: $0 <wine tree> <wine build dir> <log dir> <cc:c++>..." >&2; exit 2; }
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
wine=$(cd "$1" && pwd)
wbuild=$(cd "$2" && pwd)
mkdir -p "$3"
logs=$(cd "$3" && pwd)
shift 3
scratch="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/host-tests"

self_looping="02-wine-bridge 18-pe-ntdll-wow64"
summary="$logs/summary.md"
printf '| part | compiler | result |\n|---|---|---|\n' > "$summary"
failed=0

for runner in "$repo"/out/*/tests/run-host-tests.sh; do
    part=$(basename "$(dirname "$(dirname "$runner")")")
    for pair in "$@"; do
        cc=${pair%%:*}
        cxx=${pair#*:}
        case " $self_looping " in
            *" $part "*)
                [ "$pair" = "$1" ] || continue
                label="own (gcc, clang)" ;;
            *) label=$cc ;;
        esac
        work="$scratch/$part-$cc"
        rm -rf "$work"
        mkdir -p "$work/tmp"
        case "$part" in
            13-*) args="$wine $wbuild" ;;
            14-*) args="$wbuild" ;;
            05-*|08-*|15-*|19-*) args="$work/build" ;;
            *) args="" ;;
        esac
        log="$logs/$part-$cc.log"
        echo "::group::$part ($label)"
        start=$(date +%s)
        # shellcheck disable=SC2086
        (
            cd "$(dirname "$(dirname "$runner")")" &&
            env CC="$cc" CXX="$cxx" HOSTCC="$cc" COMPILERS="$cc" \
                WINE_SRC="$wine" WINE_BUILD="$wbuild" TMPDIR="$work/tmp" \
                TEST_OUT="$work/test-out" FAKE_SCRATCH="$work/fake" \
                sh "$runner" $args
        ) > "$log" 2>&1
        rc=$?
        cat "$log"
        echo "::endgroup::"
        secs=$(( $(date +%s) - start ))
        if [ $rc -eq 0 ]; then
            result="pass (${secs}s)"
            if grep -qE '^SKIPPED|skipped' "$log"; then
                result="$result; skipped: $(grep -E '^SKIPPED|skipped' "$log" | head -n 3 | tr '\n' ' ' | cut -c1-200)"
            fi
        else
            result="**FAIL** (exit $rc, ${secs}s): $(grep -m 1 -iE 'error|fail' "$log" | cut -c1-200)"
            echo "::error title=host tests $part ($label)::$(grep -m 1 -iE 'error|fail' "$log" | cut -c1-300)"
            failed=1
        fi
        printf '| %s | %s | %s |\n' "$part" "$label" "$(printf '%s' "$result" | tr '|' '/')" >> "$summary"
    done
done

echo "::group::check-app-store.sh out/"
if sh "$repo/check-app-store.sh" "$repo/out" > "$logs/check-app-store.log" 2>&1; then
    printf '| out/ | check-app-store.sh | pass |\n' >> "$summary"
else
    printf '| out/ | check-app-store.sh | **FAIL** |\n' >> "$summary"
    echo "::error title=check-app-store.sh::forbidden API in out/ (see the log)"
    failed=1
fi
cat "$logs/check-app-store.log"
echo "::endgroup::"

cat "$summary"
[ -z "${GITHUB_STEP_SUMMARY:-}" ] || { echo "## Host tests"; cat "$summary"; } >> "$GITHUB_STEP_SUMMARY"
exit $failed
