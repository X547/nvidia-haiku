project('NvKmsDemos', 'cpp', 'c',
	default_options: ['cpp_std=c++20']
)

cpp = meson.get_compiler('cpp')

dep_nvrm_common = dependency('nvrm-common')
dep_nvkms_common = dependency('nvkms-common')
dep_nvrm_client_cpp = dependency('nvrm-client-cpp')
dep_nvkms_client_cpp = dependency('nvkms-client-cpp')
dep_nvrm_helpers_cpp = dependency('nvrm-helpers-cpp')

dep_yamp_cpp = dependency('yaml-cpp')

dep_be = cpp.find_library('be')
dep_media = cpp.find_library('media')

executable('NvKmsTest',
	'NvKmsTest.cpp',
	'NvUtils.cpp',
	'NvKmsBitmap.cpp',
	dependencies: [
		dep_nvrm_common,
		dep_nvkms_common,
		dep_nvrm_client_cpp,
		dep_nvkms_client_cpp,
	],
	gnu_symbol_visibility: 'hidden',
	install: true,
)

executable('NvKmsTest2',
	'NvKmsTest2.cpp',
	'NvUtils.cpp',
	'NvKmsBitmap.cpp',
	dependencies: [
		dep_nvrm_common,
		dep_nvkms_common,
		dep_nvrm_client_cpp,
		dep_nvkms_client_cpp,
	],
	gnu_symbol_visibility: 'hidden',
	install: true,
)

executable('NvKmsInfo',
	'NvKmsInfo.cpp',
	dependencies: [
		dep_nvrm_common,
		dep_nvkms_common,
		dep_nvrm_client_cpp,
		dep_nvkms_client_cpp,
		dep_yamp_cpp,
	],
	gnu_symbol_visibility: 'hidden',
	install: true,
)

executable('NvKmsHotplugTest',
	'NvKmsHotplugTest.cpp',
	dependencies: [
		dep_nvrm_common,
		dep_nvkms_common,
		dep_nvrm_client_cpp,
		dep_nvkms_client_cpp,
	],
	gnu_symbol_visibility: 'hidden',
	install: true,
)

executable('NvKmsHotplugDemo',
	'NvKmsHotplugDemo.cpp',
	'NvUtils.cpp',
	'NvKmsBitmap.cpp',
	dependencies: [
		dep_nvrm_common,
		dep_nvkms_common,
		dep_nvrm_client_cpp,
		dep_nvkms_client_cpp,
	],
	gnu_symbol_visibility: 'hidden',
	install: true,
)

executable('NvKmsAnimDemo',
	'NvKmsAnimDemo.cpp',
	'NvUtils.cpp',
	'NvKmsBitmap.cpp',
	dependencies: [
		dep_nvrm_common,
		dep_nvkms_common,
		dep_nvrm_client_cpp,
		dep_nvkms_client_cpp,
	],
	gnu_symbol_visibility: 'hidden',
	install: true,
)

executable('NvKmsAnimSemSurfDemo',
	'NvKmsAnimSemSurfDemo.cpp',
	'NvUtils.cpp',
	'NvKmsBitmap.cpp',
	dependencies: [
		dep_nvrm_common,
		dep_nvkms_common,
		dep_nvrm_client_cpp,
		dep_nvkms_client_cpp,
		dep_nvrm_helpers_cpp,
	],
	gnu_symbol_visibility: 'hidden',
	install: true,
)

executable('NvKmsLayersDemo',
	'NvKmsLayersDemo.cpp',
	'NvUtils.cpp',
	'NvKmsBitmap.cpp',
	dependencies: [
		dep_nvrm_common,
		dep_nvkms_common,
		dep_nvrm_client_cpp,
		dep_nvkms_client_cpp,
	],
	gnu_symbol_visibility: 'hidden',
	install: true,
)

subdir('NvKmsMediaConsumerDemo')
