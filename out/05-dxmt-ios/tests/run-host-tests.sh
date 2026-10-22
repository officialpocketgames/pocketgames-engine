#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 05 Part I, spec 06 Part F and spec 07 Part D: builds and runs the host unit tests (BC
# decoder, mip-clamp policy, 32-bit pointer/chain/record helpers, atomic wait,
# the DXSO 32-bit argument chain, the Direct3D 9 iOS rules)
# with any C++20 compiler. Usage: run-host-tests.sh [build-dir]
# CXX selects the compiler (default c++).

set -eu

here=$(cd "$(dirname "$0")" && pwd)
src="$here/../dxmt/src"
build=${1:-"$here/build"}
cxx=${CXX:-c++}
flags="-std=c++20 -O1 -Wall -Wextra -Werror"

mkdir -p "$build"

$cxx $flags -I"$src/dxmt" -o "$build/test_bc_decode" "$here/test_bc_decode.cpp" "$src/dxmt/dxmt_bc_decode.cpp"
$cxx $flags -I"$src/dxmt" -o "$build/test_mip_clamp" "$here/test_mip_clamp.cpp"
$cxx $flags -I"$src/winemetal" -I"$src/winemetal/unix" -o "$build/test_wow64" "$here/test_wow64.cpp"
$cxx $flags -pthread -I"$src/util" -o "$build/test_atomic_wait" "$here/test_atomic_wait.cpp"
$cxx $flags -I"$src/airconv" -I"$src/winemetal/unix" -o "$build/test_dxso_args32" "$here/test_dxso_args32.cpp"
$cxx $flags -I"$src/d3d9" -o "$build/test_d3d9_policy" "$here/test_d3d9_policy.cpp"
$cxx $flags -I"$src/d3d11" -o "$build/test_swap_effect" "$here/test_swap_effect.cpp"
# Dynamic-buffer discard: DXMT's own buffer code against fake Metal (bench_dynamic_discard.cpp).
# Optimised like the shipping build; run it by hand without --check for the 33k-cycle benchmark.
$cxx -std=c++20 -O2 -Wall -Wextra -Werror -DDXMT_PAGE_SIZE=4096 -DDXMT_NATIVE=1 -DDXMT_IOS=1 \
  -I"$src/dxmt" -I"$src/util" -I"$src/winemetal" -I"$src/airconv" -I"$src/../include" \
  -I"$src/../include/native/windows" -I"$src/../include/native/directx" \
  -o "$build/bench_dynamic_discard" "$here/bench_dynamic_discard.cpp" \
  "$src/dxmt/dxmt_dynamic.cpp" "$src/dxmt/dxmt_buffer.cpp" "$src/dxmt/dxmt_allocation.cpp" "$src/util/util_bloom.cpp"

status=0
"$build/test_bc_decode" || status=1
"$build/test_mip_clamp" || status=1
"$build/test_wow64" || status=1
"$build/test_atomic_wait" || status=1
"$build/test_dxso_args32" || status=1
"$build/test_d3d9_policy" || status=1
"$build/test_swap_effect" || status=1
"$build/bench_dynamic_discard" --check || status=1
exit $status
