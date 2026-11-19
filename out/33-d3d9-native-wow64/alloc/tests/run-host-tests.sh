#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 Part J4: the guest-window allocator's host tests (window range, alignment, free and
# reuse, the 2 s block return with a fake clock, refusal on a native thread or without a
# window), with fakes of Wine's NtAllocateVirtualMemory / NtFreeVirtualMemory and ios_wow_base.
# Usage: run-host-tests.sh [build dir]. CC selects the C compiler (default cc); COMPILERS, when
# set, is a list of compilers to run it with instead (e.g. "gcc clang").

set -eu

here=$(cd "$(dirname "$0")" && pwd)
alloc="$here/.."
build=${1:-"$here/build"}
mkdir -p "$build"

for cc in ${COMPILERS:-${CC:-cc}}; do
    name=$(basename "$cc")
    $cc -std=c11 -O1 -g -Wall -Wextra -Werror -pthread -DIOS_WOW_WINDOW_FAKE_CLOCK -I"$alloc" \
        -o "$build/test_window_alloc-$name" "$here/test_window_alloc.c" "$alloc/ios_wow_window_alloc.c"
    "$build/test_window_alloc-$name"
done
echo "33-d3d9-native-wow64/alloc host tests: OK"
