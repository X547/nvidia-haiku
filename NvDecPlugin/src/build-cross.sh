#!/bin/bash
# Cross-build the NVDEC media add-on for x86_64.
#
# usage: build-cross.sh <haiku build dir> <NVK tree> <output dir>
#        (OGKM_SRC=<open-gpu-kernel-modules checkout>)
#
# The NVK tree (airos-works/mesa-nvk, airos-nvk-r2) provides the resource
# manager API (src/nouveau/vulkan/nvkmd/nvrm) and NVIDIA's class headers; the
# RM headers come from NVIDIA's open-gpu-kernel-modules, which NVK expects in
# nvrm/open-gpu-kernel-modules unless OGKM_SRC names it (the nvidia_rm
# driver's checkout).
# The add-on goes to <output dir>/add-ons/media/plugins/nvdec.
set -euo pipefail

BUILD=$(realpath "$1")
NVK=$(realpath "$2")
OUT=$(realpath -m "$3")
SRC=$(dirname "$(realpath "$0")")
HAIKU=$(realpath "$SRC/../../../../..")
# VP9's headers are read by sunxi_cedar's parser, which this shares.
CEDAR=$(realpath "$SRC/../sunxi_cedar")
NVRM=$NVK/src/nouveau/vulkan/nvkmd/nvrm
[ -f "$NVRM/nvRmApi.c" ] || { echo "no nvRmApi.c in $NVRM" >&2; exit 1; }
O=$(realpath "${OGKM_SRC:-$NVRM/open-gpu-kernel-modules}")
[ -f "$O/src/nvidia/arch/nvalloc/unix/include/nv.h" ] || { echo "no RM headers in $O" >&2; exit 1; }

TOOLS=$BUILD/cross-tools-x86_64/bin/x86_64-unknown-haiku
OBJ=$BUILD/objects/haiku/x86_64/release
GCC_SYSLIBS=$(ls -d "$BUILD"/build_packages/gcc_syslibs_devel-*)
GCC_SYSLIBS_RUNTIME=$(ls -d "$BUILD"/build_packages/gcc_syslibs-*)
GCCLIB=$(dirname "$($TOOLS-gcc -print-libgcc-file-name)")

INCLUDES=(-I"$SRC" -I"$CEDAR" -I"$NVRM" -I"$O/src/common/sdk/nvidia/inc"
	-I"$O/src/nvidia/arch/nvalloc/common/inc" -I"$O/src/nvidia/arch/nvalloc/unix/include"
	-I"$O/src/nvidia/inc/kernel" -I"$NVK/src/nouveau/headers/nvidia"
	-I"$GCC_SYSLIBS/develop/headers/c++"
	-I"$GCC_SYSLIBS/develop/headers/c++/x86_64-unknown-haiku"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include"
	-I"$GCC_SYSLIBS/develop/headers/gcc/include-fixed"
	-I"$HAIKU/headers/private/media" -I"$HAIKU/headers/private/shared"
	-I"$HAIKU/headers/compatibility/bsd" -I"$HAIKU/headers/glibc" -I"$HAIKU/headers/posix"
	-I"$HAIKU/headers" -I"$HAIKU/headers/os" -I"$HAIKU/headers/os/app"
	-I"$HAIKU/headers/os/drivers" -I"$HAIKU/headers/os/interface"
	-I"$HAIKU/headers/os/kernel" -I"$HAIKU/headers/os/media"
	-I"$HAIKU/headers/os/storage" -I"$HAIKU/headers/os/support")
FLAGS=(-O2 -g -fPIC -nostdinc -D_DEFAULT_SOURCE "${INCLUDES[@]}")

OBJDIR=$OUT/.obj-nvdec
mkdir -p "$OBJDIR" "$OUT/add-ons/media/plugins"
pids=()
for f in nvdec_engine nvdec_h264 h264_parse nvdec_hevc hevc_parse nvdec_vp9 \
		nvdec_vp9_tables nvdec_convert; do
	$TOOLS-gcc "${FLAGS[@]}" -c "$SRC/$f.c" -o "$OBJDIR/$f.o" & pids+=($!)
done
for f in vp9_parse vp9_tables bits; do
	$TOOLS-gcc "${FLAGS[@]}" -c "$CEDAR/$f.c" -o "$OBJDIR/cedar_$f.o" & pids+=($!)
done
$TOOLS-gcc "${FLAGS[@]}" -c "$NVRM/nvRmApi.c" -o "$OBJDIR/nvRmApi.o" & pids+=($!)
$TOOLS-g++ "${FLAGS[@]}" -c "$SRC/NVDecPlugin.cpp" -o "$OBJDIR/NVDecPlugin.o" & pids+=($!)
failed=0
for pid in "${pids[@]}"; do wait "$pid" || failed=1; done
[ $failed = 0 ] || { echo "compilation failed" >&2; exit 1; }

$TOOLS-g++ -shared -nostdlib -o "$OUT/add-ons/media/plugins/nvdec" \
	"$OBJ/system/glue/arch/x86_64/crti.o" "$GCCLIB/crtbeginS.o" \
	"$OBJ/system/glue/init_term_dyn.o" "$OBJDIR"/*.o \
	"$OBJ/kits/media/libmedia.so" "$OBJ/kits/libbe.so" "$OBJ/system/libroot/libroot.so" \
	"$GCC_SYSLIBS_RUNTIME/lib/libstdc++.so" "$GCC_SYSLIBS_RUNTIME/lib/libgcc_s.so" \
	"$GCCLIB/libgcc.a" "$GCCLIB/crtendS.o" "$OBJ/system/glue/arch/x86_64/crtn.o" \
	-Wl,--no-undefined -Wl,-rpath-link,"$BUILD"
rm -rf "$OBJDIR"
echo "built nvdec into $OUT/add-ons/media/plugins"
