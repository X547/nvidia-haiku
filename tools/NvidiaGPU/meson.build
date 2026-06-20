project('NvidiaGPU', 'cpp',
	default_options: ['cpp_std=c++20']
)

cpp = meson.get_compiler('cpp')

# RUSD lives behind the plain C RM headers (nvrm-common) and is driven through
# the RAII C++ wrappers (nvrm-client-cpp), exactly like the NvRmDemos.
dep_nvrm_common = dependency('nvrm-common')
dep_nvrm_client_cpp = dependency('nvrm-client-cpp')
dep_be = cpp.find_library('be')
dep_localestub = cpp.find_library('localestub', required: false)

deps = [dep_nvrm_common, dep_nvrm_client_cpp, dep_be]
if dep_localestub.found()
	deps += dep_localestub
endif

executable('NvidiaGPU',
	'NvidiaGPUApp.cpp',
	'NvidiaGPUWindow.cpp',
	'NvidiaGPUView.cpp',
	'GPUProgressBar.cpp',
	'GpuStats.cpp',
	dependencies: deps,
	gnu_symbol_visibility: 'hidden',
	install: true,
	install_dir: 'apps/Nebula',
)

install_symlink('NvidiaGPU',
	pointing_to: '../../../../../apps/Nebula/NvidiaGPU',
	install_dir: 'data/deskbar/menu/Applications/Nebula',
)

# Compile the app resources (icon, signature, version) and attach them to the
# installed binary when Haiku's resource tools are present. This is best-effort
# so the project still builds in environments without `rc`.
rc = find_program('rc', required: false)
if rc.found()
	nvidia_gpu_rsrc = custom_target('NvidiaGPU.rsrc',
		input: 'NvidiaGPU.rdef',
		output: 'NvidiaGPU.rsrc',
		command: [rc, '-o', '@OUTPUT@', '@INPUT@'],
		build_by_default: true,
	)
	meson.add_install_script('attach_resources.sh', nvidia_gpu_rsrc.full_path())
endif
