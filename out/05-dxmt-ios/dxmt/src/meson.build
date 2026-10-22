subdir('util')
subdir('airconv')

if dxmt_native
subdir('nativemetal')
else
subdir('winemetal')
endif

subdir('dxmt')
subdir('dxgi')
subdir('d3d11')
subdir('d3d10')

# MYIOSDECK spec 07 A4: the Direct3D 9 frontend (LGPL-2.1-or-later, from
# dxmt-d3d9 v0.4-d3d9; see LICENSE-NOTES.md). Windows half only.
if not dxmt_native
subdir('d3d9')
endif

if get_option('enable_nvapi')
subdir('nvapi')
endif

if get_option('enable_nvngx')
subdir('nvngx')
endif