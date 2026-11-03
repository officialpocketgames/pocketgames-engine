#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 C1.2/C1.3 on the host: angle/build-angle.sh run from start to end on a fake ANGLE build
# with fake Apple and Chromium tools (tests/fake_apple_tools.py: libtool, a linker that loads
# archive members and writes -map, nm, gn desc, ninja's inputs and compile commands). It proves the
# script's own logic, not ANGLE's build (the CI's): which objects go into libangle.a, LINK.txt,
# the import check naming the source of a hit, and the failure reports of the two links.
#   A  the entry points' needs only: a process helper, a Vulkan GPU-info object and a build tool
#      are left out; dlopen named with its source; GN's frameworks in LINK.txt; passes
#   B  a kept object imports fork: FAIL naming its source; the dead-stripped program doesn't
#   C  the probe link fails: FAIL with the linker's lines, the whole closure kept
#   D  the size link fails: FAIL with the linker's lines; gn desc failing leaves the base LINK.txt
# Needs python3 and bash. Environment: TMPDIR.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PART=$(cd "$HERE/.." && pwd)
command -v python3 > /dev/null 2>&1 || { echo "SKIPPED test-build-angle-fake (no python3)"; exit 0; }
command -v bash > /dev/null 2>&1 || { echo "SKIPPED test-build-angle-fake (no bash)"; exit 0; }
T=${TMPDIR:-/tmp}/build-angle-fake.$$
[ -n "${KEEP:-}" ] && echo "test-build-angle-fake: keeping $T" || trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/fx/sdk" "$T/depot"
checks=0
failures=0
ok() { checks=$((checks + 1)); }
bad() { checks=$((checks + 1)); failures=$((failures + 1)); echo "FAIL: $*"; }

for t in xcrun nm otool stat git gclient gn autoninja ninja; do
  printf '#!/bin/sh\nexec python3 -I -B "%s" %s "$@"\n' "$HERE/fake_apple_tools.py" "$t" > "$T/bin/$t"
  chmod +x "$T/bin/$t"
done
if ! command -v shasum > /dev/null 2>&1; then
  printf '#!/bin/sh\n[ "${1:-}" = -a ] && shift 2\nexec sha256sum "$@"\n' > "$T/bin/shasum"
  chmod +x "$T/bin/shasum"
fi
# depot_tools: gclient, and a bootstrap that writes what build-angle.sh checks for
printf '#!/bin/sh\nexit 0\n' > "$T/depot/gclient"
printf '#!/bin/sh\necho bootstrap-3.11_bin/bin > "$(dirname "$0")/python3_bin_reldir.txt"\n' > "$T/depot/ensure_bootstrap"
printf 'open(__file__.replace("update_depot_tools_toggle.py", ".disable_auto_update"), "w").close()\n' \
  > "$T/depot/update_depot_tools_toggle.py"
chmod +x "$T/depot/gclient" "$T/depot/ensure_bootstrap"
commit=$(sed -n 's/^ANGLE_COMMIT=\([0-9a-f]*\).*/\1/p' "$PART/angle/build-angle.sh")

# the fixtures: ninja's inputs (default toolchain, another toolchain, a source, a header), GN's
# description, ninja's compile commands (DisplayMtl has none)
cat > "$T/fx/inputs.txt" << 'EOF'
obj/src/libGLESv2/libGLESv2_static/entry_points_gles.o
obj/src/libGLESv2/libEGL_static/entry_points_egl.o
obj/src/libANGLE/libANGLE/Context.o
obj/src/libANGLE/renderer/metal/angle_metal_backend/DisplayMtl.o
obj/src/common/angle_common/system_utils_posix.o
obj/src/common/angle_common/process_posix.o
obj/src/gpu_info_util/angle_gpu_info_util_vulkan/SystemInfo_vulkan.o
obj/third_party/zlib/zlib/inflate.o
obj/tools/gen/gen_main.o
clang_x64/obj/tools/gen/gen_host.o
../../src/libANGLE/Context.cpp
gen/angle/version.h
EOF
cat > "$T/fx/gn-desc.json" << 'EOF'
{"//:libEGL_static": {"type": "static_library", "deps": ["//:libGLESv2_static"]},
 "//:libGLESv2_static": {"type": "static_library", "deps": ["//src/libANGLE:libANGLE", "//tools:gen_action"],
                         "frameworks": ["IOSurface.framework", "IOKit.framework"]},
 "//src/libANGLE:libANGLE": {"type": "source_set", "libs": ["z"]},
 "//tools:gen_action": {"type": "action", "deps": ["//tools:gen"]},
 "//tools:gen": {"type": "executable", "frameworks": ["AppKit.framework"]}}
EOF
python3 -I -B - "$T/fx/inputs.txt" "$T/fx/compdb.json" << 'EOF'
import json, sys
objs = [l.strip() for l in open(sys.argv[1]) if l.strip().endswith(".o")]
src = {"entry_points_gles": "src/libGLESv2/entry_points_gles_autogen.cpp",
       "entry_points_egl": "src/libGLESv2/entry_points_egl_autogen.cpp",
       "Context": "src/libANGLE/Context.cpp", "system_utils_posix": "src/common/system_utils_posix.cpp",
       "process_posix": "src/common/process_posix.cpp",
       "SystemInfo_vulkan": "src/gpu_info_util/SystemInfo_vulkan.cpp",
       "inflate": "third_party/zlib/inflate.c", "gen_main": "tools/gen/main.cpp", "gen_host": "tools/gen/main.cpp"}
db = []
for o in objs:
    name = o.rsplit("/", 1)[1][:-2]
    if name in src:
        db.append({"directory": "/w/angle/out/ios", "file": "../../" + src[name], "output": o, "command": "clang++ -c"})
json.dump(db, open(sys.argv[2], "w"), indent=1)
EOF

# setup <dir> <variant>: a fake ANGLE checkout with its objects at out/ios
setup() {
  s="$1/angle"
  b="$s/out/ios"
  mkdir -p "$s/.git" "$s/third_party/zlib" "$s/src/common" "$s/src/gpu_info_util" "$s/tools/gen"
  for h in EGL GLES2 GLES3 KHR; do mkdir -p "$s/include/$h"; echo "/* $h */" > "$s/include/$h/$h.h"; done
  echo "target_os = ['ios']" > "$s/.gclient"
  echo "ANGLE's licence" > "$s/LICENSE"
  printf 'Name: zlib\nLicense: Zlib\nLicense File: LICENSE\n' > "$s/third_party/zlib/README.chromium"
  echo "zlib's licence" > "$s/third_party/zlib/LICENSE"
  echo 'void *h = dlopen(name, RTLD_NOW);' > "$s/src/common/system_utils_posix.cpp"
  echo 'pid_t p = fork();' > "$s/src/common/process_posix.cpp"
  echo 'void *v = dlopen(lib, RTLD_LAZY);' > "$s/src/gpu_info_util/SystemInfo_vulkan.cpp"
  o() { mkdir -p "$b/$(dirname "$1")"; shift_file="$b/$1"; shift; printf '%s\n' "$@" > "$shift_file"; }
  o obj/src/libGLESv2/libGLESv2_static/entry_points_gles.o "DEF _glDrawArrays" "DEF _glGetString" "UNDEF _angle_ContextDraw"
  o obj/src/libGLESv2/libEGL_static/entry_points_egl.o "DEF _eglGetDisplay" "DEF _eglGetProcAddress" \
    "DEF _eglGetPlatformDisplay" "UNDEF _angle_ContextDraw"
  if [ "$2" = B ]; then
    o obj/src/libANGLE/libANGLE/Context.o "DEF _angle_ContextDraw" "UNDEF _angle_GetEnv" "UNDEF _inflate" \
      "UNDEF _memcpy" "DEAD _angle_RunApp"
    o obj/src/common/angle_common/process_posix.o "DEF _angle_RunApp" "DEAD _fork" "DEAD _execv"
  else
    o obj/src/libANGLE/libANGLE/Context.o "DEF _angle_ContextDraw" "UNDEF _angle_GetEnv" "UNDEF _inflate" "UNDEF _memcpy"
    o obj/src/common/angle_common/process_posix.o "DEF _angle_RunApp" "UNDEF _fork" "UNDEF _execv"
  fi
  o obj/src/libANGLE/renderer/metal/angle_metal_backend/DisplayMtl.o "OBJC" "DEF _OBJC_CLASS_\$_MtlDisplay" \
    "UNDEF _MTLCreateSystemDefaultDevice"
  o obj/src/common/angle_common/system_utils_posix.o "DEF _angle_GetEnv" "UNDEF _dlopen" "UNDEF _getenv"
  o obj/src/gpu_info_util/angle_gpu_info_util_vulkan/SystemInfo_vulkan.o "DEF _angle_VkInfo" "UNDEF _dlopen"
  o obj/third_party/zlib/zlib/inflate.o "DEF _inflate"
  o obj/tools/gen/gen_main.o "DEF _main" "UNDEF _system"
  o clang_x64/obj/tools/gen/gen_host.o "DEF _main" "UNDEF _fork"
}

# run <variant> [env assignments]: build-angle.sh for ios; sets $out and $rc
run() {
  v=$1
  shift
  w="$T/work-$v"
  out="$T/out-$v"
  setup "$w" "$v"
  env PATH="$T/bin:$PATH" DEPOT_TOOLS="$T/depot" FAKE_FX="$T/fx" FAKE_ANGLE_COMMIT="$commit" "$@" \
    bash "$PART/angle/build-angle.sh" "$w" "$out" ios > "$T/log-$v.txt" 2>&1
  rc=$?
}
has() { if grep -qF -- "$2" "$1"; then ok; else bad "$3: no \"$2\" in $(basename "$1")"; fi; }
hasnt() { if grep -qF -- "$2" "$1"; then bad "$3: \"$2\" in $(basename "$1")"; else ok; fi; }
members() { sed -n 's/^MEMBER //p' "$1/libangle.a" | sed 's/^[0-9]*_//' | LC_ALL=C sort | tr '\n' ' '; }

run A
if [ $rc -eq 0 ]; then ok; else bad "A: exit $rc"; sed -n '1,60p' "$T/log-A.txt"; fi
c="$out/checks.txt"
[ -f "$out/logs/archive-members.tsv" ] && ok || bad "A: no logs/archive-members.tsv"
m=$(members "$out")
[ "$m" = "Context.o DisplayMtl.o entry_points_egl.o entry_points_gles.o inflate.o system_utils_posix.o " ] && ok \
  || bad "A: libangle.a members: $m"
has "$c" "libangle.a: 6 of the closure's 9 objects" A
has "$out/logs/left-out.txt" "obj/src/common/angle_common/process_posix.o" A
has "$out/logs/left-out.txt" "obj/src/gpu_info_util/angle_gpu_info_util_vulkan/SystemInfo_vulkan.o" A
has "$out/logs/left-out.txt" "obj/tools/gen/gen_main.o" A
hasnt "$out/logs/left-out.txt" "clang_x64" "A (another toolchain's object is not in the closure)"
has "$c" "REVIEW: _dlopen is imported by: ../../src/common/system_utils_posix.cpp (obj/src/common/angle_common/system_utils_posix.o)" A
has "$c" "REVIEW: ../../src/common/system_utils_posix.cpp calls dlopen" A
hasnt "$c" "SystemInfo_vulkan.cpp" "A (left-out sources are not scanned)"
hasnt "$c" "FAIL" A
has "$c" "the linked program imports nothing ci/forbidden-imports.txt fails" A
has "$out/LINK.txt" "-framework IOKit" "A (GN's frameworks)"
has "$out/LINK.txt" "-lz" "A (GN's libraries)"
has "$out/LINK.txt" "-framework UIKit" "A (the base list)"
hasnt "$out/LINK.txt" "AppKit" "A (a build tool's framework)"
[ "$(grep -c 'IOSurface' "$out/LINK.txt")" = 1 ] && ok || bad "A: IOSurface twice in LINK.txt"
has "$out/third-party.txt" "third_party/zlib: Zlib (LICENSE)" A
has "$out/NOTICES.txt" "zlib's licence" A
has "$out/logs/archive-members.tsv" "DisplayMtl.o	obj/src/libANGLE/renderer/metal/angle_metal_backend/DisplayMtl.o	?" \
  "A (no compile command: ?)"
[ -f "$out/.stamp" ] && ok || bad "A: no stamp"
run A
has "$T/log-A.txt" "already built" "A again (the stamp)"

run B
[ $rc -ne 0 ] && ok || bad "B: exit 0 with fork imported"
c="$out/checks.txt"
has "$c" "FAIL: libangle.a imports _fork: ../../src/common/process_posix.cpp (obj/src/common/angle_common/process_posix.o)" B
has "$c" "FAIL: libangle.a imports _execv: ../../src/common/process_posix.cpp" B
has "$c" "the linked program imports nothing ci/forbidden-imports.txt fails" "B (dead-stripped)"
[ -f "$out/.stamp" ] && bad "B: stamp written on failure" || ok

run C FAKE_PROBE_FAIL=1
[ $rc -ne 0 ] && ok || bad "C: exit 0 with a failed probe link"
c="$out/checks.txt"
has "$c" "FAIL: the probe link (every entry point of the closure) failed" C
has "$c" "  Undefined symbols for architecture arm64:" C
has "$c" "  clang: error: linker command failed" C
has "$c" "libangle.a: 9 of the closure's 9 objects" "C (the whole closure)"
has "$c" "FAIL: libangle.a imports _fork: ../../src/common/process_posix.cpp" "C (attribution still)"
has "$out/logs/probe-link.log" "symbol(s) not found" C

run D FAKE_SIZE_FAIL=1 FAKE_GN_FAIL=1
[ $rc -ne 0 ] && ok || bad "D: exit 0 with a failed size link"
c="$out/checks.txt"
has "$c" "FAIL: the size link failed; logs/size-link.log:" D
has "$c" "  ld: symbol(s) not found for architecture arm64" D
has "$c" "REVIEW: gn desc or angle_closure.py gn-link failed" D
hasnt "$out/LINK.txt" "IOKit" "D (base list only)"
has "$c" "libangle.a: 6 of the closure's 9 objects" D

echo "test-build-angle-fake: $checks checks, $failures failures"
[ $failures -eq 0 ]
