// NvRmMemoryMapperDemo
//
// Demonstrates NV_MEMORY_MAPPER (class 0x00fe) — a GPU-driven paging-ops
// "channel" that lets userspace queue MAP / UNMAP / SEMAPHORE_WAIT /
// SEMAPHORE_SIGNAL operations against a target VirtualMemory range, with
// semaphore synchronization against other channels via a shared
// NV_SEMAPHORE_SURFACE.
//
// Pipeline:
//   GART1 (sysmem) --[CE copy]--> VRAM --[CE copy]--> GART2 (sysmem)
//
// The VRAM range starts unbound — only its virtual reservation is live, the
// physical pages are a separate NV01_MEMORY_LOCAL_USER allocation. The
// NV_MEMORY_MAPPER object binds them at runtime around the copy and tears
// the binding back down afterwards. The CE channel and the MEMORY_MAPPER
// rendezvous through three slots in a shared semsurf:
//   slot 0 — "map done"   : MEMORY_MAPPER signals, CE waits before copying
//   slot 1 — "copy done"  : CE releases (WFI), MEMORY_MAPPER waits before unmap
//   slot 2 — "unmap done" : MEMORY_MAPPER signals, CPU waits via OS event
//
// Only one OS event fd is registered (against slot 2). Once both engines are
// submitted, the CPU sits on a single poll() / value-check loop until the
// final signal lands; GART2 is then verified against GART1.
//
// MEMORY_MAPPER processes its op queue from a kernel work item: MAP and UNMAP
// take a queue iteration apiece, and SEMAPHORE_WAIT yields the worker until
// the semsurf callback re-queues it on the next signal. From the user's
// point of view the call to NV00FE_CTRL_CMD_SUBMIT_OPERATIONS is purely
// non-blocking — enqueue everything and walk away.

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <memory>
#include <system_error>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
#include "nvos.h"
#include "class/cl0040.h"               // NV01_MEMORY_LOCAL_USER
#include "class/cl00fe.h"               // NV_MEMORY_MAPPER, NV_MEMORY_MAPPER_*
#include "class/cl50a0.h"               // NV50_MEMORY_VIRTUAL
#include "class/cl2080_notification.h"  // NV2080_NOTIFIERS_FIFO_EVENT_MTHD, NV2080_NOTIFIERS_RC_ERROR
#include "class/clc5b5.h"               // TURING_DMA_COPY_A
#include "class/clc6b5.h"               // AMPERE_DMA_COPY_A
#include "class/clc7b5.h"               // AMPERE_DMA_COPY_B
#include "class/clc8b5.h"               // HOPPER_DMA_COPY_A
#include "class/clc9b5.h"               // BLACKWELL_DMA_COPY_A
#include "class/clcab5.h"               // BLACKWELL_DMA_COPY_B
#include "ctrl/ctrl0080/ctrl0080gpu.h"  // NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2
#include "ctrl/ctrl00fe.h"              // NV00FE_CTRL_CMD_SUBMIT_OPERATIONS
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
// 1 MB is plenty to demonstrate the path while keeping a single CE LAUNCH
// chain modest; WriteCopyDma still chunks it internally at 128 KB.
static const NvU64 kBufSize = 1ull * 1024 * 1024;

// Semsurf slot layout. Values are 1 across the board — this demo runs a
// single pass; per-iteration sequencing isn't needed.
enum {
	kSlotMapDone   = 0,
	kSlotCopyDone  = 1,
	kSlotUnmapDone = 2,
	kSlotCount     = 3,
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


// Allocate a VirtualMemory range without binding any physical pages to it.
// NV_MEMORY_ALLOCATION_PARAMS::offset is an IN/OUT field — when the kernel
// satisfies a VIRTUAL allocation it writes the assigned base GPU VA back
// into it (see virtmemConstruct_IMPL in open-gpu-kernel-modules). That base
// VA is what NV_MEMORY_MAPPER's MAP uses internally via DMA_OFFSET_FIXED,
// and what the CE channel must use when copying to/from the range.
static NvHandle AllocUnboundVirtual(
	NvRmApi &rm, NvHandle hDevice, NvHandle hVaSpace,
	NvU64 size, NvU64 align, NvU64 &gpuAddrOut)
{
	NV_MEMORY_ALLOCATION_PARAMS params {
		.owner = rm.Client().Get(),
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB),
		.size = size,
		.alignment = align,
		.hVASpace = hVaSpace,
	};
	NvHandle hVirt = rm.Alloc(NV50_MEMORY_VIRTUAL, hDevice, &params, sizeof(params));
	gpuAddrOut = params.offset;
	return hVirt;
}


// Build the MEMORY_MAPPER program:
//   MAP -> SIGNAL map-done -> WAIT copy-done -> UNMAP -> SIGNAL unmap-done.
//
// SUBMIT_OPERATIONS_PARAMS carries a 4096-entry array — bigger than we want
// to put on the main stack, so allocate on the heap. operationsCount tells
// the kernel how many entries to walk; the rest can stay zero.
static void SubmitMemoryMapperOps(
	NvRmObject &memMapper,
	NvHandle hVirtVram, NvHandle hPhysVram, NvU64 bufSize)
{
	auto params = std::make_unique<NV00FE_CTRL_SUBMIT_OPERATIONS_PARAMS>();
	memset(params.get(), 0, sizeof(*params));

	NvU32 n = 0;
	{
		NV00FE_CTRL_OPERATION &op = params->pOperations[n++];
		op.type = NV00FE_CTRL_OPERATION_TYPE_MAP;
		op.data.map.hVirtualMemory  = hVirtVram;
		op.data.map.virtualOffset   = 0;
		op.data.map.hPhysicalMemory = hPhysVram;
		op.data.map.physicalOffset  = 0;
		op.data.map.size            = bufSize;
		op.data.map.dmaFlags        = 0;
	}
	{
		NV00FE_CTRL_OPERATION &op = params->pOperations[n++];
		op.type = NV00FE_CTRL_OPERATION_TYPE_SEMAPHORE_SIGNAL;
		op.data.semaphore.index = kSlotMapDone;
		op.data.semaphore.value = 1;
	}
	{
		NV00FE_CTRL_OPERATION &op = params->pOperations[n++];
		op.type = NV00FE_CTRL_OPERATION_TYPE_SEMAPHORE_WAIT;
		op.data.semaphore.index = kSlotCopyDone;
		op.data.semaphore.value = 1;
	}
	{
		NV00FE_CTRL_OPERATION &op = params->pOperations[n++];
		op.type = NV00FE_CTRL_OPERATION_TYPE_UNMAP;
		op.data.unmap.hVirtualMemory = hVirtVram;
		op.data.unmap.virtualOffset  = 0;
		op.data.unmap.size           = bufSize;
		op.data.unmap.dmaFlags       = 0;
	}
	{
		NV00FE_CTRL_OPERATION &op = params->pOperations[n++];
		op.type = NV00FE_CTRL_OPERATION_TYPE_SEMAPHORE_SIGNAL;
		op.data.semaphore.index = kSlotUnmapDone;
		op.data.semaphore.value = 1;
	}
	params->operationsCount = n;

	memMapper.Control(NV00FE_CTRL_CMD_SUBMIT_OPERATIONS, params.get(), sizeof(*params));
}


int main()
{
	try {
		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		// Graphics-engine channel driving CE work via SET_OBJECT routing
		// (same approach as NvRmChannelCopyDemo). Channel host services the
		// semaphore acquire/release methods regardless of which engine owns
		// the bound subchannel.
		NvChannel channel(rmDev);
		NvCommandBuffer cmdBuf(channel);

		NvU32 copyClass = PickCopyClass(rmDev);
		NvRmObject ceObj = channel.Channel().Alloc(copyClass, nullptr, 0);

		// Two GART buffers (sysmem) — auto-mapped into the channel's VA space
		// at allocation. CE addresses them directly.
		NvGpuMemory gart1, gart2;
		gart1.AllocSystem(rmDev, channel.VaSpace(), kBufSize, 0x1000, /*mapCpu=*/true);
		gart2.AllocSystem(rmDev, channel.VaSpace(), kBufSize, 0x1000, /*mapCpu=*/true);

		// VRAM physical pages — no virtual binding yet.
		NV_MEMORY_ALLOCATION_PARAMS vramPhysParams {
			.owner = rm.Client().Get(),
			.type  = NVOS32_TYPE_IMAGE,
			.flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
			.attr =
				DRF_DEF(OS32, _ATTR, _LOCATION,    _VIDMEM) |
				DRF_DEF(OS32, _ATTR, _COHERENCY,   _UNCACHED) |
				DRF_DEF(OS32, _ATTR, _PHYSICALITY, _CONTIGUOUS) |
				DRF_DEF(OS32, _ATTR, _PAGE_SIZE,   _DEFAULT),
			.size = kBufSize,
			.alignment = 0x1000,
		};
		NvRmObject vramPhys(rm, rm.Alloc(
			NV01_MEMORY_LOCAL_USER, rmDev.Subdevice().Get(),
			&vramPhysParams, sizeof(vramPhysParams)));

		// VRAM virtual range — reserves a GPU VA window in the channel's
		// VA space without populating any pages. MEMORY_MAPPER will bind
		// vramPhys into it.
		NvU64 vramGpuAddr = 0;
		NvRmObject vramVirt(rm, AllocUnboundVirtual(
			rm, rmDev.Device().Get(), channel.VaSpace(),
			kBufSize, 0x1000, vramGpuAddr));

		// Semaphore surface — 3 slots. Channel binding lets RM scan it on
		// the CE's non-stall interrupts so registered waiters (the
		// MEMORY_MAPPER internal callback for slot 1, and the OS-event fd
		// for slot 2) wake at the right values.
		NvSemSurf semSurf(rmDev, channel.VaSpace(), kSlotCount);
		semSurf.BindChannel(channel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});

		// Notification memory backs NV_MEMORY_MAPPER_NOTIFICATION (just an
		// NvU32 status). The kernel writes to it via BAR1; on sysmem the
		// CPU mapping is the simplest way to see the result.
		NvGpuMemory notifyMem;
		notifyMem.AllocSystem(rmDev, channel.VaSpace(), 0x1000, 0x1000, /*mapCpu=*/true);
		auto *notif = (volatile NV_MEMORY_MAPPER_NOTIFICATION *)notifyMem.CpuAddr();
		notif->status = 0;

		// Allocate NV_MEMORY_MAPPER under the subdevice. It internally dupes
		// the semsurf handle so it can register its own waiters when running
		// SEMAPHORE_WAIT ops; userspace channels still use the original
		// handle for their own release/acquire methods.
		NV_MEMORY_MAPPER_ALLOCATION_PARAMS mmParams {
			.hSemaphoreSurface   = semSurf.Handle(),
			.maxQueueSize        = 32,
			.hNotificationMemory = notifyMem.PhysHandle(),
			.notificationOffset  = 0,
		};
		NvRmObject memMapper = rmDev.Subdevice().Alloc(
			NV_MEMORY_MAPPER, &mmParams, sizeof(mmParams));

		// Single OS event — wakes only on the final unmap-done signal. The
		// RC_ERROR binding gives us an extra wakeup path if the CE channel
		// is killed mid-flight, so we don't sit on the poll timeout in that
		// case.
		NvOsEvent osEvent(rm);
		NvRmObject hErrorEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_RC_ERROR, /*nonStallIntr=*/false);

		printf("channel:           %#" PRIx32 "\n", channel.Handle());
		printf("ce class:          %#" PRIx32 "\n", copyClass);
		printf("ce object:         %#" PRIx32 "\n", ceObj.Get());
		printf("buffer size:       %" PRIu64 " bytes\n", (NvU64)kBufSize);
		printf("gart1 gpu addr:    %#" PRIx64 "  (sysmem, cpu-mapped)\n", gart1.GpuAddr());
		printf("gart2 gpu addr:    %#" PRIx64 "  (sysmem, cpu-mapped)\n", gart2.GpuAddr());
		printf("vram phys handle:  %#" PRIx32 "  (vidmem, no virt binding)\n", vramPhys.Get());
		printf("vram virt handle:  %#" PRIx32 "  gpu addr: %#" PRIx64 "  (reserved, unbound)\n",
			vramVirt.Get(), vramGpuAddr);
		printf("memory mapper:     %#" PRIx32 "\n", memMapper.Get());
		printf("semsurf:           %#" PRIx32 "  slots=%d  64bit=%d\n",
			semSurf.Handle(), kSlotCount, (int)semSurf.Is64Bit());
		puts("");

		auto srcBytes = (NvU8 *)gart1.CpuAddr();
		auto dstBytes = (NvU8 *)gart2.CpuAddr();

		// Pattern with a positional fingerprint — any partial or misaligned
		// copy lights up in the final memcmp.
		for (NvU64 i = 0; i < kBufSize; i++) {
			srcBytes[i] = (NvU8)((i * 0x9E37u + 0xC0DEu) >> ((i & 3) * 4));
		}
		memset(dstBytes, 0xCC, kBufSize);

		// Publish the target values CPU-side before any submission — required
		// on hardware without native 64-bit semaphores (no-op otherwise) so
		// ReadValue() can synthesize the full 64-bit semantic afterwards.
		semSurf.RecordSubmission(kSlotMapDone,   1);
		semSurf.RecordSubmission(kSlotCopyDone,  1);
		semSurf.RecordSubmission(kSlotUnmapDone, 1);

		// Install the single CPU waiter *before* submitting — MEMORY_MAPPER
		// on a short queue can race the registration if we wait until after.
		semSurf.RegisterWaiter(kSlotUnmapDone, 1, 0, (NvU64)osEvent.Fd());

		// CE pushbuffer:
		//   ACQUIRE map-done  -> COPY gart1 -> vram
		//                     -> COPY vram  -> gart2
		//                     -> RELEASE copy-done (WFI)
		//                     -> non-stall interrupt
		{
			WriteSetObject(cmdBuf, kCeSubch, copyClass);

			semSurf.WriteAcquire(cmdBuf, kSlotMapDone, 1);
			WriteCopyDma(cmdBuf, kCeSubch, gart1.GpuAddr(), vramGpuAddr, kBufSize);
			WriteCopyDma(cmdBuf, kCeSubch, vramGpuAddr, gart2.GpuAddr(), kBufSize);
			semSurf.WriteRelease(cmdBuf, kSlotCopyDone, 1, /*waitForIdle=*/true);
			WriteNonStallInterrupt(cmdBuf);
		}

		// Queue all five MEMORY_MAPPER ops in one SUBMIT_OPERATIONS call.
		// The control returns as soon as the entries are validated and
		// queued; the actual MAP/UNMAP/wait/signal runs from the kernel
		// worker.
		SubmitMemoryMapperOps(memMapper, vramVirt.Get(), vramPhys.Get(), kBufSize);

		// Kick the CE channel. The ACQUIRE at the front parks the engine
		// on the host scheduler until MEMORY_MAPPER lands the map-done
		// signal — no CPU coordination needed between the two submissions.
		cmdBuf.SubmitTo(channel);

		// Single CPU wait. If the channel dies the OS event also fires via
		// the RC_ERROR binding; CheckError() converts that to an exception.
		// notif->status is the MEMORY_MAPPER worker's error channel: a
		// validation failure (bad handle, etc.) is reported here and the
		// worker stops, so slot 2 would never signal — surface it instead
		// of hanging.
		while (semSurf.ReadValue(kSlotUnmapDone) < 1) {
			channel.CheckError();
			NvU32 mmStatus = notif->status;
			if (mmStatus != 0) {
				throw std::system_error((int)mmStatus,
					NvStatusErrorCategory::Instance(),
					"NV_MEMORY_MAPPER worker reported error");
			}
			struct pollfd pfd {
				.fd = osEvent.Fd(),
				.events = POLLIN | POLLPRI,
			};
			int pr = poll(&pfd, 1, 5000);
			if (pr < 0) {
				CheckErrno(pr);
			}
		}

		printf("slot map-done    = %" PRIu64 "\n", semSurf.ReadValue(kSlotMapDone));
		printf("slot copy-done   = %" PRIu64 "\n", semSurf.ReadValue(kSlotCopyDone));
		printf("slot unmap-done  = %" PRIu64 "\n", semSurf.ReadValue(kSlotUnmapDone));
		printf("notif status     = %#" PRIx32 "\n", (NvU32)notif->status);

		int cmp = memcmp(srcBytes, dstBytes, kBufSize);
		printf("verify           = %s\n", cmp == 0 ? "OK" : "MISMATCH");

		semSurf.UnbindChannel(channel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});

		return cmp == 0 ? 0 : 2;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmMemoryMapperDemo: %s\n", ex.what());
		return 1;
	}
}
