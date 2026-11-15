/*
 * ceuntiletest - does the copy engine read NVDEC's picture layout?
 *
 * Fills a surface in the arrangement NVDEC writes its pictures in
 * (nvdecTileOffset, which the byte-exact H.264 output confirms), has the copy
 * engine copy it out as a block-linear source into a pitch-linear buffer, and
 * compares the result with the picture the pattern describes. Every block
 * height the copy engine knows is tried; the one that matches is what the
 * decoder's surfaces are to the copy engine.
 *
 * The copy channel shares the VA space of a channel on NVDEC, as the add-on's
 * copies will.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <system_error>
#include <vector>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
#include "class/cl2080_notification.h"
#include "class/clc5b5.h"
#include "class/clc6b5.h"
#include "class/clc7b5.h"
#include "class/clc8b5.h"
#include "class/clc9b5.h"
#include "class/clcab5.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
}

#include <ErrorUtils.h>
#include <FileDesc.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include <nvrm-helpers/NvChannel.h>
#include <nvrm-helpers/NvCommandBuffer.h>
#include <nvrm-helpers/NvGpuMemory.h>

#include "nvdec_engine.h"


static const NvU32 kCopySubchannel = 4;


static NvU32 PickCopyClass(NvRmDevice &dev)
{
	static const NvU32 kCandidates[] = {
		BLACKWELL_DMA_COPY_B,
		BLACKWELL_DMA_COPY_A,
		HOPPER_DMA_COPY_A,
		AMPERE_DMA_COPY_B,
		AMPERE_DMA_COPY_A,
		TURING_DMA_COPY_A,
	};

	NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS params {};
	dev.Device().Control(NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &params, sizeof(params));
	for (NvU32 candidate : kCandidates) {
		for (NvU32 i = 0; i < params.numClasses; i++) {
			if (params.classList[i] == candidate) {
				return candidate;
			}
		}
	}
	throw std::system_error(ENOTSUP, std::generic_category(), "no supported DMA copy class");
}


// A value for every position that differs between neighbouring bytes, lines
// and 16-byte columns, so a misplaced line or column shows.
static uint8_t Pattern(int x, int y)
{
	return (uint8_t)(x * 7 + y * 31 + (x >> 4) * 13 + (y >> 3) * 5);
}


struct TestCase {
	const char *name;
	int width;
	int height;
};


int main()
{
	try {
		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice dev(rm, 0);

		NvChannel decodeChannel(dev, NV2080_ENGINE_TYPE_NVDEC0);
		NvChannel copyChannel(dev, NV2080_ENGINE_TYPE_GRAPHICS, decodeChannel.VaSpace());
		NvCommandBuffer cmdBuf(copyChannel);

		const NvU32 copyClass = PickCopyClass(dev);
		NvRmObject copyObject = copyChannel.Channel().Alloc(copyClass, nullptr, 0);
		printf("copy class %#x, decode and copy channels share VA space %#x\n",
			copyClass, decodeChannel.VaSpace());

		NvGpuMemory semaphore;
		semaphore.AllocSystem(dev, copyChannel.VaSpace(), 0x1000, 0x1000, true);
		memset(semaphore.CpuAddr(), 0, semaphore.Size());
		NvU64 sequence = 0;

		static const TestCase kCases[] = {
			{"176x144 luma", 176, 144},
			{"176x72 chroma", 176, 72},
			{"1920x1080 luma", 1920, 1080},
			{"1920x540 chroma", 1920, 540},
			{"3840x2160 luma", 3840, 2160},
		};

		int failures = 0;
		for (const TestCase &test : kCases) {
			// The decoder's surfaces: lines a multiple of 64 bytes wide and
			// whole blocks of sixteen lines tall.
			const int pitch = (test.width + 63) & ~63;
			const int alignedHeight = (test.height + 15) & ~15;
			// Room for the tallest block tried (four GOBs, 32 lines): the
			// copy engine reads whole blocks.
			const size_t tiledSize = (size_t)pitch * ((alignedHeight + 63) & ~63);

			NvGpuMemory tiled;
			NvGpuMemory linear;
			tiled.AllocSystem(dev, copyChannel.VaSpace(), (tiledSize + 0xfff) & ~(size_t)0xfff, 0x10000, true);
			linear.AllocSystem(dev, copyChannel.VaSpace(), (tiledSize + 0xfff) & ~(size_t)0xfff, 0x10000, true);
			memset(tiled.CpuAddr(), 0, tiled.Size());

			auto *tiledBytes = static_cast<uint8_t*>(tiled.CpuAddr());
			for (int y = 0; y < alignedHeight; y++) {
				for (int x = 0; x < pitch; x++) {
					tiledBytes[nvdecTileOffset(x, y, pitch)] = Pattern(x, y);
				}
			}

			int matched = -1;
			for (NvU32 blockHeightLog2 = 0; blockHeightLog2 <= 2; blockHeightLog2++) {
				memset(linear.CpuAddr(), 0xcc, linear.Size());

				WriteSetObject(cmdBuf, kCopySubchannel, copyClass);
				WriteCopyBlockLinearToPitch(
					cmdBuf, kCopySubchannel,
					tiled.GpuAddr(), pitch, alignedHeight, blockHeightLog2,
					linear.GpuAddr(), test.width,
					test.width, test.height
				);
				WriteSemRelease64(cmdBuf, semaphore.GpuAddr(), ++sequence, true);
				cmdBuf.SubmitTo(copyChannel);
				while (*static_cast<volatile NvU64*>(semaphore.CpuAddr()) < sequence) {
					copyChannel.CheckError();
					usleep(100);
				}

				const auto *out = static_cast<const uint8_t*>(linear.CpuAddr());
				int wrong = 0;
				int firstX = -1;
				int firstY = -1;
				for (int y = 0; y < test.height; y++) {
					for (int x = 0; x < test.width; x++) {
						if (out[(size_t)y * test.width + x] != Pattern(x, y)) {
							if (wrong == 0) {
								firstX = x;
								firstY = y;
							}
							wrong++;
						}
					}
				}
				printf("  %-16s block height %d GOBs: ", test.name, 1 << blockHeightLog2);
				if (wrong == 0) {
					printf("identical\n");
					matched = (int)blockHeightLog2;
				} else {
					printf("%d of %d bytes differ, first at (%d, %d)\n",
						wrong, test.width * test.height, firstX, firstY);
				}
			}
			if (matched < 0) {
				failures++;
			}
		}

		printf(failures == 0
			? "the copy engine reads NVDEC's layout\n"
			: "the copy engine does not read NVDEC's layout as a block-linear surface\n");
		return failures == 0 ? 0 : 2;
	} catch (const std::exception &e) {
		fprintf(stderr, "ceuntiletest: %s\n", e.what());
		return 1;
	}
}
