// NvRmUserMemoryDemo
//
// Exercises the kernel's user-page locking hooks (os_lock_user_pages /
// nv_register_user_pages and their teardown) by importing an ordinary
// userland buffer into the GPU as an OS-descriptor memory object and then
// driving the DMA copy engine against it.
//
// A B_FULL_LOCK area is wrapped via NvGpuMemory::WrapUserSystem, which issues
// the VID_HEAP_CONTROL ioctl (NVOS32_FUNCTION_ALLOC_OS_DESCRIPTOR) so the
// kernel clones the area and RM maps its pages into the channel's VA space.
// Two CE copies then run:
//
//   * read  — GPU copies the user buffer into a separate sysmem buffer; the
//             CPU verifies the destination matches the pattern it wrote into
//             the user buffer (proves the GPU read the locked user pages).
//   * write — GPU copies a pattern from that sysmem buffer back into the user
//             buffer; the CPU verifies its own original pointer now sees the
//             new pattern (proves the GPU wrote the same physical pages and
//             that the host pointer aliases them).
//
// Then the area is deleted while still imported and the GPU writes it once
// more, as when a team exits before its file descriptors are closed.
//
// Each copy is bracketed by a WFI semaphore release and a synchronous
// NvChannel::WaitIdle(), so no event/poll plumbing is needed.

#include <fcntl.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <system_error>

#include <OS.h>

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
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvGpuMemory.h"
#include "NvChannel.h"
#include "NvCommandBuffer.h"


// CE methods on subch 4 (matching mesa's SUBC_NV90B5 convention).
static const NvU32 kCeSubch = 4;
// 2 MB: several hundred 4 KB pages, so the non-contiguous page array is
// genuinely exercised rather than a single contiguous run.
static const NvU64 kBufSize = 2ull * 1024 * 1024;


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
	void *userPtr = nullptr;
	area_id userArea = -1;
	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		NvChannel channel(rmDev);
		NvCommandBuffer cmdBuf(channel);

		NvU32 copyClass = PickCopyClass(rmDev);
		NvRmObject ceObj = channel.Channel().Alloc(copyClass, nullptr, 0);

		// The OS-descriptor path takes only memory in a B_FULL_LOCK area,
		// which the kernel clones to keep the pages while the GPU maps them.
		userArea = create_area("user memory demo", &userPtr, B_ANY_ADDRESS, kBufSize,
			B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (userArea < B_OK) {
			throw std::system_error(ENOMEM, std::generic_category(), "create_area");
		}

		// Import the user buffer (GPU-writable) and a separate ordinary sysmem
		// buffer that serves as the copy peer for both directions.
		NvGpuMemory userMem, peer;
		userMem.WrapUserSystem(rmDev, channel.VaSpace(), userPtr, kBufSize, 0x1000, /*writable=*/true);
		peer.AllocSystem(rmDev, channel.VaSpace(), kBufSize, 0x1000, /*mapCpu=*/true);

		auto userBytes = (NvU8*)userPtr;
		auto peerBytes = (NvU8*)peer.CpuAddr();

		printf("channel:          %#" PRIx32 "\n", channel.Handle());
		printf("ce class:         %#" PRIx32 "\n", copyClass);
		printf("user buffer:      %p (%" PRIu64 " bytes)\n", userPtr, (NvU64)kBufSize);
		printf("user gpu addr:    %#" PRIx64 "  (os-descriptor, locked user pages)\n", userMem.GpuAddr());
		printf("peer gpu addr:    %#" PRIx64 "  (sysmem, cpu-mapped)\n", peer.GpuAddr());
		puts("");

		auto runCopy = [&](NvU64 srcGpu, NvU64 dstGpu) {
			WriteSetObject(cmdBuf, kCeSubch, copyClass);
			WriteCopyDma(cmdBuf, kCeSubch, srcGpu, dstGpu, kBufSize);
			cmdBuf.SubmitTo(channel);
			channel.WaitIdle();
		};

		int failures = 0;

		// Read path: CPU fills the user buffer, GPU copies it into the peer,
		// CPU checks the peer matches.
		for (NvU64 i = 0; i < kBufSize; i++) {
			userBytes[i] = (NvU8)(i * 7 + 0x11);
		}
		memset(peerBytes, 0xCC, kBufSize);

		runCopy(userMem.GpuAddr(), peer.GpuAddr());

		if (memcmp(userBytes, peerBytes, kBufSize) == 0) {
			puts("read  (user -> peer): OK");
		} else {
			puts("read  (user -> peer): MISMATCH");
			failures++;
		}

		// Write path: CPU fills the peer, GPU copies it into the user buffer,
		// CPU checks its own pointer now reflects the new contents.
		for (NvU64 i = 0; i < kBufSize; i++) {
			peerBytes[i] = (NvU8)(i * 13 + 0x5A);
		}
		memset(userBytes, 0x00, kBufSize);

		runCopy(peer.GpuAddr(), userMem.GpuAddr());

		if (memcmp(peerBytes, userBytes, kBufSize) == 0) {
			puts("write (peer -> user): OK");
		} else {
			puts("write (peer -> user): MISMATCH");
			failures++;
		}

		// Delete the area while the GPU still maps its pages, as a team does
		// when it exits before its file descriptors are closed: the kernel's
		// clone keeps the pages, so the GPU still writes to memory that is
		// not handed to anyone else, and releasing the import afterwards
		// frees them.
		delete_area(userArea);
		userArea = -1;
		runCopy(peer.GpuAddr(), userMem.GpuAddr());
		channel.CheckError();
		puts("write after the area was deleted: OK");
		userMem = NvGpuMemory();

		if (failures != 0) {
			fprintf(stderr, "[!] %d check(s) failed\n", failures);
			return 2;
		}
		puts("\nall checks passed");
		return 0;
	} catch (const std::system_error &ex) {
		if (userArea >= B_OK) {
			delete_area(userArea);
		}
		fprintf(stderr, "[!] NvRmUserMemoryDemo: %s\n", ex.what());
		return 1;
	}
}
