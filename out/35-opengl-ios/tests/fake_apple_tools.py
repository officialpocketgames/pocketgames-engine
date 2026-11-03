# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 C1.2/C1.3: stand-ins for the Apple and Chromium tools angle/build-angle.sh runs, so
# tests/test-build-angle-fake.sh can run the script on Linux. Called as
#   python3 -I fake_apple_tools.py <tool> <arguments of the tool>
# for xcrun (libtool, clang, --show-sdk-path), nm, otool, stat, git, gclient, gn, autoninja and
# ninja. Only what build-angle.sh uses is imitated.
#
# A fake object is a text file of lines: "DEF <symbol>" (defined), "UNDEF <symbol>" (needed),
# "DEAD <symbol>" (needed only by code a dead-stripped link drops), "OBJC" (it holds an
# Objective-C class or category). A fake archive is "!fake-archive", then "MEMBER <name>" and the
# member's lines, per member. A fake program lists its imports as "UNDEF <symbol>".
#
# Environment: FAKE_FX (the fixtures: inputs.txt, compdb.json, gn-desc.json), FAKE_ANGLE_COMMIT
# (what `git rev-parse HEAD` says), FAKE_PROBE_FAIL / FAKE_SIZE_FAIL (the probe link, or the size
# link of size_main.c, fails as ld does), FAKE_GN_FAIL (gn desc fails).

import os
import sys

ARCHIVE = "!fake-archive"
LINK_ERROR = """Undefined symbols for architecture arm64:
  "_inflate", referenced from:
      angle::Decompress() in 00003_Context.o
ld: symbol(s) not found for architecture arm64
clang: error: linker command failed with exit code 1 (use -v to see invocation)
"""


def read_object(lines):
    obj = {"def": [], "undef": [], "dead": [], "objc": False}
    for line in lines:
        parts = line.split()
        if parts == ["OBJC"]:
            obj["objc"] = True
        elif len(parts) == 2 and parts[0] in ("DEF", "UNDEF", "DEAD"):
            obj[parts[0].lower()].append(parts[1])
    return obj


def read_file(path):
    """[(member name or None, object)]"""
    with open(path) as f:
        lines = f.read().splitlines()
    if not lines or lines[0] != ARCHIVE:
        return [(None, read_object(lines))]
    members = []
    name, body = None, []
    for line in lines[1:]:
        if line.startswith("MEMBER "):
            if name is not None:
                members.append((name, read_object(body)))
            name, body = line[len("MEMBER "):], []
        else:
            body.append(line)
    if name is not None:
        members.append((name, read_object(body)))
    return members


def libtool(args):
    out = args[args.index("-o") + 1]
    filelist = args[args.index("-filelist") + 1]
    with open(filelist) as f:
        paths = [p.strip() for p in f if p.strip()]
    with open(out, "w") as o:
        o.write(ARCHIVE + "\n")
        for p in paths:
            o.write("MEMBER %s\n" % os.path.basename(p))
            with open(p) as f:
                o.write(f.read().rstrip("\n") + "\n")
    return 0


def clang(args):
    expanded = []
    for a in args:
        if a.startswith("@"):
            with open(a[1:]) as f:
                expanded += f.read().split()
        else:
            expanded.append(a)
    out = expanded[expanded.index("-o") + 1]
    roots = [a[len("-Wl,-u,"):] for a in expanded if a.startswith("-Wl,-u,")]
    mapfile = next((a[len("-Wl,-map,"):] for a in expanded if a.startswith("-Wl,-map,")), None)
    dead_strip = "-Wl,-dead_strip" in expanded
    sources = [a for a in expanded if a.endswith(".c")]
    if any(os.path.basename(s) == "size_main.c" for s in sources):
        roots += ["_eglGetProcAddress", "_eglGetPlatformDisplay", "_glGetString"]
        if os.environ.get("FAKE_SIZE_FAIL"):
            sys.stderr.write(LINK_ERROR)
            return 1
    if mapfile and os.environ.get("FAKE_PROBE_FAIL"):
        sys.stderr.write(LINK_ERROR)
        return 1
    archives = [a for a in expanded if a.endswith(".a") and os.path.exists(a)]
    pool = []  # (archive, member, object)
    for a in archives:
        pool += [(a, m, o) for m, o in read_file(a)]
    loaded = []
    defined = {"_main"} if sources else set()
    need = list(roots)
    if "-ObjC" in expanded:
        for entry in pool:
            if entry[2]["objc"] and entry not in loaded:
                loaded.append(entry)
                defined.update(entry[2]["def"])
                need += entry[2]["undef"] + entry[2]["dead"]
    while need:
        sym = need.pop(0)
        if sym in defined:
            continue
        for entry in pool:
            if sym in entry[2]["def"] and entry not in loaded:
                loaded.append(entry)
                defined.update(entry[2]["def"])
                need += entry[2]["undef"] + entry[2]["dead"]
                break
    imports = set()
    for _, _, o in loaded:
        imports.update(o["undef"])
        if not dead_strip:
            imports.update(o["dead"])
    imports -= defined
    with open(out, "w") as f:
        for s in sorted(imports):
            f.write("UNDEF %s\n" % s)
        f.write("#" * (1000 + 4096 * len(loaded)) + "\n")
    if mapfile:
        with open(mapfile, "w") as f:
            f.write("# Path: %s\n# Arch: arm64\n# Object files:\n[  0] linker synthesized\n" % out)
            f.write("[  1] /tmp/fake/%s.o\n" % os.path.basename(sources[0])[:-2] if sources else "")
            for i, (a, m, _) in enumerate(loaded):
                f.write("[%3d] %s(%s)\n" % (i + 2, a, m))
            f.write("# Sections:\n# Address\tSize\tSegment\tSection\n")
    return 0


def xcrun(args):
    while args and args[0] in ("-sdk", "--sdk"):
        args = args[2:]
    if args == ["--show-sdk-path"]:
        print(os.path.join(os.environ.get("FAKE_FX", "/nonexistent"), "sdk"))
        return 0
    if args and args[0] == "-f":
        print("/usr/bin/" + args[1])
        return 0
    tool, rest = args[0], args[1:]
    if tool == "libtool":
        return libtool(rest)
    if tool in ("clang", "clang++"):
        return clang(rest)
    sys.stderr.write("fake xcrun: no tool %s\n" % tool)
    return 1


def nm(args):
    path = args[-1]
    entries = read_file(path)
    for member, o in entries:
        if member is not None:
            print("\n%s(%s):" % (path, member))
        if "-gU" in args:
            for s in o["def"]:
                print("0000000000000000 T %s" % s)
        elif "-u" in args:
            for s in o["undef"] + o["dead"]:
                print(s)
    return 0


def stat(args):
    if args[:2] == ["-f", "%z"]:
        print(os.path.getsize(args[2]))
        return 0
    sys.stderr.write("fake stat: only -f %z\n")
    return 1


def git(args):
    if "rev-parse" in args:
        print(os.environ.get("FAKE_ANGLE_COMMIT", "0" * 40))
    return 0


def fixture(name):
    with open(os.path.join(os.environ["FAKE_FX"], name)) as f:
        sys.stdout.write(f.read())


def gn(args):
    if args and args[0] == "args":
        print("angle_enable_metal = true")
    elif args and args[0] == "desc":
        if os.environ.get("FAKE_GN_FAIL"):
            sys.stderr.write("ERROR fake gn desc\n")
            return 1
        fixture("gn-desc.json")
    return 0


def ninja(args):
    if "-t" in args:
        what = args[args.index("-t") + 1]
        if what == "inputs":
            fixture("inputs.txt")
        elif what == "compdb":
            fixture("compdb.json")
    return 0


TOOLS = {"xcrun": xcrun, "nm": nm, "otool": lambda a: 0, "stat": stat, "git": git,
         "gclient": lambda a: 0, "gn": gn, "autoninja": lambda a: 0, "ninja": ninja}

if __name__ == "__main__":
    sys.exit(TOOLS[sys.argv[1]](sys.argv[2:]))
