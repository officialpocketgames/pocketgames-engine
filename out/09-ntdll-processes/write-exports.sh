#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 09 clarification 6: prints, one per line with Mach-O's leading "_", every symbol the
# loader can look up by name (ios_unixlib.c) for the libraries listed in unixlibs.txt:
# <name>_unix_call_funcs, <name>_unix_call_wow64_funcs and, for lines marked "init",
# <name>_unix_lib_init. <name> follows ios_unixlib_symbol (ios_pure.c): the file name without its
# last extension, lower case, anything but [a-z0-9_] as "_", and winemetal as dxmt_winemetal.
# tests/run-host-tests.sh checks that the two agree.
#
# usage: write-exports.sh [unixlibs.txt]   (default: the file beside this script)

set -eu
LIST=${1:-"$(dirname "$0")/unixlibs.txt"}

sed -e 's/#.*//' "$LIST" | while read -r dll flag; do
    [ -n "$dll" ] || continue
    base=$(printf '%s\n' "$dll" | sed -e 's/\.[^.]*$//' | tr 'A-Z' 'a-z' | sed -e 's/[^a-z0-9_]/_/g')
    if [ "$base" = winemetal ]; then base=dxmt_winemetal; fi
    printf '_%s_unix_call_funcs\n' "$base"
    printf '_%s_unix_call_wow64_funcs\n' "$base"
    if [ "${flag:-}" = init ]; then printf '_%s_unix_lib_init\n' "$base"; fi
done
