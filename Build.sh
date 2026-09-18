#!/bin/bash
set -e

baseDir="$PWD"
buildBaseDir="$PWD/build.$(getarch)"
installDir="$PWD/install.$(getarch)"

# Submodules (open-gpu-kernel-modules, mesa-nvk, mesa-zink, VideoStreamsWsi)
# ship as empty directories until initialized -- do it here so a fresh clone
# doesn't fail deep into the build with confusing "no such file" errors from
# inside one of them instead.
git submodule update --init --recursive

# nvrm_sdk's meson.build (lib_nv_kernel / lib_nv_modeset_kernel) links two
# prebuilt object files straight off disk -- it does not build them itself:
#   open-gpu-kernel-modules/src/nvidia/_out/Haiku_x86_64/nv-kernel.o
#   open-gpu-kernel-modules/src/nvidia-modeset/_out/Haiku_x86_64/nv-modeset-kernel.o
# Those come from NVIDIA's own upstream (plain GNU Make, not meson) build for
# the "OS agnostic" portion of the driver -- see open-gpu-kernel-modules/
# Makefile's own nv_kernel_o/nv_modeset_kernel_o rules, which just run
# `$(MAKE) -C src/nvidia` / `$(MAKE) -C src/nvidia-modeset`. Deliberately NOT
# running the top-level `make modules` target here: that also pulls in
# kernel-open/, which builds the actual Linux kbuild kernel module and has
# no business running on Haiku at all. TARGET_OS defaults to `uname` output
# (utils.mk), which is "Haiku" when this runs natively here, so no override
# is needed for the output path to land where meson expects it.
make -C "$baseDir/open-gpu-kernel-modules/src/nvidia"
make -C "$baseDir/open-gpu-kernel-modules/src/nvidia-modeset"

function buildProject {
	projectName="$1"
	shift
	buildDir="$buildBaseDir/$projectName"

	# Projects live directly at the repo root (e.g. ./accelerant), not under
	# a source/ subdirectory.
	cd "$baseDir/$projectName"

	# nvrm_sdk's meson.build generates two flavors of .pc file: four tied to
	# a library target (Meson installs those alongside the library, i.e.
	# develop/lib/pkgconfig, since the libraries themselves install to
	# develop/lib), but its two standalone pkg.generate(name: 'nvrm', ...)/
	# 'nvkms' calls (no library target) fall back to Meson's plain
	# lib/pkgconfig default instead. Both need to be on the search path, or
	# nvrm_cpp_sdk's dependency('nvrm')/dependency('nvkms') calls fail with
	# "not found" even though nvrm_sdk built and installed successfully.
	meson setup "$buildDir" \
		-Dpkg_config_path="$installDir/develop/lib/pkgconfig:$installDir/lib/pkgconfig" \
		-Dprefix="$installDir" "$@"

	ninja -C "$buildDir"
	ninja -C "$buildDir" install
}

buildProject nvrm_sdk
buildProject nvrm_cpp_sdk
buildProject nvidia_gsp
buildProject accelerant
# NvKmsTest has no corresponding project directory in this repo (or
# upstream X547/nvidia-haiku, as of this fix) -- nothing to build here yet.
buildProject VideoStreamsWsi
buildProject mesa-nvk \
	-Dpkg_config_path="/boot/data/packages/llvm-project/build-libclc.x86_64/install/share/pkgconfig:/boot/data/packages/SPIRV-LLVM-Translator/build.x86_64/install/lib/pkgconfig:/boot/data/packages/Vulkan/SPIRV-Tools/build.x86_64/install/lib/pkgconfig" \
	-Dgallium-drivers= \
	-Dvulkan-drivers=nouveau \
	-Dplatforms=wayland \
	-Dgallium-rusticl=false \
	-Degl=disabled \
	-Dglvnd=disabled \
	-Dglx=disabled \
	-Ddisplay-info=disabled
buildProject mesa-zink \
	-Dpkg_config_path="/boot/data/packages/libglvnd/build.x86_64/install/develop/lib/pkgconfig:/boot/data/packages/Vulkan/SPIRV-Tools/build.x86_64/install/lib/pkgconfig" \
	-Dplatforms=haiku \
	-Dgallium-opencl=disabled \
	-Dgallium-rusticl=false \
	-Dgallium-drivers=zink \
	-Dglx=disabled \
	-Dglvnd=enabled \
	-Degl=enabled \
	-Dvulkan-drivers= \
	-Dllvm=disabled \
	-Dshader-cache=enabled
