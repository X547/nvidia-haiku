// NvRmChannelPipelineDemo
//
// Exercises pipelined submission: many channel submissions in flight at once
// with NO CPU wait between them. Each iteration copies a distinct source slot
// to a distinct destination slot via the DMA copy engine and submits it
// immediately. Because every iteration writes its own src/dst slot, nothing
// aliases, so the GPU can chew through the backlog at its own pace while the
// CPU keeps queueing.
//
// The point of the test is the *absence* of a per-iteration wait. The channel
// now tracks GPU progress internally (NvChannel's progress tracker), so:
//   * NvChannel::Submit only blocks when the GPFIFO ring is genuinely full,
//     reclaiming slots as the GPU retires earlier submissions;
//   * NvCommandBuffer recycles its chunk ring off the same progress signal.
// With 256 iterations and a 256-entry GPFIFO (two entries per submit) the ring
// wraps several times, so the flow-control + reclamation paths are all hit.
//
// Only ONE completion wait happens, at the very end: each iteration emits a
// WFI semaphore release of its sequence number into a shared slot; since
// releases retire in channel order, waiting for the final value guarantees
// every copy has landed. Then the whole batch is verified at once.

#include <fcntl.h>
#include <unistd.h>

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <system_error>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
#include "class/clc5b5.h"              // TURING_DMA_COPY_A
#include "class/clc6b5.h"              // AMPERE_DMA_COPY_A
#include "class/clc7b5.h"              // AMPERE_DMA_COPY_B
#include "class/clc8b5.h"              // HOPPER_DMA_COPY_A
#include "class/clc9b5.h"              // BLACKWELL_DMA_COPY_A
#include "class/clcab5.h"              // BLACKWELL_DMA_COPY_B
#include "ctrl/ctrl0080/ctrl0080gpu.h" // NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2
}

#include <ErrorUtils.h>
#include <FileDesc.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvGpuMemory.h"
#include "NvChannel.h"
#include "NvCommandBuffer.h"


// CE methods on subch 4 (matching mesa's SUBC_NV90B5 convention).
static const NvU32 kCeSubch = 4;
// 256 submissions over a 256-entry GPFIFO (2 entries each) wraps the ring
// twice, forcing flow-control waits and reclamation rather than letting the
// whole batch sit resident.
static const NvU64 kIterations = 256;
// One page per slot; the copy is small because the test is about submission
// pipelining, not bandwidth.
static const NvU64 kSlotSize = 0x1000;


static NvU32 PickCopyClass(NvRmDevice &rmDev)
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
	rmDev.Device().Control(NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &params, sizeof(params));

	for (NvU32 cand : kCandidates) {
		for (NvU32 i = 0; i < params.numClasses; i++) {
			if (params.classList[i] == cand) {
				return cand;
			}
		}
	}
	throw std::system_error(ENOTSUP, std::generic_category(), "no supported DMA copy class");
}


int main()
{
	try {
		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		NvChannel channel(rmDev);
		NvCommandBuffer cmdBuf(channel);

		NvU32 copyClass = PickCopyClass(rmDev);
		NvRmObject ceObj = channel.Channel().Alloc(copyClass, nullptr, 0);

		// One src and one dst buffer, each partitioned into kIterations slots
		// so every iteration owns disjoint memory and nothing aliases across
		// the in-flight backlog. Both CPU-mapped: CPU fills src, reads dst.
		const NvU64 bufSize = kIterations * kSlotSize;
		NvGpuMemory src, dst;
		src.AllocSystem(rmDev, channel.VaSpace(), bufSize, 0x1000, /*mapCpu=*/true);
		dst.AllocSystem(rmDev, channel.VaSpace(), bufSize, 0x1000, /*mapCpu=*/true);

		// Shared completion slot; each iteration releases its sequence number
		// after the copy retires (WFI). Releases are in channel order, so the
		// final value reaching kIterations proves every copy completed.
		NvGpuMemory done;
		done.AllocSystem(rmDev, channel.VaSpace(), 0x1000, 0x1000, /*mapCpu=*/true);
		auto donePtr = (volatile NvU64*)done.CpuAddr();
		*donePtr = 0;

		auto srcBytes = (NvU8*)src.CpuAddr();
		auto dstBytes = (NvU8*)dst.CpuAddr();
		memset(dstBytes, 0xCC, bufSize);

		printf("channel:     %#" PRIx32 "\n", channel.Handle());
		printf("ce class:    %#" PRIx32 "\n", copyClass);
		printf("iterations:  %" PRIu64 "  (slot=%" PRIu64 " bytes)\n",
			kIterations, kSlotSize);
		puts("");

		// Queue every iteration back-to-back, with no wait in between.
		for (NvU64 i = 0; i < kIterations; i++) {
			NvU8 *s = srcBytes + i * kSlotSize;
			NvU32 salt = (NvU32)i * 0x9E3779B9u + 0xDEADBEEFu;
			for (NvU64 b = 0; b < kSlotSize; b++) {
				s[b] = (NvU8)(b ^ (salt >> ((b & 3) * 8)));
			}

			WriteSetObject(cmdBuf, kCeSubch, copyClass);
			WriteCopyDma(cmdBuf, kCeSubch,
				src.GpuAddr() + i * kSlotSize,
				dst.GpuAddr() + i * kSlotSize,
				kSlotSize);
			// WFI release so the value only lands once this copy has retired.
			WriteSemRelease64(cmdBuf, done.GpuAddr(), i + 1, /*waitForIdle=*/true);

			cmdBuf.SubmitTo(channel);
		}

		printf("queued %" PRIu64 " submissions; submitted GP total = %" PRIu32 "\n",
			kIterations, channel.SubmittedGpTotal());

		// The single completion wait. Spin to the final sequence value; a
		// channel kill turns into a thrown exception via CheckError() rather
		// than a hang.
		while (*donePtr < kIterations) {
			channel.CheckError();
		}

		printf("all retired; completed GP total = %" PRIu32 "\n",
			channel.CompletedGpTotal());

		// Verify every slot in one pass.
		NvU64 mismatches = 0;
		for (NvU64 i = 0; i < kIterations; i++) {
			if (memcmp(srcBytes + i * kSlotSize, dstBytes + i * kSlotSize, kSlotSize) != 0) {
				mismatches++;
			}
		}

		if (mismatches != 0) {
			fprintf(stderr, "[!] %" PRIu64 " of %" PRIu64 " slots mismatched\n",
				mismatches, kIterations);
			return 2;
		}
		printf("OK: %" PRIu64 " slots verified\n", kIterations);
		return 0;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmChannelPipelineDemo: %s\n", ex.what());
		return 1;
	}
}
