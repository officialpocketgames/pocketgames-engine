#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 Part C: build-pe.sh plan, against refs/wine-11.4 with the real overlays, and against
# fixture overlay folders that break B2 item 3 (two overlays delivering one path, a unix-side
# file, a server file).
#   test-plan.sh <scratch dir>
# WINE_SRC is the Wine 11.4 tree (default refs/wine-11.4).

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
out_root=$(cd "$top/.." && pwd)
repo=$(cd "$out_root/.." && pwd)
wine=${WINE_SRC:-"$repo/refs/wine-11.4"}
dir=$1
script="$top/build-pe.sh"
failures=0

bad() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

if [ ! -f "$wine/VERSION" ]; then
    echo "SKIPPED: test-plan.sh (no Wine 11.4 tree at $wine; set WINE_SRC)"
    exit 0
fi
rm -rf "$dir"
mkdir -p "$dir"

# ---- the real overlays ----
if bash "$script" --wine "$wine" --work "$dir/work" --allow-missing-overlays plan > "$dir/real.out" 2>&1; then
    for want in "overlay spec17-include (spec 17): 09-ntdll-processes/include -> include" \
                "    dlls/ntdll/unixlib.h" \
                "    dlls/mscoree/metahost.c" \
                "    dlls/dinput/joystick_ios.c  new" \
                "    dlls/nsi/nsi.c" \
                "    dlls/kernelbase/tick_convert.h  new" \
                "== farm arm64ec-windows" \
                "dlls/kernelbase/arm64ec-windows/kernelbase.dll" \
                "dlls/bthprops.cpl/arm64ec-windows/bthprops.cpl" \
                "dlls/winspool.drv/arm64ec-windows/winspool.drv" \
                "programs/services/aarch64-windows/services.exe" \
                "dlls/ntoskrnl.exe/aarch64-windows/ntoskrnl.exe" \
                "dlls/wow64/aarch64-windows/wow64.dll" \
                "== farm i386-windows"; do
        grep -qxF -- "$want" "$dir/real.out" || grep -qF -- "$want" "$dir/real.out" || bad "plan: no '$want'"
    done
    grep -q "^ERROR" "$dir/real.out" && bad "plan: errors with the real overlays: $(grep '^ERROR' "$dir/real.out")"
    # every name of both 64-bit lists is a module of the tree
    [ "$(grep -c '/arm64ec-windows/' "$dir/real.out")" = "$(grep -vc '^#' "$top/farms/arm64ec-windows.txt")" ] ||
        bad "plan: arm64ec target count"
    # no programs in the arm64ec farm
    grep -q '^programs/.*/arm64ec-windows/' "$dir/real.out" && bad "plan: a program in the arm64ec farm"
else
    bad "plan with the real overlays failed: $(cat "$dir/real.out")"
fi

# without --allow-missing-overlays a missing overlay is an error naming it
missing=""
for d in 09-ntdll-processes/include 09-ntdll-processes/ntdll-unix/unixlib.h 14-media-audio-ios/pe/metahost.c \
         15-controllers/pe 16-sync-ios/pe 18-pe-ntdll-wow64/pe 19-pe-dlls-build/pe; do
    [ -e "$out_root/$d" ] || missing="$missing $d"
done
if bash "$script" --wine "$wine" --work "$dir/work" plan > "$dir/strict.out" 2>&1; then
    [ -z "$missing" ] || bad "plan passed with missing overlays:$missing"
else
    if [ -z "$missing" ]; then
        bad "plan failed with every overlay present: $(grep ERROR "$dir/strict.out")"
    else
        for d in $missing; do
            grep -q "ERROR: missing overlay .*$d" "$dir/strict.out" || bad "plan: the missing overlay $d isn't named"
        done
    fi
fi

# ---- fixture: two overlays deliver the same path ----
fx="$dir/dup/out"
mkdir -p "$fx/15-controllers/pe/dlls/dinput" "$fx/18-pe-ntdll-wow64/pe/dlls/dinput" "$fx/19-pe-dlls-build/pe/dlls/nsi"
echo one > "$fx/15-controllers/pe/dlls/dinput/dinput.c"
echo two > "$fx/18-pe-ntdll-wow64/pe/dlls/dinput/dinput.c"
echo ok > "$fx/19-pe-dlls-build/pe/dlls/nsi/nsi.c"
if bash "$script" --wine "$wine" --out-root "$fx" --work "$dir/work-dup" --allow-missing-overlays plan > "$dir/dup.out" 2>&1; then
    bad "plan passed with two overlays delivering dlls/dinput/dinput.c"
else
    grep -q "ERROR: dlls/dinput/dinput.c is delivered by two overlays: spec15 and spec18" "$dir/dup.out" ||
        bad "plan: the duplicate isn't named with both overlays: $(grep ERROR "$dir/dup.out")"
fi
# and the tree step refuses it too, before copying anything
if bash "$script" --wine "$wine" --out-root "$fx" --work "$dir/work-dup" --allow-missing-overlays tree > "$dir/dup-tree.out" 2>&1; then
    bad "tree passed with two overlays delivering one path"
else
    grep -q "step 'tree' failed" "$dir/dup-tree.out" || bad "tree: no failure line naming the step"
    [ -d "$dir/work-dup/src" ] && bad "tree: copied the tree despite the error"
fi

# ---- fixture: a unix-side file and a server file ----
fx="$dir/unix/out"
mkdir -p "$fx/19-pe-dlls-build/pe/dlls/x/unix" "$fx/16-sync-ios/pe/server"
echo x > "$fx/19-pe-dlls-build/pe/dlls/x/unix/b.c"
echo y > "$fx/16-sync-ios/pe/server/thread.c"
if bash "$script" --wine "$wine" --out-root "$fx" --work "$dir/work-unix" --allow-missing-overlays plan > "$dir/unix.out" 2>&1; then
    bad "plan passed with a dlls/x/unix/ file"
else
    grep -q "ERROR: dlls/x/unix/b.c (overlay spec19) is a unix-side file" "$dir/unix.out" || bad "plan: the unix file isn't named"
    grep -q "ERROR: server/thread.c (overlay spec16) is a unix-side file" "$dir/unix.out" || bad "plan: the server file isn't named"
fi

if [ $failures -gt 0 ]; then
    echo "test-plan.sh: $failures failure(s)" >&2
    exit 1
fi
echo "test-plan.sh: all passed"
