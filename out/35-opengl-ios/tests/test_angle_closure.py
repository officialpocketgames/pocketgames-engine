# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 Part C1.2 and C1.3: host test of angle/angle_closure.py, on samples written like
# `gn desc --format=json`, ld's -map output and `ninja -t compdb`.
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


DESC = {
    "//:libGLESv2_static": {"type": "static_library", "deps": ["//:libANGLE", "//:translator_gen"],
                            "frameworks": ["Metal.framework"]},
    "//:libEGL_static": {"type": "static_library", "deps": ["//:libGLESv2_static"], "libs": ["c++"]},
    "//:libANGLE": {"type": "source_set", "deps": ["//src/common:angle_common", "//:gpu_info"],
                    "public_deps": ["//:angle_headers"], "frameworks": ["IOSurface.framework", "QuartzCore.framework"]},
    "//src/common:angle_common": {"type": "source_set", "deps": [], "weak_frameworks": ["MetalFX.framework"]},
    "//:angle_headers": {"type": "group", "public_deps": ["//:headers_impl"]},
    "//:headers_impl": {"type": "source_set", "libs": ["/sdk/usr/lib/libz.tbd"]},
    "//:gpu_info": {"type": "static_library", "frameworks": ["Metal.framework", "CoreGraphics.framework"]},
    # a code generator: an action's tool, built for the build machine; never linked
    "//:translator_gen": {"type": "action", "deps": ["//:gen_tool"]},
    "//:gen_tool": {"type": "executable", "deps": ["//:tool_lib"], "frameworks": ["AppKit.framework"]},
    "//:tool_lib": {"type": "source_set", "frameworks": ["Cocoa.framework"]},
    # a test library nothing links
    "//:angle_test_utils": {"type": "source_set", "frameworks": ["XCTest.framework"]},
    "//:libGLESv2": {"type": "shared_library", "deps": ["//:libANGLE"]},
}


def test_gn_link():
    flags, n = ac.gn_link(DESC, ["//:libEGL_static", "//:libGLESv2_static"])
    check(flags == ["-framework CoreGraphics", "-framework IOSurface", "-framework Metal",
                    "-framework QuartzCore", "-weak_framework MetalFX", "/sdk/usr/lib/libz.tbd", "-lc++"],
          "gn-link flags: %r" % flags)
    check(n == 7, "gn-link closure size %d (EGL, GLESv2, libANGLE, common, headers group and impl, gpu_info)" % n)
    check("-framework AppKit" not in flags and "-framework Cocoa" not in flags,
          "an action's tool and its libraries are not linked")
    check("-framework XCTest" not in flags, "a target nothing links stays out")
    try:
        ac.gn_link(DESC, ["//:missing"])
        check(False, "a missing label raises")
    except KeyError:
        check(True, "")
    try:
        ac.gn_link(DESC, ["//:libGLESv2"])
        check(False, "a shared library is not a starting point")
    except KeyError:
        check(True, "")
    # a dependency in another toolchain (absent from the default toolchain's description) is skipped
    desc = dict(DESC)
    desc["//:libEGL_static"] = {"type": "static_library",
                                "deps": ["//:libGLESv2_static", "//:x(//build/toolchain/mac:clang_x64)"]}
    flags2, n2 = ac.gn_link(desc, ["//:libEGL_static"])
    check(n2 == 7 and "-lc++" not in flags2, "other toolchains skipped; the replaced target's libs gone")


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


COMPDB = [
    {"directory": "/w/angle/out/ios", "file": "../../src/common/system_utils_posix.cpp",
     "output": "obj/src/common/angle_common/system_utils_posix.o", "command": "clang++ -c ..."},
    {"directory": "/w/angle/out/ios", "file": "/w/angle/src/libANGLE/Context.cpp",
     "command": "clang++ -c /w/angle/src/libANGLE/Context.cpp -o obj/libANGLE/Context.o"},
    {"directory": "/w/angle/out/ios", "file": "../../tools/gen.cc",
     "output": "clang_x64/obj/tools/gen/gen.o", "command": "clang++ -c ..."},
    {"directory": "/w/angle/out/ios", "file": "../../src/x.c",
     "arguments": ["clang", "-c", "../../src/x.c", "-o", "obj/src/x/x.o"]},
]


def test_sources():
    t = ac.compdb_outputs(COMPDB)
    check(t.get("obj/src/common/angle_common/system_utils_posix.o") == "../../src/common/system_utils_posix.cpp",
          "output field: %r" % t)
    check(t.get("obj/libANGLE/Context.o") == "../../src/libANGLE/Context.cpp",
          "-o from the command, an absolute source made relative: %r" % t.get("obj/libANGLE/Context.o"))
    check(t.get("obj/src/x/x.o") == "../../src/x.c", "-o from arguments")
    check(t.get("clang_x64/obj/tools/gen/gen.o") == "../../tools/gen.cc", "another toolchain's object keyed by its path")


def test_command_line():
    with tempfile.TemporaryDirectory() as d:
        dj = os.path.join(d, "desc.json")
        with open(dj, "w") as f:
            json.dump(DESC, f)
        mp = os.path.join(d, "probe.map")
        with open(mp, "w") as f:
            f.write(MAP_LD64)
        cj = os.path.join(d, "compdb.json")
        with open(cj, "w") as f:
            json.dump(COMPDB, f)
        ol = os.path.join(d, "objects.txt")
        with open(ol, "w") as f:
            f.write("obj/libANGLE/Context.o\nobj/unknown/u.o\n\n")
        script = os.path.join(HERE, "..", "angle", "angle_closure.py")

        def run(*args):
            p = subprocess.run([sys.executable, "-I", "-B", script] + list(args),
                               capture_output=True, text=True)
            return p.returncode, p.stdout, p.stderr

        rc, out, err = run("gn-link", dj, "//:libEGL_static")
        check(rc == 0 and out.splitlines()[0] == "-framework CoreGraphics" and "7 targets" in err,
              "gn-link command: %d %r %r" % (rc, out, err))
        rc, out, err = run("gn-link", dj, "//:nope")
        check(rc == 2 and "//:nope" in err, "gn-link refuses an unknown label: %d %r" % (rc, err))
        rc, out, _ = run("map-members", mp, "libangle-closure-ios.a")
        check(rc == 0 and out == "00012_system_utils.o\n00007_Context.o\n", "map-members command: %r" % out)
        rc, out, _ = run("sources", cj, ol)
        check(rc == 0 and out == "obj/libANGLE/Context.o\t../../src/libANGLE/Context.cpp\nobj/unknown/u.o\t?\n",
              "sources command: %r" % out)
        rc, _, err = run("nothing")
        check(rc == 2 and "usage" in err, "usage on a wrong call")


test_gn_link()
test_map_members()
test_sources()
test_command_line()
print("test_angle_closure: %d checks, %d failures" % (checks, failures))
sys.exit(1 if failures else 0)
