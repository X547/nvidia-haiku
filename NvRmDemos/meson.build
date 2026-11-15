project('NvRmDemos', 'cpp', 'c',
	default_options: ['cpp_std=c++20']
)

pkg = import('pkgconfig')

dep_nvrm_common = dependency('nvrm-common')
dep_nvrm_client_cpp = dependency('nvrm-client-cpp')
dep_png = dependency('libpng')

# Shared helpers used by every demo and re-exported as nvrm-helpers-cpp so
# external projects (NvKmsDemos, etc.) can pull in the channel/pushbuffer
# plumbing the same way they consume nvrm-client-cpp.
lib_nvrm_helpers_cpp = static_library('nvrm-helpers-cpp',
	'NvOsEvent.cpp',
	'NvGpuMemory.cpp',
	'NvChannel.cpp',
	'NvCommandBuffer.cpp',
	'NvSemSurf.cpp',
	'NvCanvas2D.cpp',
	'NvCompute.cpp',
	dependencies: [
		dep_nvrm_common,
		dep_nvrm_client_cpp,
	],
	gnu_symbol_visibility: 'hidden',
	install: true,
	install_dir: 'develop/lib',
)

dep_nvrm_helpers_cpp = declare_dependency(
	link_with: lib_nvrm_helpers_cpp,
	dependencies: [
		dep_nvrm_common,
		dep_nvrm_client_cpp,
	],
)

# pkg-config: no subdirs entry, so consumers see the install root on -I and
# must write '#include <nvrm-helpers/NvChannel.h>'. The 'requires' list
# transitively pulls in nvrm-common + nvrm-client-cpp.
pkg.generate(lib_nvrm_helpers_cpp,
	requires: ['nvrm-common', 'nvrm-client-cpp'],
)

install_headers('NvOsEvent.h',       subdir: 'nvrm-helpers')
install_headers('NvGpuMemory.h',     subdir: 'nvrm-helpers')
install_headers('NvChannel.h',       subdir: 'nvrm-helpers')
install_headers('NvCommandBuffer.h', subdir: 'nvrm-helpers')
install_headers('NvSemSurf.h',       subdir: 'nvrm-helpers')
install_headers('NvCanvas2D.h',      subdir: 'nvrm-helpers')
install_headers('NvCompute.h',       subdir: 'nvrm-helpers')
install_headers('shaders/FillShader.h', 'shaders/Nv12ToRgb32Shader.h', subdir: 'nvrm-helpers/shaders')

demos = {
	'NvRmChannelDemo':         ['NvRmChannelDemo.cpp'],
	'NvRmChannelSemSurfDemo':  ['NvRmChannelSemSurfDemo.cpp'],
	'NvRmChannelCopyDemo':     ['NvRmChannelCopyDemo.cpp'],
	'NvRmChannelPipelineDemo': ['NvRmChannelPipelineDemo.cpp'],
	'NvRmChannelChainDemo':    ['NvRmChannelChainDemo.cpp'],
	'NvRmChannelWaitBenchDemo':['NvRmChannelWaitBenchDemo.cpp'],
	'NvRmChannelErrorDemo':    ['NvRmChannelErrorDemo.cpp'],
	'NvRmMemoryMapperDemo':    ['NvRmMemoryMapperDemo.cpp'],
	'NvRmUserMemoryDemo':      ['NvRmUserMemoryDemo.cpp'],
	'NvRmUserMemoryMapDemo':   ['NvRmUserMemoryMapDemo.cpp'],
	'NvRmComputeDemo':         ['NvRmComputeDemo.cpp'],
}

foreach name, sources : demos
	executable(name,
		sources,
		dependencies: [dep_nvrm_helpers_cpp],
		gnu_symbol_visibility: 'hidden',
		install: true,
	)
endforeach

# Image demos additionally link libpng for PngIo (BmpIo has no extra deps).
image_io_sources = ['ImageIo.cpp', 'BmpIo.cpp', 'PngIo.cpp']
image_demos = {
	'NvRm2dBlitDemo':   ['NvRm2dBlitDemo.cpp'] + image_io_sources,
	'NvRmCanvas2DDemo': ['NvRmCanvas2DDemo.cpp'] + image_io_sources,
	'NvRmNv12ToRgbDemo': ['NvRmNv12ToRgbDemo.cpp'] + image_io_sources,
}

foreach name, sources : image_demos
	executable(name,
		sources,
		dependencies: [dep_nvrm_helpers_cpp, dep_png],
		gnu_symbol_visibility: 'hidden',
		install: true,
	)
endforeach

# GUI demo: displays NvCanvas2D output in a Haiku window (zero-copy BBitmap
# over the GART draw buffer), so it additionally links libbe.
cpp = meson.get_compiler('cpp')
dep_be = cpp.find_library('be')

executable('NvRmCanvasGuiDemo',
	'NvRmCanvasGuiDemo.cpp',
	dependencies: [dep_nvrm_helpers_cpp, dep_be],
	gnu_symbol_visibility: 'hidden',
	install: true,
)

dep_vulkan = dependency('vulkan', required: false)
if dep_vulkan.found()
	executable('NvShaderDump',
		'tools/NvShaderDump.cpp',
		dependencies: [dep_vulkan],
		gnu_symbol_visibility: 'hidden',
		install: true,
	)
endif
