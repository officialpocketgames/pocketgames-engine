#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 08 Part E (clarification 5): builds and runs the host unit tests for the pure parts of
# thread suspension and contexts (server/ios_suspend.c) with any C11 compiler; the Mach calls
# are fakes. Usage: run-host-tests.sh [build-dir]. CC selects the compiler (default cc).

set -eu

here=$(cd "$(dirname "$0")" && pwd)
src="$here/../server"
build=${1:-"$here/build"}
cc=${CC:-cc}
flags="-std=c11 -O1 -Wall -Wextra -Werror"

mkdir -p "$build"

$cc $flags -I"$src" -o "$build/test_ios_suspend" "$here/test_ios_suspend.c" "$src/ios_suspend.c"

status=0
"$build/test_ios_suspend" || status=1

# device bug of build 9: set_queue_mask without a queue fd (server/ios_queue_pure.h)
$cc $flags -I"$src" -o "$build/test_queue_mask" "$here/test_queue_mask.c" && "$build/test_queue_mask" || status=1

# spec 17 Part J, Part H: sock.c's own state mapping, extracted, with Darwin's numbers; Wine's
# headers from WINE_SRC (default refs/wine-11.4 of this repository)
wine_src=${WINE_SRC:-"$here/../../../refs/wine-11.4"}
sed -n '/^static inline MIB_TCP_STATE tcp_state_to_mib_state/,/^}/p' "$src/sock.c" > "$build/tcp_map.inc"
if [ -s "$build/tcp_map.inc" ] && [ -f "$wine_src/include/tcpmib.h" ]; then
    $cc -std=gnu11 -O1 -Wall -Wextra -Werror -I"$src" -I"$build" -I"$wine_src/include" \
        -o "$build/test_s17_server" "$here/test_s17_server.c" && "$build/test_s17_server" || status=1
else
    echo "FAIL: no tcp_state_to_mib_state in $src/sock.c or no Wine tree at $wine_src"
    status=1
fi
# spec 00 F: no API an App Store build must not use
sh "$here/../../../check-app-store.sh" "$here/.." || status=1
exit $status
