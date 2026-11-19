#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 E1: the export table of a built d3d9.dll (names and ordinals, read with llvm-objdump -p)
# equals a .def file's EXPORTS. Exits non-zero, printing both lists, when they differ.
#
#   check_exports.sh <llvm-objdump> <d3d9.dll> <file.def>

set -eu
[ $# -eq 3 ] || { echo "usage: $0 <llvm-objdump> <dll> <def>" >&2; exit 2; }

want=$(tr -d '\r' < "$3" | awk '/^[[:space:]]+[A-Za-z0-9_]+[[:space:]]+@[0-9]+/ { o = $2; sub("@", "", o); print o, $1 }' | sort)
have=$("$1" -p "$2" | awk '/^Export Table:/ { t = 1; next } t && /^$/ { t = 0 } t && NF == 3 && $1 ~ /^[0-9]+$/ { print $1, $3 }' | sort)
if [ -z "$want" ] || [ "$want" != "$have" ]; then
    echo "check_exports: the exports of $2 differ from $3" >&2
    echo "want:" >&2; echo "$want" >&2
    echo "have:" >&2; echo "$have" >&2
    exit 1
fi
echo "check_exports: $(echo "$have" | wc -l | tr -d ' ') exports with the ordinals of $(basename "$3")"
