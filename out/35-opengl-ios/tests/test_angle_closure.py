# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 Part C1.2 and C1.3: host test of angle/angle_closure.py, on samples written like GN's
# project.json (`gn gen --ide=json`), `gn desc --format=json` and ld's -map output.
#   python3 -I tests/test_angle_closure.py

import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "angle"))
import angle_closure as ac  # noqa: E402

checks = 0
failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print("FAIL: " + what)


TC = "//build/toolchain/mac:clang_arm64"
HOST = "//build/toolchain/mac:clang_x64"


def q(label):
    return "%s(%s)" % (label, TC)


# GN's project.json: labels with their toolchain, as GN writes them
PROJECT = {
    "build_settings": {"build_dir": "//out/ios/", "default_toolchain": TC, "root_path": "/w/angle"},
    "targets": {
        q("//:libGLESv2_static"): {"type": "static_library", "deps": [q("//src/libANGLE:libANGLE"), q("//:translator_gen")],
                                   "frameworks": ["Metal.framework"],
                                   "sources": ["//src/libGLESv2/entry_points_gles_1_0_autogen.cpp",
                                               "//src/libGLESv2/entry_points_gles_1_0_autogen.h"]},
        q("//:libEGL_static"): {"type": "static_library", "deps": [q("//:libGLESv2_static")], "libs": ["c++"],
                                "sources": ["//src/libEGL/egl_stubs.cpp"]},
        q("//src/libANGLE:libANGLE"): {"type": "source_set",
                                       "deps": [q("//src/common:angle_common"), q("//src/gpu_info_util:gpu_info"),
                                                q("//src/compiler:translator")],
                                       "public_deps": [q("//:angle_headers")],
                                       "frameworks": ["IOSurface.framework", "QuartzCore.framework"],
                                       "sources": ["//src/libANGLE/GLES1State.cpp", "//src/libANGLE/Platform.cpp",
                                                   "//src/libANGLE/renderer/metal/DisplayMtl.mm",
                                                   "//out/ios/gen/angle/version_info.cpp"]},
        q("//src/common:angle_common"): {"type": "source_set", "weak_frameworks": ["MetalFX.framework"],
                                         "sources": ["//src/common/system_utils_posix.cpp", "//src/common/debug.h"]},
        q("//:angle_headers"): {"type": "group", "public_deps": [q("//:headers_impl")]},
        q("//:headers_impl"): {"type": "source_set",
                               "libs": ["/sdk/usr/lib/libz.tbd",
                                        "//third_party/llvm-build/Release+Asserts/lib/clang/24/lib/darwin/libclang_rt.osx.a",
                                        "//third_party/prebuilt/libfoo.a"],
                               "frameworks": ["//third_party/bar/Bar.framework"]},
        q("//src/gpu_info_util:gpu_info"): {"type": "static_library",
                                            "frameworks": ["Metal.framework", "CoreGraphics.framework"],
                                            "sources": ["//src/gpu_info_util/SystemInfo.cpp",
                                                        "//src/gpu_info_util/SystemInfo_vulkan.cpp"]},
        # objects named <target>.<name>.o, the other layout
        q("//src/compiler:translator"): {"type": "static_library", "sources": ["//src/compiler/Compiler.cpp"]},
        # a code generator: an action's tool, built for the build machine; never linked
        q("//:translator_gen"): {"type": "action", "deps": ["//tools:gen(%s)" % HOST, q("//tools:gen")]},
        q("//tools:gen"): {"type": "executable", "deps": [q("//tools:tool_lib")], "frameworks": ["AppKit.framework"],
                           "sources": ["//tools/gen/main.cpp"]},
        "//tools:gen(%s)" % HOST: {"type": "executable", "sources": ["//tools/gen/main.cpp"]},
        q("//tools:tool_lib"): {"type": "source_set", "frameworks": ["Cocoa.framework"]},
        # a test library nothing links
        q("//:angle_test_utils"): {"type": "source_set", "frameworks": ["XCTest.framework"]},
        q("//:libGLESv2"): {"type": "shared_library", "deps": [q("//src/libANGLE:libANGLE")]},
    },
}

OBJECTS = [
    "obj/libGLESv2_static/entry_points_gles_1_0_autogen.o",
    "obj/libEGL_static/egl_stubs.o",
    "obj/src/libANGLE/libANGLE/GLES1State.o",
    "obj/src/libANGLE/libANGLE/Platform.o",
    "obj/src/libANGLE/libANGLE/DisplayMtl.o",
    "obj/src/libANGLE/libANGLE/version_info.o",
    "obj/src/common/angle_common/system_utils_posix.o",
    "obj/src/gpu_info_util/gpu_info/SystemInfo.o",
    # SystemInfo_vulkan.cpp has no object (not built)
    "obj/src/compiler/translator.Compiler.o",
    "obj/src/libANGLE/libANGLE/stray.o",          # an object no source of the target names
    "obj/tools/gen/main.o",                       # the tool's, not linked
    "obj/angle_test_utils/test.o",                # nothing links it
    "clang_x64/obj/tools/gen/main.o",             # another toolchain's
]


def test_load():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "project.json")
        with open(p, "w") as f:
            f.write("gn: some line first\n")
            json.dump(PROJECT, f)
        desc = ac.load(p)
        check("//:libEGL_static" in desc and "//tools:gen(%s)" % HOST in desc,
              "project.json: default toolchain dropped from labels, another kept")
        check(desc["//src/libANGLE:libANGLE"]["public_deps"] == ["//:angle_headers"], "deps relabelled")
        p2 = os.path.join(d, "desc.json")
        with open(p2, "w") as f:
            json.dump({"//:a": {"type": "group"}}, f)
        check(ac.load(p2) == {"//:a": {"type": "group", "deps": [], "public_deps": []}}, "gn desc's plain form")
        p3 = os.path.join(d, "bad.json")
        with open(p3, "w") as f:
            f.write("ERROR at //BUILD.gn\n")
        try:
            ac.load(p3)
            check(False, "no JSON refused")
        except ValueError:
            check(True, "")


def desc():
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "project.json")
        with open(p, "w") as f:
            json.dump(PROJECT, f)
        return ac.load(p)


def test_gn_link():
    flags, n, dropped = ac.gn_link(desc(), ["//:libEGL_static", "//:libGLESv2_static"])
    check(flags == ["-framework CoreGraphics", "-framework IOSurface", "-framework Metal",
                    "-framework QuartzCore", "-weak_framework MetalFX", "/sdk/usr/lib/libz.tbd", "-lc++"],
          "gn-link flags: %r" % flags)
    check(dropped == [
        ("//third_party/bar/Bar.framework", "a path in ANGLE's source tree"),
        ("//third_party/llvm-build/Release+Asserts/lib/clang/24/lib/darwin/libclang_rt.osx.a",
         "Chromium's clang runtime; Xcode's clang links its own"),
        ("//third_party/prebuilt/libfoo.a", "a path in ANGLE's source tree"),
    ], "gn-link leaves out the clang runtime and source-tree paths: %r" % dropped)
    check(not any("//" in f for f in flags), "no source-tree path among the flags")
    check(n == 8, "closure size %d (EGL, GLESv2, libANGLE, common, gpu_info, translator, headers group and impl)" % n)
    check("-framework AppKit" not in flags and "-framework Cocoa" not in flags,
          "an action's tool and its libraries are not linked")
    check("-framework XCTest" not in flags, "a target nothing links stays out")
    for bad in ("//:missing", "//:libGLESv2"):
        try:
            ac.gn_link(desc(), [bad])
            check(False, "%s refused" % bad)
        except KeyError:
            check(True, "")


def test_gn_paths():
    p = ac.gn_paths(desc(), ["//:libEGL_static", "//:libGLESv2_static"])
    check(len(p) == 8, "a chain for each of the closure's 8 targets: %d" % len(p))
    check(p["//:libGLESv2_static"] == ["//:libGLESv2_static"], "a start label is its own chain")
    check(p["//:headers_impl"] == ["//:libGLESv2_static", "//src/libANGLE:libANGLE", "//:angle_headers", "//:headers_impl"],
          "through a group's public deps: %r" % p["//:headers_impl"])
    check("//tools:gen" not in p and "//tools:tool_lib" not in p, "an action's tool has no chain")
    p = ac.gn_paths(desc(), ["//:libEGL_static"])
    check(p["//src/common:angle_common"] == ["//:libEGL_static", "//:libGLESv2_static", "//src/libANGLE:libANGLE",
                                             "//src/common:angle_common"], "the shortest chain from one start")


def test_gn_objects():
    with tempfile.TemporaryDirectory() as b:
        for o in OBJECTS:
            os.makedirs(os.path.join(b, os.path.dirname(o)), exist_ok=True)
            open(os.path.join(b, o), "w").close()
        rows, missing, n, with_objects = ac.gn_objects(desc(), ["//:libEGL_static", "//:libGLESv2_static"], b)
        got = dict(rows)
        check(got == {
            "obj/libGLESv2_static/entry_points_gles_1_0_autogen.o": "src/libGLESv2/entry_points_gles_1_0_autogen.cpp",
            "obj/libEGL_static/egl_stubs.o": "src/libEGL/egl_stubs.cpp",
            "obj/src/libANGLE/libANGLE/GLES1State.o": "src/libANGLE/GLES1State.cpp",
            "obj/src/libANGLE/libANGLE/Platform.o": "src/libANGLE/Platform.cpp",
            "obj/src/libANGLE/libANGLE/DisplayMtl.o": "src/libANGLE/renderer/metal/DisplayMtl.mm",
            "obj/src/libANGLE/libANGLE/version_info.o": "out/ios/gen/angle/version_info.cpp",
            "obj/src/libANGLE/libANGLE/stray.o": "?",
            "obj/src/common/angle_common/system_utils_posix.o": "src/common/system_utils_posix.cpp",
            "obj/src/gpu_info_util/gpu_info/SystemInfo.o": "src/gpu_info_util/SystemInfo.cpp",
            "obj/src/compiler/translator.Compiler.o": "src/compiler/Compiler.cpp",
        }, "gn-objects rows: %r" % rows)
        check(len(rows) == len(got), "each object once")
        check(missing == ["src/gpu_info_util/SystemInfo_vulkan.cpp (//src/gpu_info_util:gpu_info)"],
              "a compiled source with no object reported, a header not: %r" % missing)
        check(n == 8 and with_objects == 6, "counts: %d targets, %d with objects" % (n, with_objects))


MAP_LD64 = """# Path: /w/probe-ios
# Arch: arm64
# Object files:
[  0] linker synthesized
[  1] /w/size_empty-1a2b.o
[  2] /w/libangle-closure-ios.a(00012_system_utils.o)
[  3] /w/libangle-closure-ios.a(00007_Context.o)
[  4] /w/libangle-closure-ios.a(00012_system_utils.o)
[  5] /Applications/Xcode.app/.../MacOSX.sdk/System/Library/Frameworks/Metal.framework/Metal.tbd
[  6] /w/other.a(foo.o)
# Sections:
# Address	Size    	Segment	Section
0x100003F70	0x00000008	__TEXT	__text
# Symbols:
[  2] /w/libangle-closure-ios.a(99999_not_an_object.o)
"""

MAP_PRIME = """# Path: probe
# Arch: arm64
# Object files:
[  0] linker synthesized
[  1] /tmp/size_empty.o
[  2] /w/libangle-closure-macos.a[3](00003_egl.o)
[  3] /w/libangle-closure-macos.a[1](00001_a(b).o)
[  4] /w/libangle-closure-macos.a[9](00009_entry_points_gles.o)
# Sections:
"""


def test_map_members():
    m = ac.map_members(MAP_LD64, "libangle-closure-ios.a")
    check(m == ["00012_system_utils.o", "00007_Context.o"], "ld64 map members: %r" % m)
    m = ac.map_members(MAP_LD64)
    check(m == ["00012_system_utils.o", "00007_Context.o", "foo.o"], "ld64 map, any archive: %r" % m)
    m = ac.map_members(MAP_PRIME, "libangle-closure-macos.a")
    check(m == ["00003_egl.o", "00009_entry_points_gles.o"],
          "newer map layout with member indices: %r (a name with parentheses is not one of ours)" % m)
    check(ac.map_members("no map here\n") == [], "no object section, no members")


def test_command_line():
    with tempfile.TemporaryDirectory() as d:
        pj = os.path.join(d, "project.json")
        with open(pj, "w") as f:
            json.dump(PROJECT, f)
        mp = os.path.join(d, "probe.map")
        with open(mp, "w") as f:
            f.write(MAP_LD64)
        b = os.path.join(d, "out")
        for o in OBJECTS:
            os.makedirs(os.path.join(b, os.path.dirname(o)), exist_ok=True)
            open(os.path.join(b, o), "w").close()
        empty = os.path.join(d, "empty")
        os.makedirs(empty)
        script = os.path.join(HERE, "..", "angle", "angle_closure.py")

        def run(*args):
            p = subprocess.run([sys.executable, "-I", "-B", script] + list(args),
                               capture_output=True, text=True)
            return p.returncode, p.stdout, p.stderr

        rc, out, err = run("gn-link", pj, "//:libEGL_static")
        check(rc == 0 and out.splitlines()[0] == "-framework CoreGraphics" and "8 targets" in err,
              "gn-link command: %d %r %r" % (rc, out, err))
        check("dropped: //third_party/llvm-build/Release+Asserts/lib/clang/24/lib/darwin/libclang_rt.osx.a "
              "(Chromium's clang runtime; Xcode's clang links its own)" in err and "//" not in out,
              "gn-link names what it left out: %r" % err)
        rc, out, _ = run("gn-paths", pj, "//:libEGL_static", "//:libGLESv2_static")
        check(rc == 0 and "//src/common:angle_common\t//:libGLESv2_static > //src/libANGLE:libANGLE > "
              "//src/common:angle_common" in out.splitlines() and len(out.splitlines()) == 8,
              "gn-paths command: %r" % out)
        rc, out, err = run("gn-link", pj, "//:nope")
        check(rc == 2 and "//:nope" in err, "gn-link refuses an unknown label: %d %r" % (rc, err))
        rc, out, err = run("gn-objects", pj, b, "//:libEGL_static")
        lines = out.splitlines()
        check(rc == 0 and len(lines) == 10 and "obj/src/libANGLE/libANGLE/GLES1State.o\tsrc/libANGLE/GLES1State.cpp" in lines,
              "gn-objects command: %d %r" % (rc, lines))
        last = err.splitlines()[-1]
        check(last == "angle_closure: 8 targets in the link closure, 6 with objects, 10 objects, "
                      "1 compiled sources without an object" and "no object: src/gpu_info_util/SystemInfo_vulkan.cpp" in err,
              "gn-objects summary: %r" % err)
        rc, out, err = run("gn-objects", pj, empty, "//:libEGL_static")
        check(rc == 3 and out == "", "gn-objects with no object: %d" % rc)
        bad = os.path.join(d, "bad.json")
        with open(bad, "w") as f:
            f.write("not json\n")
        rc, _, err = run("gn-objects", bad, b, "//:libEGL_static")
        check(rc == 2 and "no JSON object" in err, "gn-objects on a file with no JSON: %d %r" % (rc, err))
        rc, out, _ = run("map-members", mp, "libangle-closure-ios.a")
        check(rc == 0 and out == "00012_system_utils.o\n00007_Context.o\n", "map-members command: %r" % out)
        rc, _, err = run("nothing")
        check(rc == 2 and "usage" in err, "usage on a wrong call")


test_load()
test_gn_link()
test_gn_paths()
test_gn_objects()
test_map_members()
test_command_line()
print("test_angle_closure: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
