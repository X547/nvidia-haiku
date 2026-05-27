#!/bin/bash
set -e

rm -rf "build.$(getarch)"
meson setup "build.$(getarch)" \
	-Dpkg_config_path="$PWD/../nvrm_sdk/build.$(getarch)/install/develop/lib/pkgconfig:$PWD/../nvrm_cpp_sdk/build.$(getarch)/install/develop/lib/pkgconfig" \
	-Dprefix="$PWD/build.$(getarch)/install"
