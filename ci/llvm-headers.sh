#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# CI: the LLVM 15.0.7 headers that out/05-dxmt-ios/build-ios.sh compiles airconv against
# (spec 05, "Allowed references": LLVM 15.0.7 from llvm.org). build-ios.sh only compiles, it
# links nothing from LLVM, so a host CMake configure plus the tablegen targets that generate
# headers (llvm/Config/*.h, llvm/IR/Attributes.inc, Intrinsics*.h, ...) is enough.
#
#   ci/llvm-headers.sh <dir>
#
# Leaves <dir>/llvm-src (llvm/include of the release, as build-ios.sh's <llvm-src-dir>) and
# <dir>/llvm-build (build-ios.sh's <llvm-build-dir>; only its include/ is kept). Skipped when
# <dir>/.ci-done exists. The release tarball's SHA-256 is checked against LLVM_SHA256 when set,
# else against the digest GitHub records for the release asset (gh api); with neither, the
# hash is printed with a warning.

set -eu

[ $# -eq 1 ] || { echo "usage: $0 <dir>" >&2; exit 2; }
mkdir -p "$1"
dir=$(cd "$1" && pwd)
version=15.0.7
name="llvm-project-$version.src"
url="https://github.com/llvm/llvm-project/releases/download/llvmorg-$version/$name.tar.xz"

if [ -f "$dir/.ci-done" ]; then
    echo "llvm-headers: $dir already done"
    exit 0
fi

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}

tarball="$dir/$name.tar.xz"
[ -f "$tarball" ] || curl -fL --retry 4 --retry-delay 10 -o "$tarball" "$url"
got=$(sha256_of "$tarball")
want=${LLVM_SHA256:-}
if [ -z "$want" ] && command -v gh >/dev/null 2>&1; then
    want=$(gh api "repos/llvm/llvm-project/releases/tags/llvmorg-$version" \
        --jq ".assets[] | select(.name == \"$name.tar.xz\") | .digest // empty" 2>/dev/null |
        sed 's/^sha256://' || true)
fi
if [ -n "$want" ]; then
    [ "$got" = "$want" ] || { echo "::error::llvm-headers: SHA-256 mismatch: got $got, expected $want"; rm -f "$tarball"; exit 1; }
    echo "llvm-headers: $name.tar.xz SHA-256 $got verified"
else
    echo "::warning::llvm-headers: no recorded SHA-256 for $name.tar.xz (set LLVM_SHA256); got $got"
fi

rm -rf "$dir/unpack" "$dir/llvm-src" "$dir/llvm-build" "$dir/b"
mkdir -p "$dir/unpack"
tar -xf "$tarball" -C "$dir/unpack" "$name/llvm" "$name/cmake"

cmake -G Ninja -S "$dir/unpack/$name/llvm" -B "$dir/b" \
    -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=AArch64 \
    -DLLVM_ENABLE_PROJECTS= -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_ZLIB=OFF \
    -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF > "$dir/cmake.log" 2>&1 ||
    { tail -n 40 "$dir/cmake.log"; echo "::error::llvm-headers: cmake configure failed"; exit 1; }

targets=""
for t in intrinsics_gen attributes_gen omp_gen acc_gen llvm_vcsrevision_h; do
    if ninja -C "$dir/b" -t query "$t" >/dev/null 2>&1; then targets="$targets $t"; fi
done
echo "llvm-headers: building$targets"
# shellcheck disable=SC2086
ninja -C "$dir/b" $targets > "$dir/ninja.log" 2>&1 ||
    { tail -n 40 "$dir/ninja.log"; echo "::error::llvm-headers: tablegen failed"; exit 1; }

mkdir -p "$dir/llvm-src" "$dir/llvm-build"
mv "$dir/unpack/$name/llvm/include" "$dir/llvm-src/include"
cp "$dir/unpack/$name/llvm/LICENSE.TXT" "$dir/llvm-src/" 2>/dev/null || true
mv "$dir/b/include" "$dir/llvm-build/include"
rm -rf "$dir/unpack" "$dir/b" "$tarball"
touch "$dir/.ci-done"
echo "llvm-headers: $dir/llvm-src/include and $dir/llvm-build/include ready"
