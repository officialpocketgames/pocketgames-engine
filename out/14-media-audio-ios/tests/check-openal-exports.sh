#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 14 round 4 build check: the exports of openal32.dll (openal32.spec), the function list
# (openal32_funcs.h) and the functions openal32_main.c defines name the same set, once each.
# Exits non-zero and prints the differences otherwise.

set -u
T=$(cd "$(dirname "$0")" && pwd)
D="$T/../pe/dlls/openal32"
W=${TMPDIR:-/tmp}/check-openal-exports.$$
mkdir -p "$W" || exit 1
trap 'rm -rf "$W"' EXIT

sed -n 's/^@ cdecl \([A-Za-z0-9_]*\)(.*/\1/p' "$D/openal32.spec" > "$W/spec"
sed -n 's/^ *OA(\([A-Za-z0-9_]*\),.*/\1/p' "$D/openal32_funcs.h" > "$W/list"

# openal32_main.c: the plain definitions, then EFX_OBJECT(Obj, Objs) expanded from its template
grep -v '##' "$D/openal32_main.c" | sed -n 's/^[A-Za-z].* AL_API \([A-Za-z0-9_]*\)(.*/\1/p' > "$W/defs"
grep '##' "$D/openal32_main.c" | sed -n 's/^[A-Za-z].* AL_API \([A-Za-z0-9_#]*\)(.*/\1/p' > "$W/efx"
sed -n 's/^EFX_OBJECT(\([A-Za-z]*\), *\([A-Za-z]*\))$/\1 \2/p' "$D/openal32_main.c" |
while read -r obj objs; do
  sed -e "s/##Objs/$objs/g" -e "s/##Obj/$obj/g" -e 's/##//g' "$W/efx"
done >> "$W/defs"

status=0
for f in spec list defs; do
  dup=$(sort "$W/$f" | uniq -d)
  [ -z "$dup" ] || { echo "check-openal-exports: $f names twice: $dup"; status=1; }
  sort -u "$W/$f" > "$W/$f.sorted"
done
for f in list defs; do
  if ! cmp -s "$W/spec.sorted" "$W/$f.sorted"; then
    echo "check-openal-exports: openal32.spec and $f differ (< spec, > $f):"
    diff "$W/spec.sorted" "$W/$f.sorted" | grep '^[<>]'
    status=1
  fi
done
n=$(wc -l < "$W/spec.sorted")
[ "$n" -gt 100 ] || { echo "check-openal-exports: only $n exports found"; status=1; }
[ $status -eq 0 ] && echo "check-openal-exports: $n exports match"
exit $status
