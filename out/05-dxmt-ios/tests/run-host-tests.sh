#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 05 Part I, spec 06 Part F and spec 07 Part D: builds and runs the host unit tests (BC
# decoder, mip-clamp policy, 32-bit pointer/chain/record helpers, atomic wait,
# the DXSO 32-bit argument chain, the Direct3D 9 iOS rules, the Direct3D 9 call
# statistics and their call-site coverage; spec 05 clarification 10: the fixes adopted
# from steamac's DXMT patches; clarification 12: the shader cache's log lines; clarification
# 13: the six upstream DXMT shader fixes) with any C++20 compiler.
# Usage: run-host-tests.sh [build-dir]
# CXX selects the C++ compiler (default c++), CC the C compiler (default cc).

set -eu

here=$(cd "$(dirname "$0")" && pwd)
src="$here/../dxmt/src"
build=${1:-"$here/build"}
cxx=${CXX:-c++}
cc=${CC:-cc}
flags="-std=c++20 -O1 -Wall -Wextra -Werror"
native="$src/../include/native"

mkdir -p "$build"

$cxx $flags -I"$src/dxmt" -o "$build/test_bc_decode" "$here/test_bc_decode.cpp" "$src/dxmt/dxmt_bc_decode.cpp"
$cxx $flags -I"$src/dxmt" -o "$build/test_mip_clamp" "$here/test_mip_clamp.cpp"
$cxx $flags -I"$src/winemetal" -I"$src/winemetal/unix" -o "$build/test_wow64" "$here/test_wow64.cpp"
$cxx $flags -pthread -I"$src/util" -o "$build/test_atomic_wait" "$here/test_atomic_wait.cpp"
$cxx $flags -I"$src/airconv" -I"$src/winemetal/unix" -o "$build/test_dxso_args32" "$here/test_dxso_args32.cpp"
$cxx $flags -I"$src/d3d9" -o "$build/test_d3d9_policy" "$here/test_d3d9_policy.cpp"
# Spec 07 clarification 3: the call-statistics table and report, and the lists checked
# against d3d9.h's vtables (C, so d3d9.h declares each vtable as a struct).
$cxx $flags -I"$src/d3d9" -o "$build/test_d3d9_call_stats" "$here/test_d3d9_call_stats.cpp"
$cxx $flags -I"$src/util/log" -o "$build/test_log_cap" "$here/test_log_cap.cpp"
$cc -std=c11 -O1 -Wall -Wextra -Werror -I"$native/windows" -I"$native/directx" -I"$src/d3d9" \
  -o "$build/test_d3d9_call_list" "$here/test_d3d9_call_list.c"
$cxx $flags -I"$src/d3d11" -o "$build/test_swap_effect" "$here/test_swap_effect.cpp"
# Dynamic-buffer discard: DXMT's own buffer code against fake Metal (bench_dynamic_discard.cpp).
# Optimised like the shipping build; run it by hand without --check for the 33k-cycle benchmark.
$cxx -std=c++20 -O2 -Wall -Wextra -Werror -DDXMT_PAGE_SIZE=4096 -DDXMT_NATIVE=1 -DDXMT_IOS=1 \
  -I"$src/dxmt" -I"$src/util" -I"$src/winemetal" -I"$src/airconv" -I"$src/../include" \
  -I"$src/../include/native/windows" -I"$src/../include/native/directx" \
  -o "$build/bench_dynamic_discard" "$here/bench_dynamic_discard.cpp" \
  "$src/dxmt/dxmt_dynamic.cpp" "$src/dxmt/dxmt_buffer.cpp" "$src/dxmt/dxmt_allocation.cpp" "$src/util/util_bloom.cpp"
# Spec 05 clarification 10 (steamac's DXMT patches): 0019's slot reservation, 0017's integer
# render-target table, 0010/0011/0012's regression tests (with 0008's out-error slot), and
# 0002/0004/0023 against DXMT's own code with fake Metal.
# (DXMT's native Win32 shims, util_win32_compat.h and log.hpp, need __cdecl and have unused
# parameters; that is upstream's header, not the code under test.)
native_flags="-DDXMT_PAGE_SIZE=4096 -DDXMT_NATIVE=1 -DDXMT_IOS=1 -D__cdecl= -Wno-unused-parameter \
  -I$src/../include -I$src/../include/native/windows -I$src/../include/native/directx"
$cxx $flags -pthread -I"$src/dxmt" -I"$src/util" -o "$build/test_chunk_slots" "$here/test_chunk_slots.cpp"
$cxx $flags -DDXMT_NATIVE=1 -I"$src/airconv" -I"$src/winemetal" -o "$build/test_integer_rt_formats" \
  "$here/test_integer_rt_formats.cpp"
$cxx $flags -pthread $native_flags -I"$src/dxmt" -I"$src/util" -I"$src/winemetal" \
  -o "$build/test_scheduler_lifetime" "$here/test_scheduler_lifetime.cpp"
$cxx $flags $native_flags -I"$src/winemetal" -o "$build/test_metal_reference" "$here/test_metal_reference.cpp"
$cxx $flags -I"$src/d3d11" -I"$src/airconv" -o "$build/test_shader_bitcode" "$here/test_shader_bitcode.cpp"
$cxx $flags -pthread $native_flags -I"$src/dxmt" -I"$src/util" -I"$src/winemetal" -I"$src/airconv" \
  -o "$build/test_core_fixes" "$here/test_core_fixes.cpp" "$src/dxmt/dxmt_allocation.cpp"
# Spec 05 clarification 12: when the shader cache's lookups are logged, and what a cache
# database holds (on small SQLite databases; the system's libsqlite3 with the declarations in
# tests/sqlite3-api, so no SQLite development package is needed; skipped without the library).
$cxx $flags -I"$src/dxmt" -o "$build/test_shader_cache_log" "$here/test_shader_cache_log.cpp"
# Spec 05 clarification 13 (six upstream DXMT fixes): the cache version and the conversion
# flag; check-shader-fixes.sh checks the converter's source (the converter needs LLVM 15).
$cxx $flags $native_flags -I"$src/dxmt" -I"$src/util" -I"$src/winemetal" -I"$src/airconv" \
  -o "$build/test_shader_fixes" "$here/test_shader_fixes.cpp"
# Spec 05 clarification 17: FORMAT_SUPPORT2's typed UAV bits over the Apple7 table
# (dxmt_format.cpp with a fake device) and the read-write UAV log line.
$cxx $flags $native_flags -I"$src/dxmt" -I"$src/d3d11" -I"$src/util" -I"$src/winemetal" -I"$src/airconv" \
  -o "$build/test_format_support2" "$here/test_format_support2.cpp" "$src/dxmt/dxmt_format.cpp"
# Spec 05 clarifications 19 and 21: the constant-buffer bind rule, the frame trace's formats
# and cap (clarification 20's layouts are in test_wow64).
$cxx $flags -I"$src/d3d11" -o "$build/test_constant_buffer_bind" "$here/test_constant_buffer_bind.cpp"
$cxx $flags $native_flags -I"$src/dxmt" -I"$src/util" -I"$src/winemetal" -I"$src/airconv" \
  -o "$build/test_frame_trace" "$here/test_frame_trace.cpp"
# Spec 05 clarification 23: Metal.hpp's compute-pipeline wrappers set every field (fake
# MTLDevice_newComputePipelineState over a dirty stack), and the unix side's threads guard.
$cxx $flags $native_flags -I"$src/winemetal" -I"$src/winemetal/unix" \
  -o "$build/test_compute_pipeline_info" "$here/test_compute_pipeline_info.cpp"
# Spec 05 clarification 25: the memory barrier before an indirect dispatch.
$cxx $flags -I"$src/winemetal" -I"$src/dxmt" -o "$build/test_indirect_dispatch" "$here/test_indirect_dispatch.cpp"
# Spec 05 clarification 28: the frame dump's switch, formats, names and limits.
$cxx $flags $native_flags -I"$src/dxmt" -I"$src/util" -I"$src/winemetal" -I"$src/airconv" \
  -o "$build/test_frame_dump" "$here/test_frame_dump.cpp"
# Spec 05 clarification 54: commits held while the app isn't active (C and C++).
$cc -std=c11 -O1 -Wall -Wextra -Werror -pthread -I"$src/winemetal/unix" \
  -o "$build/test_app_active" "$here/test_app_active.c"
$cxx $flags -Wno-unused-function -pthread -x c++ -fsyntax-only "$src/winemetal/unix/app_active.h"
# Spec 05 clarification 57: PG_DXMT_SERIALIZE's levels, the optimiser's loop, the waits.
$cxx $flags -I"$src/dxmt" -o "$build/test_serialize" "$here/test_serialize.cpp"
# Spec 05 clarification 30: the frame trace's state lines.
$cxx $flags $native_flags -I"$src/d3d11" -I"$src/dxmt" -I"$src/util" -I"$src/winemetal" -I"$src/airconv" \
  -o "$build/test_trace_state" "$here/test_trace_state.cpp"
sqlite_lib=
for candidate in -lsqlite3 -l:libsqlite3.so.0; do
  if echo 'int main(void) { return 0; }' | $cc -x c - -o "$build/sqlite_probe" $candidate 2>/dev/null; then
    sqlite_lib=$candidate
    break
  fi
done
if [ -n "$sqlite_lib" ]; then
  $cc -std=c11 -O1 -Wall -Wextra -Werror -I"$here/sqlite3-api" -I"$src/winemetal/unix" \
    -o "$build/test_cache_contents" "$here/test_cache_contents.c" $sqlite_lib
fi

status=0
"$build/test_bc_decode" || status=1
"$build/test_mip_clamp" || status=1
"$build/test_wow64" || status=1
"$build/test_atomic_wait" || status=1
"$build/test_dxso_args32" || status=1
"$build/test_d3d9_policy" || status=1
"$build/test_d3d9_call_stats" || status=1
"$build/test_log_cap" || status=1
"$build/test_d3d9_call_list" || status=1
sh "$here/check-d3d9-call-sites.sh" "$src/d3d9" || status=1
"$build/test_swap_effect" || status=1
"$build/bench_dynamic_discard" --check || status=1
"$build/test_chunk_slots" || status=1
"$build/test_integer_rt_formats" || status=1
"$build/test_scheduler_lifetime" || status=1
"$build/test_metal_reference" || status=1
"$build/test_shader_bitcode" || status=1
"$build/test_core_fixes" || status=1
"$build/test_shader_cache_log" || status=1
"$build/test_shader_fixes" || status=1
"$build/test_format_support2" || status=1
"$build/test_constant_buffer_bind" || status=1
"$build/test_frame_trace" || status=1
"$build/test_compute_pipeline_info" || status=1
"$build/test_indirect_dispatch" || status=1
"$build/test_frame_dump" || status=1
"$build/test_trace_state" || status=1
"$build/test_app_active" || status=1
"$build/test_serialize" || status=1
sh "$here/check-shader-fixes.sh" "$src" || status=1
if [ -n "$sqlite_lib" ]; then
  "$build/test_cache_contents" "$build" || status=1
else
  echo "test_cache_contents: skipped (no libsqlite3 on this host)"
fi
exit $status
