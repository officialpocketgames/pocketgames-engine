#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 Part C: pecheck on synthetic images (written by mkpe).
#   test-pecheck.sh <pecheck binary> <mkpe binary> <scratch dir>

set -eu

pecheck=$1
mkpe=$2
dir=$3
failures=0

rm -rf "$dir"
mkdir -p "$dir"

bad() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

# a farm folder with the given files: newfarm <name>
newfarm() {
    rm -rf "${dir:?}/$1"
    mkdir -p "$dir/$1"
}

# expect_pass <farm> <folder> [pattern in the output]
expect_pass() {
    if "$pecheck" "$1" "$dir/$2" > "$dir/$2.out" 2>&1; then
        if [ $# -ge 3 ] && ! grep -q -- "$3" "$dir/$2.out"; then bad "$2: no '$3' in: $(cat "$dir/$2.out")"; fi
    else
        bad "$2: pecheck $1 failed: $(cat "$dir/$2.out")"
    fi
}

# expect_fail <farm> <folder> <pattern in the output>
expect_fail() {
    if "$pecheck" "$1" "$dir/$2" > "$dir/$2.out" 2>&1; then
        bad "$2: pecheck $1 passed, expected: $3"
    elif ! grep -q -- "$3" "$dir/$2.out"; then
        bad "$2: no '$3' in: $(cat "$dir/$2.out")"
    fi
}

ec="--machine 0x8664 --chpe"
a64="--machine 0xaa64"
x86="--machine 0x14c --pe32 --align 0x1000"
provided="--import d3d9.dll --import d3d10core.dll --import d3d11.dll --import dxgi.dll --import winemetal.dll --import nvapi64.dll"
provided2="--import d3d12.dll --import d3d12core.dll --import xtajit64.dll --import xtajit.dll"

# ---- good farms: imports inside the farm, API sets, every "provided by other parts" name ----
newfarm good-ec
"$mkpe" "$dir/good-ec/kernel32.dll" $ec
"$mkpe" "$dir/good-ec/User32.dll" $ec --import KERNEL32.dll --import api-ms-win-core-synch-l1-2-0.dll \
    --import ext-ms-win-gdi-dc-l1-2-0.dll
"$mkpe" "$dir/good-ec/game.dll" $ec $provided $provided2 --import user32.dll
printf 'trailing data, so this is the largest file' >> "$dir/good-ec/game.dll"
printf 'MSFT-not-a-PE' > "$dir/good-ec/stdole2.tlb"
expect_pass arm64ec-windows good-ec "arm64ec-windows: 4 files"

newfarm good-a64
"$mkpe" "$dir/good-a64/kernel32.dll" $a64
"$mkpe" "$dir/good-a64/cmd.exe" $a64 --import kernel32.dll --import api-ms-win-crt-runtime-l1-1-0.dll
expect_pass aarch64-windows good-a64 "aarch64-windows: 2 files"

newfarm good-x86
"$mkpe" "$dir/good-x86/kernel32.dll" $x86
"$mkpe" "$dir/good-x86/game.exe" $x86 --import kernel32.dll --import d3d9.dll
printf 'theme' > "$dir/good-x86/light.msstyles"
expect_pass i386-windows good-x86 "i386-windows: 3 files"

# the table: the largest file first
grep -A1 "largest files" "$dir/good-ec.out" | tail -n 1 | grep -q "game.dll" || bad "good-ec: largest file is not game.dll"

# ---- no builtin marker ----
newfarm nomarker
"$mkpe" "$dir/nomarker/a.dll" $a64 --no-marker
expect_fail aarch64-windows nomarker "no Wine builtin marker"

# ---- wrong machines ----
for c in "arm64ec-windows aarch64-in-ec $a64:machine aarch64, expected arm64ec" \
            "arm64ec-windows arm64x-in-ec --machine 0xa64e:ARM64X image" \
            "arm64ec-windows x64-in-ec --machine 0x8664:plain x86-64 image" \
            "arm64ec-windows i386-in-ec --machine 0x14c:machine i386, expected arm64ec" \
            "aarch64-windows x64-in-a64 --machine 0x8664:machine x86_64, expected aarch64" \
            "aarch64-windows ec-in-a64 $ec:machine arm64ec, expected aarch64" \
            "aarch64-windows arm64x-in-a64 --machine 0xa64e:ARM64X image" \
            "aarch64-windows hybrid-in-a64 --machine 0xaa64 --chpe:ARM64X image (machine 0xaa64 with hybrid metadata)" \
            "i386-windows x64-in-x86 --machine 0x8664 --pe32 --align 0x1000:machine x86_64, expected i386" \
            "i386-windows a64-in-x86 --machine 0xaa64 --pe32 --align 0x1000:machine aarch64, expected i386"; do
    farm=${c%% *}
    rest=${c#* }
    name=${rest%% *}
    opts=${rest#* }
    pattern=${opts#*:}
    opts=${opts%%:*}
    newfarm "$name"
    # shellcheck disable=SC2086
    "$mkpe" "$dir/$name/a.dll" $opts
    expect_fail "$farm" "$name" "$pattern"
done

# a data-only module (winebuild --data-only: no code section, no load configuration) in the
# ARM64EC farm: accepted with a note; the same image with code is plain x86-64 (above)
newfarm dataonly
"$mkpe" "$dir/dataonly/apisetschema.dll" --machine 0x8664 --data-only
expect_pass arm64ec-windows dataonly "note arm64ec-windows/apisetschema.dll: data-only image (no code section), accepted without ARM64EC metadata"
[ "$("$pecheck" --machine "$dir/dataonly/apisetschema.dll")" = x86_64-data ] || bad "--machine x86_64-data"

# ---- 64-bit layout ----
newfarm align4k
"$mkpe" "$dir/align4k/a.dll" $a64 --align 0x1000
expect_fail aarch64-windows align4k "section alignment 0x1000 is not a multiple of 0x4000"

newfarm align16k
"$mkpe" "$dir/align16k/a.dll" $a64 --align 0x4000
expect_pass aarch64-windows align16k "note aarch64-windows/a.dll: section alignment 0x4000 (expected 0x10000)"

newfarm pe32in64
"$mkpe" "$dir/pe32in64/a.dll" $a64 --pe32
expect_fail aarch64-windows pe32in64 "not PE32+"

newfarm datafirst
"$mkpe" "$dir/datafirst/a.dll" $ec --data-first
expect_fail arm64ec-windows datafirst "code section .text at 0x20000 comes after section .rdata at 0x10000"

newfarm debug
"$mkpe" "$dir/debug/a.dll" $a64 --debug-section
expect_fail aarch64-windows debug "section .debug_info left"

# ---- imports ----
newfarm missing
"$mkpe" "$dir/missing/a.dll" $ec --import nowhere.dll
expect_fail arm64ec-windows missing "imports nowhere.dll, which is neither in the farm"

newfarm missing-x86
"$mkpe" "$dir/missing-x86/a.dll" $x86 --import nowhere.dll
expect_fail i386-windows missing-x86 "imports nowhere.dll"

newfarm delay
"$mkpe" "$dir/delay/a.dll" $ec --delay optional.dll --delay a.dll --delay api-ms-win-core-com-l1-1-0.dll
"$mkpe" "$dir/delay/b.dll" $ec --delay OPTIONAL.dll --old-delay
expect_pass arm64ec-windows delay "optional.dll: a.dll b.dll"
grep -q "api-ms-win" "$dir/delay.out" && bad "delay: an API set was reported"
grep -q "^  a.dll" "$dir/delay.out" && bad "delay: a DLL in the farm was reported"

# ---- B6: flat folders ----
newfarm notflat
"$mkpe" "$dir/notflat/a.dll" $a64
mkdir "$dir/notflat/sub"
expect_fail aarch64-windows notflat "not a regular file"

newfarm hidden
"$mkpe" "$dir/hidden/a.dll" $a64
: > "$dir/hidden/.DS_Store"
expect_fail aarch64-windows hidden "hidden file"

newfarm symlink
"$mkpe" "$dir/symlink/a.dll" $a64
ln -s a.dll "$dir/symlink/b.dll"
expect_fail aarch64-windows symlink "symlink"

newfarm notpe
printf 'not a PE file at all, just text that is long enough to have an offset 0x3c' > "$dir/notpe/a.dll"
expect_fail aarch64-windows notpe "not a PE image"

# ---- the other modes ----
"$pecheck" --imports "$dir/good-ec/User32.dll" > "$dir/imports.out"
grep -qx "import KERNEL32.dll" "$dir/imports.out" || bad "--imports: no KERNEL32.dll"
grep -qx "import api-ms-win-core-synch-l1-2-0.dll" "$dir/imports.out" || bad "--imports: no API set"
"$pecheck" --imports "$dir/delay/b.dll" | grep -qx "delay OPTIONAL.dll" || bad "--imports: old-format delay import"
[ "$("$pecheck" --machine "$dir/good-ec/kernel32.dll")" = arm64ec ] || bad "--machine arm64ec"
[ "$("$pecheck" --machine "$dir/x64-in-ec/a.dll")" = x86_64 ] || bad "--machine x86_64"
[ "$("$pecheck" --machine "$dir/good-a64/kernel32.dll")" = aarch64 ] || bad "--machine aarch64"
[ "$("$pecheck" --machine "$dir/good-x86/kernel32.dll")" = i386 ] || bad "--machine i386"
[ "$("$pecheck" --machine "$dir/arm64x-in-ec/a.dll")" = arm64x ] || bad "--machine arm64x"
[ "$("$pecheck" --machine "$dir/good-ec/stdole2.tlb")" = - ] || bad "--machine not PE"
[ "$("$pecheck" --list-provided | wc -l | tr -d ' ')" = 10 ] || bad "--list-provided count"
"$pecheck" bogus-farm "$dir/good-ec" > /dev/null 2>&1 && bad "an unknown farm name was accepted"

if [ $failures -gt 0 ]; then
    echo "test-pecheck.sh: $failures failure(s)" >&2
    exit 1
fi
echo "test-pecheck.sh: all passed"
