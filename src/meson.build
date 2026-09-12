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

# The Direct3D 9 frontend is a Windows module only: it is built against wine's
# import libraries and reaches Metal through winemetal's unix calls, so there is
# no native (macOS) variant of it. Which architectures actually get built, and
# which of the built DLLs are installed into the app bundle, is decided by
# build/dxmt-ios/build-pe.sh (--targets / --install), not here.
#
# MADEIRA-TEMP (WOW64_DESIGN.md section 7, stage 4): the sources are imported
# and only 5 of the 21 translation units still fail, all on src/dxmt APIs that
# postdate this fork (see the status log). Until those are closed the module is
# opt-in (-Denable_d3d9=true) so that the rest of the tree still builds.
if not dxmt_native and get_option('enable_d3d9')
subdir('d3d9')
endif

if get_option('enable_nvapi')
subdir('nvapi')
endif

if get_option('enable_nvngx')
subdir('nvngx')
endif