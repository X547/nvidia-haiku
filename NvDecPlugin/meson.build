project('NvDecPlugin', 'cpp', 'c',
	default_options: ['cpp_std=c++20', 'c_std=gnu11']
)

cpp = meson.get_compiler('cpp')

dep_nvrm_helpers_cpp = dependency('nvrm-helpers-cpp')
dep_be = cpp.find_library('be')
dep_media = cpp.find_library('media')

inc_nvdec = include_directories('src', 'src/cedar', 'src/nvidia')
inc_private_media = include_directories('/boot/system/develop/headers/private/media')

lib_nvdec = static_library('nvdec',
	'src/NvdecEngine.cpp',
	'src/h264_parse.c',
	'src/nvdec_h264.c',
	'src/hevc_parse.c',
	'src/nvdec_hevc.c',
	'src/nvdec_vp9.c',
	'src/nvdec_vp9_tables.c',
	'src/nvdec_convert.c',
	'src/cedar/bits.c',
	'src/cedar/vp9_parse.c',
	'src/cedar/vp9_tables.c',
	include_directories: inc_nvdec,
	dependencies: [dep_nvrm_helpers_cpp],
	gnu_symbol_visibility: 'hidden',
	pic: true,
)

dep_nvdec = declare_dependency(
	link_with: lib_nvdec,
	include_directories: inc_nvdec,
	dependencies: [dep_nvrm_helpers_cpp],
)

shared_module('nvdec',
	'src/NVDecPlugin.cpp',
	include_directories: inc_private_media,
	dependencies: [dep_nvdec, dep_be, dep_media],
	link_args: ['-Wl,--exclude-libs,ALL'],
	name_prefix: '',
	gnu_symbol_visibility: 'hidden',
	install: true,
	install_dir: 'add-ons/media/plugins',
)

executable('mediadecode',
	'tools/mediadecode.cpp',
	dependencies: [dep_be, dep_media],
	gnu_symbol_visibility: 'hidden',
	install: false,
)

executable('nvdecstream',
	'tools/nvdecstream.c',
	dependencies: [dep_nvdec],
	gnu_symbol_visibility: 'hidden',
	install: false,
)

executable('ceuntiletest',
	'tools/ceuntiletest.cpp',
	dependencies: [dep_nvdec],
	gnu_symbol_visibility: 'hidden',
	install: false,
)
