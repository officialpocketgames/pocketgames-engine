#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 Part C: build-pe.sh's whole flow (toolchain, tree, the three trees, collect) on a fake
# Wine tree and a fake toolchain, in seconds. The fake tree has one Makefile.in per module of the
# farm lists (and a few more), and a stub configure that writes a Makefile whose rules make each
# module file with mkpe (machine and imports per module), as the real tree's rules would make a
# PE file. The fake toolchain's clang and strip do nothing. What it can't check is the real
# compile; everything around it is checked: the order of the trees and the tools tree, the
# configure arguments, file targets, the -j1 retry and the failure report, the type library
# link, the i386 rule and skip list, the import closure over two rounds, --extra, strip and
# re-marking, pecheck, NLS, MANIFEST.txt, BUILDINFO.txt, the layout of <dest>, and the stamps.
#   test-build-fake.sh <mkpe> <scratch dir>

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
mkpe=$1
dir=$2
script="$top/build-pe.sh"
# the number of names in each 64-bit list (the farms hold these, plus the closure and extras)
ec_listed=$(sed -e 's/#.*//' "$top/farms/arm64ec-windows.txt" | awk 'NF' | wc -l | tr -d ' ')
a64_listed=$(sed -e 's/#.*//' "$top/farms/aarch64-windows.txt" | awk 'NF' | wc -l | tr -d ' ')
failures=0

bad() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}
want() { # <file> <fixed string>
    grep -qF -- "$2" "$1" || bad "no '$2' in $1"
}

for tool in make bison flex; do
    if ! command -v $tool >/dev/null 2>&1; then
        echo "SKIPPED: test-build-fake.sh ($tool not installed)"
        exit 0
    fi
done

rm -rf "$dir"
mkdir -p "$dir"
dir=$(cd -P "$dir" && pwd -P)   # build-pe.sh works with physical paths
wine="$dir/wine"
out="$dir/out"
tc="$dir/toolchain"

# ---- the fake toolchain ----
mkdir -p "$tc/bin"
for cpu in aarch64 arm64ec i686 x86_64; do
    for t in clang strip; do
        printf '#!/bin/sh\nexit 0\n' > "$tc/bin/$cpu-w64-mingw32-$t"
        chmod +x "$tc/bin/$cpu-w64-mingw32-$t"
    done
done
for t in clang lld-link; do
    printf '#!/bin/sh\nexit 0\n' > "$tc/bin/$t"
    chmod +x "$tc/bin/$t"
done

# ---- the fake Wine tree ----
mkdir -p "$wine/nls"
echo "Wine version 11.4-fake" > "$wine/VERSION"
printf 'nls' > "$wine/nls/c_932.nls"
printf 'nls' > "$wine/nls/l_intl.nls"
module() { # <name> [mkpe options for its imports]
    name=$1
    shift
    case "$name" in
        *.exe) d="programs/${name%.exe}" ;;
        *) d="dlls/$name" ;;
    esac
    mkdir -p "$wine/$d"
    printf 'MODULE = %s\nFAKE_IMPORTS = %s\n' "$name" "$*" > "$wine/$d/Makefile.in"
}
for name in $(sed -e 's/#.*//' "$top/farms/arm64ec-windows.txt" "$top/farms/aarch64-windows.txt" | awk 'NF' | sort -u); do
    module "$name"
done
# the closure: kernel32 imports extra1 (not listed), which imports extra2; API sets, a provided
# name and a missing delay import don't count
# a module compiling another folder's sources (as d3dcompiler_47 does d3dcompiler_43's)
echo "PARENTSRC = ../nsi.dll" >> "$wine/dlls/d3dcompiler_47.dll/Makefile.in"
module kernel32.dll --import ntdll.dll --import extra1.dll --import api-ms-win-core-synch-l1-2-0.dll \
    --import d3d11.dll --delay optional.dll
module extra1.dll --import extra2.dll
module extra2.dll --import kernel32.dll
# i386-only material: skipped names, a driver, a 16-bit module, a type library
module wow64cpu.dll
module winex11.drv
module d3d11.dll
module hidclass.sys
module krnl386.exe16
module stdole2.tlb
# the add-on installer's module (App Review 2.5.2 check), and what a FAKE_TAINT build appends to
# it: "winemono" in UTF-16, as resource text would hold it
module appwiz.cpl
printf '#!/bin/sh\nprintf "w\\000i\\000n\\000e\\000m\\000o\\000n\\000o\\000" >> "$1"\n' > "$wine/taint.sh"
cat > "$wine/configure" <<'EOF'
#!/bin/sh
# fake configure: a Makefile with one file rule per module, made by mkpe
src=$(cd "$(dirname "$0")" && pwd)
arch=; tools=
for a in "$@"; do
    case "$a" in
        --enable-archs=*) arch=${a#*=} ;;
        --with-wine-tools=*) tools=${a#*=} ;;
    esac
done
echo "$*" > configure.args
case "$arch" in
    arm64ec) mach="--machine 0x8664 --chpe" ;;
    aarch64) mach="--machine 0xaa64" ;;
    i386) mach="--machine 0x14c --pe32 --align 0x1000" ;;
esac
{
    echo "all:"
    if [ -z "$tools" ]; then
        echo "__tooldeps__:"
        printf '\tmkdir -p tools/winebuild && printf "#!/bin/sh\\nexit 0\\n" > tools/winebuild/winebuild && chmod +x tools/winebuild/winebuild && printf "#!/bin/sh\\n" > tools/makedep && chmod +x tools/makedep\n'
    fi
    for mk in "$src"/dlls/*/Makefile.in "$src"/programs/*/Makefile.in; do
        d=${mk#"$src"/}
        d=${d%/Makefile.in}
        m=$(awk '/^MODULE/ { print $3 }' "$mk")
        imports=$(sed -n 's/^FAKE_IMPORTS = //p' "$mk")
        echo "$d/$arch-windows/$m:"
        if [ "$m" = "$FAKE_BREAK" ]; then
            printf '\t@echo "error: $@: fake failure" >&2; false\n'
        elif [ "$m" = "$FAKE_TAINT" ]; then
            printf '\tmkdir -p $(@D) && %s $@ %s %s && sh %s/taint.sh $@\n' "$MKPE" "$mach" "$imports" "$src"
        else
            printf '\tmkdir -p $(@D) && %s $@ %s %s\n' "$MKPE" "$mach" "$imports"
        fi
    done
} > Makefile
EOF
chmod +x "$wine/configure"

# ---- one overlay (spec 19's), so MANIFEST.txt has an origin with a spec ----
mkdir -p "$out/19-pe-dlls-build/pe/dlls/nsi.dll"
echo "/* changed */" > "$out/19-pe-dlls-build/pe/dlls/nsi.dll/nsi.c"
# ---- --extra: DXMT's d3d11.dll (a provided name) and one more DLL ----
mkdir -p "$dir/extra"
"$mkpe" "$dir/extra/d3d11.dll" --machine 0x8664 --chpe --import kernel32.dll
"$mkpe" "$dir/extra/MyDxmt.dll" --machine 0x8664 --chpe --import d3d11.dll --import kernel32.dll

export MKPE="$mkpe"
# CC=clang names the host's clang, while the toolchain (here a do-nothing stub) also has a bin/clang
# that build-pe.sh puts first on PATH: pecheck must still be built by the host's
if command -v clang >/dev/null 2>&1; then CC=clang; export CC; fi
run() { # <log> <args...>
    log=$1
    shift
    bash "$script" --wine "$wine" --out-root "$out" --toolchain "$tc" --jobs 4 --allow-missing-overlays "$@" > "$log" 2>&1
}

# ---- the whole flow ----
if run "$dir/all.out" --work "$dir/work" --dest "$dir/dest" --extra "arm64ec-windows=$dir/extra" all; then
    d="$dir/dest"
    [ "$(cd "$d" && LC_ALL=C ls -A | tr '\n' ' ')" = "BUILDINFO.txt MANIFEST.txt aarch64-windows arm64ec-windows i386-windows nls " ] ||
        bad "dest: $(cd "$d" && ls -A | tr '\n' ' ')"
    [ -e "$d.new" ] && bad "dest.new left behind"
    for f in kernel32.dll extra1.dll extra2.dll ntdll.dll winegstreamer.dll d3d11.dll mydxmt.dll; do
        [ -f "$d/arm64ec-windows/$f" ] || bad "arm64ec-windows has no $f"
    done
    ls "$d/arm64ec-windows" | grep -q '\.exe$' && bad "a program in arm64ec-windows"
    [ "$(ls "$d/arm64ec-windows" | wc -l | tr -d ' ')" = $((ec_listed + 4)) ] || bad "arm64ec-windows: $(ls "$d/arm64ec-windows" | wc -l) files, expected $ec_listed + 2 closure + 2 extras"
    for f in services.exe conhost.exe wow64.dll extra1.dll extra2.dll; do
        [ -f "$d/aarch64-windows/$f" ] || bad "aarch64-windows has no $f"
    done
    [ "$(ls "$d/aarch64-windows" | wc -l | tr -d ' ')" = $((a64_listed + 2)) ] || bad "aarch64-windows: $(ls "$d/aarch64-windows" | wc -l) files, expected $a64_listed + 2 closure"
    for f in kernel32.dll extra1.dll stdole2.tlb services.exe; do
        [ -f "$d/i386-windows/$f" ] || bad "i386-windows has no $f"
    done
    for f in wow64.dll wow64win.dll wow64cpu.dll winex11.drv d3d11.dll hidclass.sys krnl386.exe16 ntoskrnl.exe; do
        [ -e "$d/i386-windows/$f" ] && bad "i386-windows has $f (skipped or not a farm extension)"
    done
    [ -f "$d/nls/c_932.nls" ] && [ -f "$d/nls/l_intl.nls" ] || bad "nls tables missing"
    want "$dir/all.out" "closure adds extra1.dll (imported by kernel32.dll)"
    want "$dir/all.out" "closure adds extra2.dll (imported by extra1.dll)"
    want "$dir/all.out" "optional.dll: kernel32.dll"
    want "$dir/all.out" "arm64ec-windows: 0 error(s)"
    want "$dir/all.out" "aarch64-windows: 0 error(s)"
    want "$dir/all.out" "i386-windows: 0 error(s)"
    want "$dir/all.out" "i386-windows: appwiz.cpl has no download strings (winegecko winemono http)"
    printf 'arm64ec-windows\tnsi.dll\t' > "$dir/nsi.line"
    grep -F "$(cat "$dir/nsi.line")" "$d/MANIFEST.txt" | grep -q "	arm64ec	wine+spec 19$" || bad "MANIFEST: nsi.dll's line"
    grep -q "^arm64ec-windows	d3dcompiler_47.dll	.*	wine+spec 19$" "$d/MANIFEST.txt" || bad "MANIFEST: a PARENTSRC overlay doesn't count"
    grep -q "^arm64ec-windows	kernel32.dll	.*	wine$" "$d/MANIFEST.txt" || bad "MANIFEST: kernel32.dll's line"
    grep -q "^arm64ec-windows	mydxmt.dll	.*	arm64ec	extra:$dir/extra$" "$d/MANIFEST.txt" || bad "MANIFEST: the extra's line"
    grep -q "^i386-windows	kernel32.dll	.*	i386	wine$" "$d/MANIFEST.txt" || bad "MANIFEST: an i386 line"
    grep -q "^aarch64-windows	services.exe	.*	aarch64	wine$" "$d/MANIFEST.txt" || bad "MANIFEST: an aarch64 line"
    grep -q "^nls	c_932.nls	3	[0-9a-f]\{64\}	-	wine$" "$d/MANIFEST.txt" || bad "MANIFEST: an nls line"
    [ "$(wc -l < "$d/MANIFEST.txt" | tr -d ' ')" = "$(find "$d" -mindepth 2 -type f | wc -l | tr -d ' ')" ] ||
        bad "MANIFEST: not one line per file"
    want "$d/BUILDINFO.txt" "wine: Wine version 11.4-fake"
    want "$d/BUILDINFO.txt" "toolchain: --toolchain $tc"
    want "$d/BUILDINFO.txt" "overlay spec19 (spec 19, 19-pe-dlls-build/pe): "
    want "$d/BUILDINFO.txt" "overlay spec15 (spec 15, 15-controllers/pe): missing"
    # B3: the trees and their configure arguments
    want "$dir/work/build-arm64ec/configure.args" "--enable-archs=arm64ec"
    # ARM64EC: the plain clang (MSVC target); the other two: llvm-mingw's GNU triple
    want "$dir/work/build-arm64ec/configure.args" "--with-mingw=$tc/bin/clang --disable-tests --without-x --without-freetype --without-gnutls --without-vulkan --without-wayland --enable-winegstreamer"
    want "$dir/work/build-aarch64/configure.args" "--with-mingw=llvm-mingw --disable-tests"
    want "$dir/work/build-i386/configure.args" "--with-mingw=llvm-mingw --disable-tests"
    grep -q with-wine-tools "$dir/work/build-arm64ec/configure.args" && bad "the arm64ec tree was given other tools"
    want "$dir/work/build-aarch64/configure.args" "--with-wine-tools=$dir/work/build-arm64ec"
    want "$dir/work/build-aarch64/configure.args" "enable_conhost=aarch64 enable_rpcss=aarch64 enable_services=aarch64 enable_wineboot=aarch64"
    want "$dir/work/build-i386/configure.args" "--enable-archs=i386 CROSSCFLAGS=-O2 --with-wine-tools=$dir/work/build-arm64ec"
    grep -q enable_conhost "$dir/work/build-i386/configure.args" && bad "the i386 tree enabled host-only programs"
    # B3: the type library link
    # (widl looks for an ARM64EC tree's imported .tlb under aarch64-windows/)
    [ -L "$dir/work/build-arm64ec/dlls/stdole2.tlb/aarch64-windows/stdole2.tlb" ] || bad "no stdole2.tlb link in the arm64ec tree"
    [ "$(readlink "$dir/work/build-arm64ec/dlls/stdole2.tlb/aarch64-windows/stdole2.tlb")" = ../arm64ec-windows/stdole2.tlb ] ||
        bad "the arm64ec tree's stdole2.tlb link doesn't point at its own file"
    [ -e "$dir/work/build-i386/dlls/stdole2.tlb/aarch64-windows" ] && bad "a type library link in the i386 tree"
    [ -L "$dir/work/build-aarch64/dlls/stdole2.tlb/aarch64-windows/stdole2.tlb" ] && bad "a type library link in the aarch64 tree"

    # ---- spec 00 E1: the relinking kit, and the same farms built again from it ----
    kit="$dir/work/pe-relink-kit.tar.xz"
    if [ -f "$kit" ]; then
        mkdir -p "$dir/kit"
        tar -xJf "$kit" -C "$dir/kit"
        k="$dir/kit/pe-relink-kit"
        for f in README.txt build-pe.sh farms/arm64ec-windows.txt farms/aarch64-windows.txt farms/i386-skip.txt \
                 tools/pecheck.c BUILDINFO.txt MANIFEST.txt src/VERSION src/configure; do
            [ -f "$k/$f" ] || bad "kit: no $f"
        done
        [ -d "$k/out" ] || bad "kit: no out/"
        want "$k/src/dlls/nsi.dll/nsi.c" "/* changed */"
        want "$k/README.txt" "./build-pe.sh --wine src --out-root out --allow-missing-overlays all"
        cmp -s "$k/MANIFEST.txt" "$d/MANIFEST.txt" || bad "kit: MANIFEST.txt isn't the product's"
        if (cd "$k" && bash ./build-pe.sh --wine src --out-root out --toolchain "$tc" --jobs 4 --allow-missing-overlays all) \
            > "$dir/kit.out" 2>&1; then
            for farm in arm64ec-windows aarch64-windows i386-windows; do
                (cd "$d/$farm" && ls) | grep -vx 'd3d11.dll' | grep -vx 'mydxmt.dll' > "$dir/farm-a.lst"
                (cd "$k/build/pe-work/dest/$farm" && ls) > "$dir/farm-b.lst"
                cmp -s "$dir/farm-a.lst" "$dir/farm-b.lst" || bad "kit rebuild: $farm differs from the product (extras aside)"
            done
        else
            bad "building from the kit failed: $(tail -n 20 "$dir/kit.out")"
        fi
    else
        bad "no relinking kit at $kit"
    fi

    # ---- a second run: everything up to date ----
    if run "$dir/again.out" --work "$dir/work" --dest "$dir/dest" --extra "arm64ec-windows=$dir/extra" all; then
        grep -q "copying" "$dir/again.out" && bad "second run: the tree was copied again"
        grep -q "configure" "$dir/again.out" && bad "second run: configured again"
        [ "$(grep -c ': up to date' "$dir/again.out")" -ge 3 ] || bad "second run: the trees weren't up to date"
        grep -q "closure adds" "$dir/again.out" && bad "second run: the closure added something again"
        [ "$(ls "$dir/dest/arm64ec-windows" | wc -l | tr -d ' ')" = $((ec_listed + 4)) ] || bad "second run: arm64ec-windows changed"
    else
        bad "second run failed: $(tail -n 20 "$dir/again.out")"
    fi
else
    bad "build-pe.sh all failed: $(tail -n 30 "$dir/all.out")"
fi

# ---- --extra naming a Wine module of that farm ----
mkdir -p "$dir/extra-bad"
"$mkpe" "$dir/extra-bad/kernel32.dll" --machine 0x8664 --chpe
if run "$dir/extra-bad.out" --work "$dir/work" --dest "$dir/dest2" --extra "arm64ec-windows=$dir/extra-bad" collect; then
    bad "an extra kernel32.dll was accepted"
else
    want "$dir/extra-bad.out" "kernel32.dll is also a module built from Wine"
fi

# ---- App Review 2.5.2: an appwiz.cpl with a download string (UTF-16 "winemono") stops collect ----
if FAKE_TAINT=appwiz.cpl run "$dir/taint.out" --work "$dir/work-taint" --dest "$dir/dest-taint" all; then
    bad "an appwiz.cpl holding \"winemono\" passed collect"
else
    want "$dir/taint.out" "step 'collect' failed: i386-windows: download strings left (App Review 2.5.2): appwiz.cpl:winemono"
    if [ -e "$dir/dest-taint" ]; then bad "a product was written despite the download string"; fi
fi

# ---- a module that doesn't build: retried with -j1, then reported ----
if FAKE_BREAK=nsi.dll run "$dir/break.out" --work "$dir/work-break" arm64ec; then
    bad "a failing module didn't fail the step"
else
    want "$dir/break.out" "arm64ec: retrying nsi.dll with -j1"
    want "$dir/break.out" "nsi.dll failed to build (log: $dir/work-break/logs/arm64ec-nsi.dll.log)"
    want "$dir/break.out" "error: dlls/nsi.dll/arm64ec-windows/nsi.dll: fake failure"
    want "$dir/break.out" "step 'arm64ec' failed: 1 module(s) failed to build: nsi.dll"
fi

# ---- read-only trees: refs/ (here the fixture's $dir/refs, next to --out-root) and --wine ----
# a symlinked --wine: <work>/src must be a folder of its own, and the overlay must not reach the
# linked tree (the bug that once wrote every overlay into refs/wine-11.4)
ln -s "$wine" "$dir/wine-link"
if run "$dir/link.out" --wine "$dir/wine-link" --work "$dir/work-link" tree; then
    [ -d "$dir/work-link/src" ] && [ ! -L "$dir/work-link/src" ] || bad "symlinked --wine: <work>/src is not a folder of its own"
    want "$dir/work-link/src/dlls/nsi.dll/nsi.c" "/* changed */"
    [ -e "$wine/dlls/nsi.dll/nsi.c" ] && bad "symlinked --wine: the overlay was written into the Wine tree"
else
    bad "tree with a symlinked --wine failed: $(tail -n 5 "$dir/link.out")"
fi
# a work or dest folder inside a read-only tree is refused before anything is created
mkdir -p "$dir/refs/wine-11.4"
ln -s "$dir/refs" "$dir/refs-link"
refuse() { # <case> <pattern> <path that must not appear> <build-pe.sh args...>
    rcase=$1 rpattern=$2 rpath=$3
    shift 3
    if bash "$script" --out-root "$out" --toolchain "$tc" --allow-missing-overlays "$@" > "$dir/$rcase.out" 2>&1; then
        bad "$rcase: build-pe.sh ran"
    else
        want "$dir/$rcase.out" "$rpattern"
    fi
    if [ -e "$rpath" ]; then bad "$rcase: $rpath was created"; fi
}
refuse work-in-refs "lies inside $dir/refs, which is read-only" "$dir/refs/w" \
    --wine "$wine" --work "$dir/refs/w" plan
refuse work-in-refs-link "lies inside $dir/refs, which is read-only" "$dir/refs/w2" \
    --wine "$wine" --work "$dir/refs-link/w2" tree
refuse work-in-wine "lies inside $wine, which is read-only" "$wine/w" \
    --wine "$dir/wine-link" --work "$dir/wine-link/w" tree
refuse dest-in-refs "lies inside $dir/refs, which is read-only" "$dir/refs/wine-11.4/dest" \
    --wine "$wine" --work "$dir/work-x" --dest "$dir/refs-link/wine-11.4/dest" collect
mkdir -p "$dir/work-y/src"
refuse wine-in-work "the Wine tree $dir/work-y/src lies inside the work folder" "$dir/work-y/stamps" \
    --wine "$dir/work-y/src" --work "$dir/work-y" tree

# ---- a wrong toolchain hash stops before unpacking (no download: a planted file) ----
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)
        mkdir -p "$dir/work-tc/downloads"
        echo "not the release" > "$dir/work-tc/downloads/llvm-mingw-20260421-ucrt-ubuntu-22.04-x86_64.tar.xz"
        if bash "$script" --wine "$wine" --out-root "$out" --work "$dir/work-tc" toolchain > "$dir/tc.out" 2>&1; then
            bad "a toolchain with the wrong SHA-256 was accepted"
        else
            want "$dir/tc.out" "SHA-256 of llvm-mingw-20260421-ucrt-ubuntu-22.04-x86_64.tar.xz is not f8b8cce779affeab47bcaec6ce6e9e768b166094a366123ef64b2d0b389cc121"
            [ -e "$dir/work-tc/downloads/llvm-mingw-20260421-ucrt-ubuntu-22.04-x86_64.tar.xz" ] && bad "the bad download was kept"
            [ -d "$dir/work-tc/llvm-mingw-20260421-ucrt-ubuntu-22.04-x86_64" ] && bad "the bad download was unpacked"
        fi ;;
esac

[ $failures -eq 0 ] && [ "${KEEP:-0}" != 1 ] && rm -rf "$dir"
if [ $failures -gt 0 ]; then
    echo "test-build-fake.sh: $failures failure(s) (output kept in $dir)" >&2
    exit 1
fi
echo "test-build-fake.sh: all passed"
