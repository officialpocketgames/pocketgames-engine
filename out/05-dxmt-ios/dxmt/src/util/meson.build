# Changed for MYIOSDECK (spec 05 E1, spec 06 D). Upstream: DXMT a3a6e8f, MIT.
util_src = files([
  'util_atomic_wait.cpp',
  'util_env.cpp',
  'util_string.cpp',
  # 'util_fps_limiter.cpp',
  # 'util_flush.cpp',
  # 'util_gdi.cpp',
  # 'util_luid.cpp',
  # 'util_matrix.cpp',
  # 'util_shared_res.cpp',
  # 'util_sleep.cpp',
  'util_bloom.cpp',

  'thread.cpp',

  'com/com_guid.cpp',
  'com/com_private_data.cpp',

  'config/config.cpp',

  'log/log.cpp',

  'sha1/sha1.c',
  'sha1/sha1_util.cpp',
])

if dxmt_native
  util_src += files([
    'wsi_monitor_headless.cpp',
    'wsi_window_headless.cpp',
    'wsi_platform_darwin.cpp',
  ])
elif dxmt_ios
  # MYIOSDECK spec 05 E1: no window manager on iOS
  util_src += files([
    'wsi_monitor_ios.cpp',
    'wsi_window_ios.cpp',
    'wsi_platform_win32.cpp',
  ])
else
  util_src += files([
    'wsi_monitor_win32.cpp',
    'wsi_window_win32.cpp',
    'wsi_platform_win32.cpp',
  ])
endif

util_lib = static_library('util', util_src,
  include_directories : [ dxmt_include_path ],
)

# MYIOSDECK spec 06 D: util_atomic_wait.hpp calls ntdll's RtlWaitOnAddress and
# RtlWakeAddress* on a Windows host.
util_ext_deps = []
if host_machine.system() == 'windows' and not dxmt_native
  util_ext_deps += cpp.find_library('ntdll')
endif

util_dep = declare_dependency(
  link_with           : [ util_lib ],
  dependencies        : util_ext_deps,
  include_directories : [ include_directories('.') ],
)
