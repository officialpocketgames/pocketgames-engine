#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 clarification 6: builds the i386 thunk as a Wine builtin d3d9.dll, the way the app's DXMT
# PE build builds out/05-dxmt-ios/dxmt's i386 DLLs: meson, that tree's build-win-i686.txt
# (llvm-mingw, i686-w64-mingw32), -Dwine_builtin_dll=true, -Dwine_build_path=<a Wine i386 build>.
#
#   build-pe.sh <wine i386 build> <out dir>
#
# Writes <out dir>/i386-windows/d3d9.dll: the exports and ordinals of the full frontend's d3d9.def
# (thunk/d3d9.def), marked builtin by the Wine build's winebuild. The app's build puts it in place
# of the full frontend's i386 d3d9.dll and renames that one d3d9_dxmt32.dll (Part H).
#
# Environment:
#   LLVM_MINGW  the llvm-mingw toolchain directory (the one holding bin/); default: found from
#               i686-w64-mingw32-clang on PATH
#   WORK        the meson work directory; default <out dir>/work
#   WINE_SRC    a Wine 11.4 source tree; when set (or refs/wine-11.4 exists) the generator first
#               checks that gen/ matches its include/d3d9.h
# Needs meson (>= 1.3) and ninja. Never builds or writes inside refs/.

set -eu

[ $# -eq 2 ] || { echo "usage: $0 <wine i386 build> <out dir>" >&2; exit 2; }

part=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$part/../.." && pwd)
[ -d "$1" ] || { echo "build-pe: no Wine build at $1" >&2; exit 2; }
wine_build=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
work=${WORK:-"$out/work"}

# refs/ is read-only: no input build, work or output folder may resolve into it.
refs=$(cd "$repo/refs" 2>/dev/null && pwd -P || echo /nonexistent)
refuse_refs() {   # refuse_refs <what> <path>
    p=$2; rest=""
    while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
    case "$(cd "$p" && pwd -P)$rest/" in
        "$refs"/*) echo "build-pe: refusing $1 inside refs/ ($2): refs/ is read-only" >&2; exit 1 ;;
    esac
}
refuse_refs "the Wine build" "$wine_build"
refuse_refs "the output" "$out"
refuse_refs "the work directory" "$work"

if [ -z "${LLVM_MINGW:-}" ]; then
    cc=$(command -v i686-w64-mingw32-clang || true)
    [ -n "$cc" ] || { echo "build-pe: set LLVM_MINGW or put i686-w64-mingw32-clang on PATH" >&2; exit 2; }
    LLVM_MINGW=$(cd "$(dirname "$cc")/.." && pwd)
fi
[ -x "$LLVM_MINGW/bin/i686-w64-mingw32-clang" ] || { echo "build-pe: no i686-w64-mingw32-clang in $LLVM_MINGW/bin" >&2; exit 2; }
for tool in meson ninja; do
    command -v "$tool" > /dev/null || { echo "build-pe: $tool is needed" >&2; exit 2; }
done
for f in tools/winebuild/winebuild dlls/ntdll/i386-windows/libntdll.a; do
    [ -e "$wine_build/$f" ] || { echo "build-pe: $wine_build is not a Wine i386 build (no $f)" >&2; exit 2; }
done

# The generated stubs and vtables must match the table and d3d9.h (spec 33 F2).
wine_src=${WINE_SRC:-"$repo/refs/wine-11.4"}
if [ -z "${WINE_SRC:-}" ] && [ ! -d "$wine_src" ]; then
    # a git worktree inside the main checkout: refs/wine-11.4 is only there (not committed)
    d=$repo
    while [ "$d" != "/" ] && [ "$d" != "." ]; do
        d=$(dirname "$d")
        [ -f "$d/refs/wine-11.4/include/d3d9.h" ] && { wine_src="$d/refs/wine-11.4"; break; }
    done
fi
if [ -f "$wine_src/include/d3d9.h" ] && command -v python3 > /dev/null; then
    python3 -I "$part/tools/gen_d3d9w.py" "$wine_src/include/d3d9.h" --check
else
    echo "build-pe: no Wine source with include/d3d9.h: gen/ is used as committed"
fi

# The DXMT tree's i386 cross file, with this toolchain filled in.
mkdir -p "$work"
sed "s|@LLVM_MINGW_DIR@|$LLVM_MINGW|" "$repo/out/05-dxmt-ios/dxmt/build-win-i686.txt" > "$work/build-win-i686.txt"

rm -rf "$work/build"
meson setup "$work/build" "$part" \
    --cross-file "$work/build-win-i686.txt" \
    --prefix "$out" \
    --buildtype release \
    -Dwine_builtin_dll=true \
    -Dwine_build_path="$wine_build"
ninja -C "$work/build"
meson install -C "$work/build" --no-rebuild

dll="$out/i386-windows/d3d9.dll"
[ -f "$dll" ] || { echo "build-pe: $dll was not written" >&2; exit 1; }

# The exports and their ordinals, as the full frontend has them (llvm-mingw's objdump is
# llvm-objdump).
objdump="$LLVM_MINGW/bin/i686-w64-mingw32-objdump"
if [ -x "$objdump" ]; then
    sh "$part/tools/check_exports.sh" "$objdump" "$dll" "$part/thunk/d3d9.def"
fi
echo "build-pe: wrote $dll"
