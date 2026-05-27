// NvRmChannelCopyDemo
//
// Drives a CPU -> GPU -> GPU -> CPU copy pipeline through the DMA copy
// engine with four backing buffers:
//   * srcCpu — system memory, CPU-mapped (the CPU writes the payload)
//   * srcGpu — vidmem, no CPU mapping       (GPU-only staging)
//   * dstGpu — vidmem, no CPU mapping       (GPU-only staging)
//   * dstCpu — system memory, CPU-mapped (the CPU reads back for verify)
//
// Each iteration submits three CE copies (srcCpu→srcGpu, srcGpu→dstGpu,
// dstGpu→dstCpu), bracketing each by a timestamped, WFI semsurf release
// so we get GPU-time deltas per stage. The three stages exercise three
// distinct memory paths: PCIe upload, GPU-internal vidmem-to-vidmem,
// PCIe download. The semsurf carries four slots: one "t0" before any
// copy plus three end-of-stage timestamps. Only the last slot is polled;
// the others are read directly after the CPU wakes. The destination
// memcmp catches any corruption end-to-end.
//
// GPU timestamps are PTIMER ticks, which is 1 ns/tick on Turing+ GPUs.

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <system_error>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
#include "class/cl2080_notification.h" // NV2080_NOTIFIERS_FIFO_EVENT_MTHD, NV2080_NOTIFIERS_RC_ERROR
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

#include "NvOsEvent.h"
#include "NvGpuMemory.h"
#include "NvChannel.h"
#include "NvCommandBuffer.h"
#include "NvSemSurf.h"


// CE methods on subch 4 (matching mesa's SUBC_NV90B5 convention).
static const NvU32 kCeSubch = 4;
// 4 MB per buffer: large enough to swamp per-LAUNCH overhead and give
// meaningful bandwidth numbers but small enough to stay well inside any
// per-channel allocation budget. WriteCopyDma chunks at 128 KB internally.
static const NvU64 kBufSize = 4ull * 1024 * 1024;

// Four timestamp slots: one start, three end-of-stage.
enum {
	kSlotT0     = 0, // baseline: start of pipeline (post-WFI of empty queue)
	kSlotUpload = 1, // after srcCpu -> srcGpu
	kSlotGpuGpu = 2, // after srcGpu -> dstGpu
	kSlotFinal  = 3, // after dstGpu -> dstCpu  (this is the one we poll)
	kSlotCount  = 4,
};


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


// Format bandwidth: bytes copied / duration (in PTIMER ticks, == ns).
// Returns the result in MB/s where MB = 1<<20.
static double BandwidthMBps(NvU64 bytes, NvU64 ticks)
{
	if (ticks == 0) {
		return 0.0;
	}
	double seconds = (double)ticks * 1e-9;
	return (double)bytes / seconds / (1024.0 * 1024.0);
}


int main()
{
	try {
		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		// Graphics-engine channel — drives CE work via SET_OBJECT routing
		// (see comment in NvRmChannelCopyDemo history). The channel
		// owning the CE class object is enough; the engineType binding
		// just decides which runlist this channel sits on.
		NvChannel channel(rmDev);
		NvCommandBuffer cmdBuf(channel);

		NvU32 copyClass = PickCopyClass(rmDev);
		NvRmObject ceObj = channel.Channel().Alloc(copyClass, nullptr, 0);

		// Four buffers. The outer pair lives in CPU-mapped system memory;
		// the inner pair lives in vidmem with no CPU mapping at all, so
		// the GPU-to-GPU stage actually exercises the on-card path.
		NvGpuMemory srcCpu, srcGpu, dstGpu, dstCpu;
		srcCpu.AllocSystem(rmDev, channel.VaSpace(), kBufSize, 0x1000, /*mapCpu=*/true);
		srcGpu.AllocLocal (rmDev, channel.VaSpace(), kBufSize, 0x1000);
		dstGpu.AllocLocal (rmDev, channel.VaSpace(), kBufSize, 0x1000);
		dstCpu.AllocSystem(rmDev, channel.VaSpace(), kBufSize, 0x1000, /*mapCpu=*/true);

		NvSemSurf semSurf(rmDev, channel.VaSpace(), kSlotCount);
		semSurf.BindChannel(channel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});

		NvOsEvent osEvent(rm);
		// The semsurf path uses RegisterWaiter to wake the fd on the
		// completion side; we add an RC_ERROR binding so a channel kill
		// also wakes poll() instead of waiting on the 5s timeout.
		NvRmObject hErrorEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_RC_ERROR, /*nonStallIntr=*/false);

		printf("channel:           %#" PRIx32 "\n", channel.Handle());
		printf("ce class:          %#" PRIx32 "\n", copyClass);
		printf("ce object:         %#" PRIx32 "\n", ceObj.Get());
		printf("buffer size:       %" PRIu64 " bytes (%.2f MB)\n",
			(NvU64)kBufSize, (double)kBufSize / (1024.0 * 1024.0));
		printf("semsurf:           %#" PRIx32 "  slots=%d  layout.size=%" PRIu64
			"  64bit=%d\n",
			semSurf.Handle(), kSlotCount, (NvU64)semSurf.Layout().size,
			(int)semSurf.Is64Bit());
		printf("srcCpu gpu addr:   %#" PRIx64 "  (sysmem, cpu-mapped)\n", srcCpu.GpuAddr());
		printf("srcGpu gpu addr:   %#" PRIx64 "  (vidmem)\n", srcGpu.GpuAddr());
		printf("dstGpu gpu addr:   %#" PRIx64 "  (vidmem)\n", dstGpu.GpuAddr());
		printf("dstCpu gpu addr:   %#" PRIx64 "  (sysmem, cpu-mapped)\n", dstCpu.GpuAddr());
		puts("");

		auto srcBytes = (NvU8*)srcCpu.CpuAddr();
		auto dstBytes = (NvU8*)dstCpu.CpuAddr();

		const NvU64 iterations = 64;
		NvU64 mismatches = 0;
		// Running totals for the average row at the bottom.
		NvU64 sumUpload = 0, sumGpuGpu = 0, sumDownload = 0, sumTotal = 0;

		for (NvU64 wSeq = 1; wSeq <= iterations; wSeq++) {
			NvU32 salt = (NvU32)wSeq * 0x9E3779B9u + 0xDEADBEEFu;
			for (NvU64 i = 0; i < kBufSize; i++) {
				srcBytes[i] = (NvU8)(i ^ (salt >> ((i & 3) * 8)));
			}
			memset(dstBytes, 0xCC, kBufSize);

			// Same value across all four slots — each release uses wSeq
			// because the kernel matches on (slot, value).
			for (NvU32 s = 0; s < kSlotCount; s++) {
				semSurf.RecordSubmission(s, wSeq);
			}
			// We only need to wake when the final stage's timestamp is
			// in place; the earlier slots are already written by then.
			semSurf.RegisterWaiter(kSlotFinal, wSeq, 0, (NvU64)osEvent.Fd());

			WriteSetObject(cmdBuf, kCeSubch, copyClass);

			// t0 — empty pipeline marker. WFI here drains any prior
			// channel work; on the very first iteration that's a no-op,
			// on later iterations it bounds out the previous Submit.
			auto emitTimestamp = [&](NvU32 slot) {
				semSurf.WriteRelease(cmdBuf, slot, wSeq, /*waitForIdle=*/true, /*releaseTimestamp=*/true);
			};

			emitTimestamp(kSlotT0);
			WriteCopyDma(cmdBuf, kCeSubch, srcCpu.GpuAddr(), srcGpu.GpuAddr(), kBufSize);
			emitTimestamp(kSlotUpload);
			WriteCopyDma(cmdBuf, kCeSubch, srcGpu.GpuAddr(), dstGpu.GpuAddr(), kBufSize);
			emitTimestamp(kSlotGpuGpu);
			WriteCopyDma(cmdBuf, kCeSubch, dstGpu.GpuAddr(), dstCpu.GpuAddr(), kBufSize);
			emitTimestamp(kSlotFinal);

			WriteNonStallInterrupt(cmdBuf);

			cmdBuf.SubmitTo(channel);

			while (semSurf.ReadValue(kSlotFinal) < wSeq) {
				// A CE fault (e.g. a bogus copy address) kills the channel
				// and leaves the semsurf release unfired; without this check
				// the loop hangs waiting on a value that will never land.
				channel.CheckError();
				struct pollfd pfd {
					.fd = osEvent.Fd(),
					.events = POLLIN | POLLPRI,
				};
				int pr = poll(&pfd, 1, 5000);
				if (pr < 0) {
					CheckErrno(pr);
				}
			}

			// PTIMER deltas — by the time slotFinal's value is observed,
			// the GPU has already written all earlier timestamp slots.
			NvU64 t0     = *semSurf.Timestamp(kSlotT0);
			NvU64 tUp    = *semSurf.Timestamp(kSlotUpload);
			NvU64 tGpu   = *semSurf.Timestamp(kSlotGpuGpu);
			NvU64 tFinal = *semSurf.Timestamp(kSlotFinal);

			NvU64 upload   = tUp    - t0;
			NvU64 gpugpu   = tGpu   - tUp;
			NvU64 download = tFinal - tGpu;
			NvU64 total    = tFinal - t0;

			sumUpload   += upload;
			sumGpuGpu   += gpugpu;
			sumDownload += download;
			sumTotal    += total;

			int cmp = memcmp(srcBytes, dstBytes, kBufSize);
			const char *verdict = (cmp == 0) ? "OK" : "MISMATCH";
			if (cmp != 0) mismatches++;

			printf("iter %2" PRIu64 "  up=%6.1f us (%7.1f MB/s)  "
				"gpu=%6.1f us (%7.1f MB/s)  "
				"dn=%6.1f us (%7.1f MB/s)  "
				"total=%6.1f us  %s\n",
				wSeq,
				upload   / 1000.0, BandwidthMBps(kBufSize, upload),
				gpugpu   / 1000.0, BandwidthMBps(kBufSize, gpugpu),
				download / 1000.0, BandwidthMBps(kBufSize, download),
				total    / 1000.0,
				verdict);
		}

		puts("");
		printf("avg     up=%6.1f us (%7.1f MB/s)  "
			"gpu=%6.1f us (%7.1f MB/s)  "
			"dn=%6.1f us (%7.1f MB/s)  "
			"total=%6.1f us\n",
			(sumUpload   / (double)iterations) / 1000.0, BandwidthMBps(kBufSize * iterations, sumUpload),
			(sumGpuGpu   / (double)iterations) / 1000.0, BandwidthMBps(kBufSize * iterations, sumGpuGpu),
			(sumDownload / (double)iterations) / 1000.0, BandwidthMBps(kBufSize * iterations, sumDownload),
			(sumTotal    / (double)iterations) / 1000.0);

		semSurf.UnbindChannel(channel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});

		if (mismatches != 0) {
			fprintf(stderr, "[!] %" PRIu64 " of %" PRIu64 " round-trips mismatched\n",
				mismatches, iterations);
			return 2;
		}
		return 0;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmChannelCopyDemo: %s\n", ex.what());
		return 1;
	}
}
