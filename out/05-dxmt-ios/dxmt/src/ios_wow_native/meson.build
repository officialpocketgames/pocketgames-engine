# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 Part B: the ios_wow_native build. The Direct3D 9 frontend (src/d3d9) with the dxmt core
# (src/dxmt) and util (src/util) as one static library of native ARM64 code for 32-bit programs,
# for aarch64-apple-ios (cross file build-ios-arm64.txt). It also holds Part C's guest-window
# allocator (out/33-d3d9-native-wow64/alloc) and the B4 queries over Wine's win32u
# (util/util_ios_wow_native_user.c, against Wine 11.4's include/).
#
# The Windows types come from DXMT's own include/native headers, as the dxmt_native build uses
# them, with util/util_ios_wow_native.h first in every C++ file. winemetal's thunks call the
# unix side's 64-bit table (dxmt_winemetal_unix_call_funcs, spec 05 A) directly; the DXSO
# translator is called directly too (airconv, in the unix half's libdxmt_unix.a). The top-level
# meson.build hands over to this file and stops, so nothing of the PE builds runs here.
#
#   meson setup --cross-file build-ios-arm64.txt -Dios_wow_native=true -Dbuildtype=release <dir>
#   ninja -C <dir> src/ios_wow_native/libd3d9_ios_wow_native.a
#
# The sources are listed in sources.txt, which tests/ios-wow-native/compile-native-linux.sh
# compiles on Linux too.

ios_root = meson.project_source_root()
ios_repo = fs.parent(fs.parent(fs.parent(ios_root)))
ios_interfaces = ios_repo / 'interfaces'
ios_alloc = ios_repo / 'out' / '33-d3d9-native-wow64' / 'alloc'
ios_wine_include = ios_repo / 'refs' / 'wine-11.4' / 'include'
foreach dir : [ios_interfaces, ios_alloc, ios_wine_include]
  if not fs.is_dir(dir)
    error('ios_wow_native: ' + dir + ' is missing (the repository around this DXMT tree, with refs/wine-11.4)')
  endif
endforeach

ios_cc = meson.get_compiler('c')
ios_cpp = meson.get_compiler('cpp')

ios_defines = [
  '-DDXMT_NATIVE=1',
  '-DDXMT_IOS=1',
  '-DDXMT_IOS_WOW_NATIVE=1',
  '-DDXMT_PAGE_SIZE=4096',
  '-DNOMINMAX',
  '-D_WIN32_WINNT=0xa00',
]
ios_warnings = [
  '-fblocks',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  '-Wno-unused-private-field',
  '-Wno-unused-const-variable',
  '-Wno-missing-braces',
  '-Wno-extern-c-compat',
  '-Wno-microsoft-exception-spec',
  '-Wno-cast-function-type',
]
ios_c_args = ios_defines + ios_cc.get_supported_arguments(ios_warnings)
ios_cpp_args = ios_defines + ios_cpp.get_supported_arguments(ios_warnings) + [
  '-include', ios_root / 'src' / 'util' / 'util_ios_wow_native.h',
]

# The command shaders dxmt_command.cpp embeds, compiled as the PE builds compile them.
ios_xcrun = find_program('xcrun')
ios_xxd = find_program('xxd')
ios_metalir = generator(ios_xcrun,
  output    : [ '@BASENAME@.air' ],
  arguments : [ '-sdk', 'macosx', 'metal', '-o', '@OUTPUT@', '-c', '@INPUT@', '@EXTRA_ARGS@' ],
)
ios_metallib = generator(ios_xcrun,
  output    : [ '@BASENAME@.metallib' ],
  arguments : [ '-sdk', 'macosx', 'metallib', '-o', '@OUTPUT@', '@INPUT@' ],
)
ios_hexdump = generator(ios_xxd,
  output    : [ '@BASENAME@.h' ],
  arguments : [ '-n', '@BASENAME@', '-i', '@INPUT@', '@OUTPUT@' ],
)
ios_shader_header = ios_hexdump.process(
  ios_metallib.process(ios_metalir.process('../dxmt/dxmt_command.metal', extra_args : metal_shader_args))
)

ios_version = vcs_tag(
  command : [ 'git', 'describe', '--always' ],
  input   : '../../version.h.in',
  output  : 'version.h',
)

ios_src = []
foreach line : fs.read('sources.txt').split('\n')
  line = line.strip()
  if line != '' and not line.startswith('#')
    ios_src += files('..' / line)
  endif
endforeach

# B4's queries, against Wine's own headers (as Wine's unix libraries are compiled).
ios_user_lib = static_library('dxmt_native_user', '../util/util_ios_wow_native_user.c',
  c_args              : [ '-D__WINESRC__', '-DWINE_UNIX_LIB' ],
  include_directories : include_directories(ios_wine_include, is_system : true),
)

# Part C's allocator.
ios_alloc_lib = static_library('ios_wow_window_alloc', ios_alloc / 'ios_wow_window_alloc.c')

d3d9_ios_wow_native_lib = static_library('d3d9_ios_wow_native', ios_src, ios_shader_header, ios_version,
  c_args              : ios_c_args,
  cpp_args            : ios_cpp_args,
  include_directories : [
    include_directories('.', '../../include', '../../include/native/windows', '../../include/native/directx',
                        '../../libs', '../util', '../dxmt', '../d3d9', '../airconv', '../winemetal'),
    include_directories(ios_interfaces),
  ],
  link_whole          : [ ios_user_lib, ios_alloc_lib ],
)
