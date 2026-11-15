project('nvidia_gsp', 'cpp', 'c',
	default_options: ['cpp_std=c++2a']
)

dep_nv_kernel = dependency('nv-kernel')
dep_nv_modeset_kernel = dependency('nv-modeset-kernel')
dep_nvrm = dependency('nvrm')
dep_nvkms = dependency('nvkms')

add_global_arguments(
	'-Wno-missing-field-initializers',
	language: ['c', 'cpp']
)

add_global_arguments(
	'-D_KERNEL_MODE',
	'-D_DEFAULT_SOURCE',
	'-DNV_PLATFORM_MAX_IOCTL_SIZE=16384',
	'-mno-red-zone',
	'-fno-exceptions',
	language: ['c', 'cpp']
)

cpp = meson.get_compiler('cpp')

crtbegin = run_command(cpp.cmd_array(), '-print-file-name=crtbegin.o', check: true).stdout().strip()
crtend = run_command(cpp.cmd_array(), '-print-file-name=crtend.o', check: true).stdout().strip()

add_global_link_arguments(
	'/boot/system/develop/lib/_KERNEL_',
	crtbegin,
	crtend,
	'/boot/system/develop/lib/haiku_version_glue.o',
	'-nostdlib',
	'-Wl,-z,max-page-size=0x1000',
	language: ['c', 'cpp']
)


lib_nvhaiku_common = static_library('nvhaiku_common',
	'CppUtils.cpp',
	'DeviceNamesArray.cpp',
)


subdir('nvidia')
subdir('nvidia-modeset')
