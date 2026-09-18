lib_nvidia_gsp = shared_library('nvidia_gsp',
	'IntrSafePool.cpp',
	'Driver.cpp',
	'NvidiaModule.cpp',
	'BaseDevice.cpp',
	'ControlDevice.cpp',
	'Device.cpp',
	'os-haiku.cpp',
	'stubs.c',
	include_directories: [
		'..',

	    '/boot/system/develop/headers/private',
	    '/boot/system/develop/headers/private/shared',
	    '/boot/system/develop/headers/private/kernel',
	    '/boot/system/develop/headers/private/kernel/arch/x86',
	    '/boot/system/develop/headers/private/kernel/arch/x86/64',
	    '/boot/system/develop/headers/private/system',
	    '/boot/system/develop/headers/private/system/arch/x86_64',
	    '/boot/system/develop/headers/private/kernel/boot/platform/efi',
	],
	link_whole: [
		lib_nvhaiku_common,
	],
	dependencies: [
		dep_nvrm,
		dep_nv_kernel,
	],
	gnu_symbol_visibility: 'hidden',
	install: false,
)

# Haiku kernel drivers must be installed under their exact bare name (no
# "lib" prefix, no ".so" suffix) -- name_prefix/name_suffix can no longer
# be set to '' directly to get that (current Meson rejects an empty
# string outright, and an empty array means "use the platform default",
# not "no affix at all", so it isn't a substitute -- verified against the
# actual Meson version this builds with). Building normally and copying
# to the bare name at install time is Meson's own supported pattern for
# forcing an exact output filename.
custom_target('nvidia_gsp_driver',
	input: lib_nvidia_gsp,
	output: 'nvidia_gsp',
	command: ['cp', '@INPUT@', '@OUTPUT@'],
	install: true,
	install_dir: 'add-ons/kernel/drivers/bin',
)

install_symlink('nvidia_gsp', pointing_to: '../../bin/nvidia_gsp', install_dir: 'add-ons/kernel/drivers/dev/graphics')
