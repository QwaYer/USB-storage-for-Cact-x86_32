# ==============================================================================
# usb_storage module — Meson build system
# ==============================================================================
#
#   meson setup build-meson --cross-file cross/i686-cact-clang.ini \
#       -Dkern_root=../CactKernel-x86_32 -Dlocal_repo=../LocalRepoCactOS-x86_32
#   ninja -C build-meson          # build-meson/usb_storage.cctk
#   ninja -C build-meson stage    # copy into <local_repo>/lib
#
# The .cctk is a single relocatable object (ET_REL): every source file is
# compiled to an i386 object and the objects are combined with a relocatable
# link (`clang -r`), because the kernel's USB module loader maps one ET_REL.
# ==============================================================================

project('usb-storage-mod', 'c',
  meson_version: '>=1.0.0',
  default_options: [
    'c_std=gnu17',
    'warning_level=1',
    'buildtype=plain',
  ])

bash = find_program('bash')
cc = meson.get_compiler('c')

src_root = meson.project_source_root()

# Path options are relative to this repo's root unless absolute.
_kern = get_option('kern_root')
kern_root = _kern.startswith('/') ? _kern : src_root / _kern
_local = get_option('local_repo')
local_repo = _local.startswith('/') ? _local : src_root / _local
install_dir = local_repo / 'lib'

# Kernel headers this module compiles against.
mod_includes = [
  kern_root / 'Cact/drivers/usb',
  kern_root / 'Cact/kernel/sync',
]

mod_c_args = [
  '-ffreestanding',
  '-fno-pie',
  '-fno-stack-protector',
  '-nostdlib',
  # Ring 0: the kernel has not enabled SSE for itself, so clang must not lower
  # struct copies to SSE2 (the same trap the kernel build hits).
  '-mno-mmx',
  '-mno-sse',
  '-mno-sse2',
  '-O2',
]
foreach d : mod_includes
  mod_c_args += '-I' + d
endforeach
mod_c_args += '-I' + src_root

_cc_cmd  = ' '.join(cc.cmd_array())
_args    = ' '.join(mod_c_args)

_sources = ['usb_storage.c']

usb_storage_cctk = custom_target('usb_storage.cctk',
  input: _sources,
  output: 'usb_storage.cctk',
  depend_files: [],
  command: [bash, '-c', (
    'set -e; ' +
    'out="$1"; shift; ' +
    'd=$(mktemp -d); ' +
    'for f in "$@"; do ' +
      _cc_cmd + ' -m32 ' + _args + ' -c "$f" -o "$d/$(basename "$f" .c).o"; ' +
    'done; ' +
    _cc_cmd + ' -m32 -nostdlib -r "$d"/*.o -o "$out"; ' +
    'rm -rf "$d"'),
    'bash', '@OUTPUT@', '@INPUT@'],
  build_by_default: true)

# Meson reserves the name "install" for itself, hence "stage".
run_target('stage',
  command: [bash, '-c', 'set -e; mkdir -p "$1"; cp -f "$2" "$1/usb_storage.cctk"',
            'bash', install_dir, usb_storage_cctk])
