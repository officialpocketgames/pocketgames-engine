#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 C1.2/C1.3 on the host: angle/build-angle.sh run from start to end on a fake ANGLE build
# with fake Apple and Chromium tools (tests/fake_apple_tools.py: libtool, a linker that loads
# archive members and writes -map, nm, gn gen's project.json; ninja refuses, as depot_tools' does
# once siso has built the out dir). It proves the script's own logic, not ANGLE's build (the
# CI's): the closure from GN, which objects go into libangle.a, LINK.txt, the import check naming
# the source of a hit, and the failure reports.
#   A  the entry points' needs only: a process helper, a Vulkan GPU-info object and the Vulkan
#      loader (volk, which dlopens an absolute path) are left out and not scanned, a build tool is
#      never in the closure; dlopen named with its source; GN's frameworks in LINK.txt; a compiled
#      source with no object noted; passes
#   B  a kept object imports fork: FAIL naming its source; the dead-stripped program doesn't
#   C  the probe link fails: FAIL with the linker's lines, the whole closure kept (and the loader's
#      absolute dlopen then fails)
#   D  the size link fails: FAIL with the linker's lines; a revision with no Perfetto switch keeps
#      Perfetto in the closure (named with its chain) but out of the archive
# All of them: the declared switches for tracing and other back ends turned off (gn gen again),
# Chromium's clang runtime and any source-tree path left out of LINK.txt (the fake clang, like the
# real one, fails on a path that isn't there).
#   E  gn gen wrote no project.json: FAIL with the helper's message
# Needs python3 and bash. Environment: TMPDIR, KEEP (keep the work folder).

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

# GN's project.json, labels with their toolchain as GN writes them: the two libraries, libANGLE
# (GLES1State.cpp has no object), the common code (with a process helper), a Vulkan GPU-info
# source set, zlib, and a code generator run by an action (built twice: never linked)
python3 -I -B - "$T/fx/project.json" << 'EOF'
import json, sys
tc = "//build/toolchain/ios:ios_clang_arm64"
q = lambda l: "%s(%s)" % (l, tc)
targets = {
    q("//:libGLESv2_static"): {"type": "static_library", "sources": ["//src/libGLESv2/entry_points_gles.cpp"],
                               "deps": [q("//src/libANGLE:libANGLE"), q("//:gen_action")]},
    q("//:libEGL_static"): {"type": "static_library", "sources": ["//src/libEGL/entry_points_egl.cpp"],
                            "deps": [q("//:libGLESv2_static")]},
    q("//src/libANGLE:libANGLE"): {"type": "source_set",
                                   "sources": ["//src/libANGLE/Context.cpp", "//src/libANGLE/Stray.cpp",
                                               "//src/libANGLE/GLES1State.cpp", "//src/libANGLE/Context.h",
                                               "//src/libANGLE/renderer/metal/DisplayMtl.mm"],
                                   "deps": [q("//src/common:angle_common"), q("//src/gpu_info_util:angle_gpu_info_util_vulkan"),
                                            q("//third_party/zlib:zlib"), q("//third_party/perfetto/src/base:base")],
                                   "frameworks": ["IOSurface.framework", "IOKit.framework"],
                                   "libs": ["z", "//third_party/llvm-build/Release+Asserts/lib/clang/24/lib/darwin/libclang_rt.ios.a",
                                            "//third_party/prebuilt/libfoo.a"]},
    # tracing: gone from GN's description once angle_use_perfetto = false
    q("//third_party/perfetto/src/base:base"): {"type": "source_set",
                                                "sources": ["//third_party/perfetto/src/base/subprocess_posix.cc"]},
    q("//src/common:angle_common"): {"type": "source_set",
                                     "sources": ["//src/common/system_utils_posix.cpp", "//src/common/process_posix.cpp"]},
    q("//src/gpu_info_util:angle_gpu_info_util_vulkan"): {"type": "source_set",
                                                          "sources": ["//src/gpu_info_util/SystemInfo_vulkan.cpp"],
                                                          "deps": [q("//src/third_party/volk:volk")]},
    # a Vulkan loader that dlopens an absolute path (the iOS job's finding); nothing the entry points need
    q("//src/third_party/volk:volk"): {"type": "source_set", "sources": ["//src/third_party/volk/volk.c"]},
    q("//third_party/zlib:zlib"): {"type": "static_library", "sources": ["//third_party/zlib/inflate.c"]},
    q("//:gen_action"): {"type": "action", "deps": [q("//tools/gen:gen"), "//tools/gen:gen(//build/toolchain/mac:clang_x64)"]},
    q("//tools/gen:gen"): {"type": "executable", "sources": ["//tools/gen/main.cpp"], "frameworks": ["AppKit.framework"]},
    "//tools/gen:gen(//build/toolchain/mac:clang_x64)": {"type": "executable", "sources": ["//tools/gen/main.cpp"]},
}
json.dump({"build_settings": {"build_dir": "//out/ios/", "default_toolchain": tc, "root_path": "/w/angle"},
           "targets": targets}, open(sys.argv[1], "w"), indent=1)
EOF
# the arguments the fake revision declares, with their defaults (args.gn turns Vulkan off; the
# script must turn off the Perfetto and Vulkan system-info switches; tracing is off already)
cat > "$T/fx/declared-args.txt" << 'EOF'
angle_enable_metal = true
angle_enable_vulkan = true
angle_enable_vulkan_system_info = true
angle_enable_trace = false
angle_use_perfetto = true
use_siso = true
EOF

# setup <dir> <variant>: a fake ANGLE checkout with its objects at out/ios, where GN puts them
setup() {
  s="$1/angle"
  b="$s/out/ios"
  mkdir -p "$s/.git" "$s/third_party/zlib" "$s/src/common" "$s/src/gpu_info_util" "$s/src/third_party/volk"
  for h in EGL GLES2 GLES3 KHR; do mkdir -p "$s/include/$h"; echo "/* $h */" > "$s/include/$h/$h.h"; done
  echo "target_os = ['ios']" > "$s/.gclient"
  echo "ANGLE's licence" > "$s/LICENSE"
  printf 'Name: zlib\nLicense: Zlib\nLicense File: LICENSE\n' > "$s/third_party/zlib/README.chromium"
  echo "zlib's licence" > "$s/third_party/zlib/LICENSE"
  echo 'void *h = dlopen(name, RTLD_NOW);' > "$s/src/common/system_utils_posix.cpp"
  echo 'pid_t p = fork();' > "$s/src/common/process_posix.cpp"
  echo 'void *v = dlopen(lib, RTLD_LAZY);' > "$s/src/gpu_info_util/SystemInfo_vulkan.cpp"
  echo '        module = dlopen("/usr/local/lib/libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);' > "$s/src/third_party/volk/volk.c"
  o() { mkdir -p "$b/$(dirname "$1")"; f="$b/$1"; shift; printf '%s\n' "$@" > "$f"; }
  o obj/libGLESv2_static/entry_points_gles.o "DEF _glDrawArrays" "DEF _glGetString" "UNDEF _angle_ContextDraw"
  o obj/libEGL_static/entry_points_egl.o "DEF _eglGetDisplay" "DEF _eglGetProcAddress" \
    "DEF _eglGetPlatformDisplay" "UNDEF _angle_ContextDraw"
  if [ "$2" = B ]; then
    o obj/src/libANGLE/libANGLE/Context.o "DEF _angle_ContextDraw" "UNDEF _angle_GetEnv" "UNDEF _inflate" \
      "UNDEF _memcpy" "UNDEF _angle_Stray" "DEAD _angle_RunApp"
    o obj/src/common/angle_common/process_posix.o "DEF _angle_RunApp" "DEAD _fork" "DEAD _execv"
  else
    o obj/src/libANGLE/libANGLE/Context.o "DEF _angle_ContextDraw" "UNDEF _angle_GetEnv" "UNDEF _inflate" \
      "UNDEF _memcpy" "UNDEF _angle_Stray"
    o obj/src/common/angle_common/process_posix.o "DEF _angle_RunApp" "UNDEF _fork" "UNDEF _execv"
  fi
  o obj/src/libANGLE/libANGLE/Stray.o "DEF _angle_Stray"
  o obj/src/libANGLE/libANGLE/unnamed_gen.o "DEF _angle_Unnamed"
  o obj/src/libANGLE/libANGLE/DisplayMtl.o "OBJC" "DEF _OBJC_CLASS_\$_MtlDisplay" "UNDEF _MTLCreateSystemDefaultDevice"
  o obj/src/common/angle_common/system_utils_posix.o "DEF _angle_GetEnv" "UNDEF _dlopen" "UNDEF _getenv"
  o obj/src/gpu_info_util/angle_gpu_info_util_vulkan/SystemInfo_vulkan.o "DEF _angle_VkInfo" "UNDEF _dlopen" \
    "UNDEF _volkInitialize"
  o obj/src/third_party/volk/volk/volk.o "DEF _volkInitialize" "UNDEF _dlopen"
  o obj/third_party/zlib/zlib/inflate.o "DEF _inflate"
  o obj/third_party/perfetto/src/base/base/subprocess_posix.o "DEF _perfetto_Spawn" "UNDEF _fork" "UNDEF _execve"
  o obj/tools/gen/gen/main.o "DEF _main" "UNDEF _system"
  o clang_x64/obj/tools/gen/gen/main.o "DEF _main" "UNDEF _fork"
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
[ "$m" = "Context.o DisplayMtl.o Stray.o entry_points_egl.o entry_points_gles.o inflate.o system_utils_posix.o " ] && ok \
  || bad "A: libangle.a members: $m"
has "$c" "closure: 7 targets in the link closure, 7 with objects, 11 objects, 1 compiled sources without an object" A
has "$c" "REVIEW: compiled sources of the closure without an object (logs/gn-objects.log): 1" A
has "$out/logs/gn-objects.log" "no object: src/libANGLE/GLES1State.cpp (//src/libANGLE:libANGLE)" A
has "$c" "libangle.a: 7 of the closure's 11 objects" A
has "$out/logs/left-out.txt" "obj/src/common/angle_common/process_posix.o" A
has "$out/logs/left-out.txt" "obj/src/gpu_info_util/angle_gpu_info_util_vulkan/SystemInfo_vulkan.o" A
has "$out/logs/left-out.txt" "obj/src/third_party/volk/volk/volk.o" "A (the Vulkan loader)"
hasnt "$c" "volk.c" "A (a left-out loader's absolute dlopen is not scanned)"
has "$out/logs/left-out.txt" "obj/src/libANGLE/libANGLE/unnamed_gen.o" A
hasnt "$out/logs/left-out.txt" "tools/gen" "A (a build tool is not in the closure)"
has "$c" "REVIEW: _dlopen is imported by: src/common/system_utils_posix.cpp (obj/src/common/angle_common/system_utils_posix.o)" A
has "$c" "REVIEW: src/common/system_utils_posix.cpp calls dlopen" A
hasnt "$c" "SystemInfo_vulkan.cpp" "A (left-out sources are not scanned)"
hasnt "$c" "FAIL" A
has "$c" "the linked program imports nothing ci/forbidden-imports.txt fails" A
has "$out/LINK.txt" "-framework IOKit" "A (GN's frameworks)"
has "$out/LINK.txt" "-lz" "A (GN's libraries)"
has "$out/LINK.txt" "-framework UIKit" "A (the base list)"
hasnt "$out/LINK.txt" "AppKit" "A (a build tool's framework)"
hasnt "$out/LINK.txt" "//" "A (no source-tree path in LINK.txt)"
has "$c" "LINK.txt: dropped: //third_party/llvm-build/Release+Asserts/lib/clang/24/lib/darwin/libclang_rt.ios.a (Chromium's clang runtime; Xcode's clang links its own)" A
has "$c" "REVIEW: LINK.txt: dropped: //third_party/prebuilt/libfoo.a (a path in ANGLE's source tree)" A
has "$c" "arguments turned off (declared by this revision, on by default): angle_use_perfetto angle_enable_vulkan_system_info" A
has "$c" "  declared: angle_use_perfetto = false" A
has "$c" "  declared: angle_enable_vulkan = false" A
has "$out/args.gn" "angle_use_perfetto = false" "A (the final arguments)"
has "$out/args.gn" "angle_enable_vulkan_system_info = false" "A (the final arguments)"
hasnt "$c" "in the closure: //third_party/perfetto" "A (Perfetto gone with its switch)"
has "$c" "in the closure: //src/third_party/volk:volk, through //:libGLESv2_static > //src/libANGLE:libANGLE > //src/gpu_info_util:angle_gpu_info_util_vulkan > //src/third_party/volk:volk" A
has "$out/logs/closure-paths.txt" "//src/common:angle_common	//:libGLESv2_static > //src/libANGLE:libANGLE > //src/common:angle_common" A
[ "$(grep -c 'IOSurface' "$out/LINK.txt")" = 1 ] && ok || bad "A: IOSurface twice in LINK.txt"
has "$out/third-party.txt" "third_party/zlib: Zlib (LICENSE)" A
has "$out/NOTICES.txt" "zlib's licence" A
has "$out/logs/archive-members.tsv" "_DisplayMtl.o	obj/src/libANGLE/libANGLE/DisplayMtl.o	src/libANGLE/renderer/metal/DisplayMtl.mm" \
  "A (an Objective-C++ source)"
has "$c" "sources of the archive's members: 7 (members with no source of that name: 0)" A
[ -f "$out/.stamp" ] && ok || bad "A: no stamp"
run A
has "$T/log-A.txt" "already built" "A again (the stamp)"

run B
[ $rc -ne 0 ] && ok || bad "B: exit 0 with fork imported"
c="$out/checks.txt"
has "$c" "FAIL: libangle.a imports _fork: src/common/process_posix.cpp (obj/src/common/angle_common/process_posix.o)" B
has "$c" "FAIL: libangle.a imports _execv: src/common/process_posix.cpp" B
has "$c" "the linked program imports nothing ci/forbidden-imports.txt fails" "B (dead-stripped)"
[ -f "$out/.stamp" ] && bad "B: stamp written on failure" || ok

run C FAKE_PROBE_FAIL=1
[ $rc -ne 0 ] && ok || bad "C: exit 0 with a failed probe link"
c="$out/checks.txt"
has "$c" "FAIL: the probe link (every entry point of the closure) failed" C
has "$c" "  Undefined symbols for architecture arm64:" C
has "$c" "  clang: error: linker command failed" C
has "$c" "libangle.a: 11 of the closure's 11 objects" "C (the whole closure)"
has "$c" "FAIL: libangle.a imports _fork: src/common/process_posix.cpp" "C (attribution still)"
has "$c" "FAIL: src/third_party/volk/volk.c: dlopen of an absolute path: 1:" "C (a kept loader's absolute dlopen fails)"
has "$out/logs/probe-link.log" "symbol(s) not found" C

run D FAKE_SIZE_FAIL=1 FAKE_NO_PERFETTO_ARG=1
[ $rc -ne 0 ] && ok || bad "D: exit 0 with a failed size link"
c="$out/checks.txt"
has "$c" "FAIL: the size link failed; logs/size-link.log:" D
has "$c" "  ld: symbol(s) not found for architecture arm64" D
# a revision with no Perfetto switch: Perfetto stays in the closure, named with its chain, and
# out of the archive as nothing the entry points need reaches it
has "$c" "arguments turned off (declared by this revision, on by default): angle_enable_vulkan_system_info" D
has "$c" "in the closure: //third_party/perfetto/src/base:base, through //:libGLESv2_static > //src/libANGLE:libANGLE > //third_party/perfetto/src/base:base" D
has "$c" "libangle.a: 7 of the closure's 12 objects" D
has "$out/logs/left-out.txt" "obj/third_party/perfetto/src/base/base/subprocess_posix.o" D
hasnt "$c" "imports _fork" "D (Perfetto left out)"

run E FAKE_GN_FAIL=1
[ $rc -ne 0 ] && ok || bad "E: exit 0 with no project.json"
c="$out/checks.txt"
has "$c" "FAIL: GN's link closure of libEGL_static and libGLESv2_static (logs/gn-objects.log):" E
has "$c" "  angle_closure: " "E (the helper's message)"
has "$T/log-E.txt" "build-angle: checks failed" E

echo "test-build-angle-fake: $checks checks, $failures failures"
[ $failures -eq 0 ]
