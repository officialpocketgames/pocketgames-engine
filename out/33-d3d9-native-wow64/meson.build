# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 E and clarification 6: the i386 thunk DLL, d3d9.dll, as a Wine builtin, built the way
# the app's DXMT PE build builds out/05-dxmt-ios/dxmt's i386 DLLs: meson with
# build-win-i686.txt (llvm-mingw, i686-w64-mingw32), -Dwine_builtin_dll=true and
# -Dwine_build_path=<a Wine i386 build>. build-pe.sh drives it. Only the PE thunk is built here;
# the native dispatch is build-ios.sh's.

project('d3d9w-thunk', ['c'],
  version : '1',
  meson_version : '>=1.3.0',
  default_options : ['c_std=gnu11', 'warning_level=2', 'buildtype=release', 'b_ndebug=if-release'])

cc = meson.get_compiler('c')
if host_machine.system() != 'windows' or host_machine.cpu_family() != 'x86'
  error('the thunk is an i386 Windows DLL: configure with a build-win-i686.txt cross file')
endif

wine_build_path = get_option('wine_build_path')
if wine_build_path == ''
  error('-Dwine_build_path=<a Wine i386 build> is required (winecrt0, ntdll and winebuild)')
endif
if not wine_build_path.startswith('/')
  wine_build_path = meson.project_source_root() / wine_build_path
endif

# Wine's unix-call plumbing: winecrt0's __wine_init_unix_call, ntdll's dispatcher.
lib_winecrt0 = cc.find_library('winecrt0', dirs : [
  wine_build_path / 'dlls/winecrt0/i386-windows',
  wine_build_path / 'libs/winecrt0/i386-windows',
])
lib_ntdll = cc.find_library('ntdll', dirs : [wine_build_path / 'dlls/ntdll/i386-windows'], static : true)
winebuild = find_program(wine_build_path / 'tools/winebuild/winebuild')

thunk_src = files(
  'thunk/d3d9_thunk.c',
  'thunk/d3d9_thunk_device.c',
  'thunk/d3d9_thunk_resource.c',
  'thunk/d3d9_thunk_stubs.S',
  'gen/d3d9w_stubs_i386.S',
  'gen/d3d9w_vtables_i386.c',
)

thunk_c_args = cc.get_supported_arguments([
  '-DNOMINMAX',
  '-D_WIN32_WINNT=0xa00',
  # the program calls in (COM methods, the window procedure) with a 4-byte aligned stack
  '-mpreferred-stack-boundary=2',
  '-Wno-unused-parameter',
  '-Wno-cast-function-type',
])

thunk_link_args = cc.get_supported_link_arguments([
  '-static',
  '-Wl,--file-alignment=4096',
  '-Wl,--enable-stdcall-fixup',
  '-Wl,--kill-at',
])

d3d9_dll = shared_library('d3d9', thunk_src,
  name_prefix         : '',
  c_args              : thunk_c_args,
  include_directories : include_directories('thunk', 'gen', 'include'),
  dependencies        : [lib_winecrt0, lib_ntdll, cc.find_library('user32'), cc.find_library('gdi32')],
  vs_module_defs      : 'thunk/d3d9.def',
  link_args           : thunk_link_args,
  install             : true,
  install_dir         : 'i386-windows',
)

if get_option('wine_builtin_dll')
  custom_target('postprocess_d3d9',
    input            : d3d9_dll,
    output           : 'd3d9.dll.postproc',
    command          : [winebuild, '--builtin', '@INPUT@'],
    depends          : d3d9_dll,
    build_by_default : true,
  )
endif
