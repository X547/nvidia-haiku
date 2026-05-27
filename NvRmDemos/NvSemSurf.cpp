#include "NvSemSurf.h"

#include <string.h>
#include <utility>

extern "C" {
#include "class/cl00da.h"  // NV_SEMAPHORE_SURFACE, NV_SEMAPHORE_SURFACE_ALLOC_PARAMETERS
#include "ctrl/ctrl00da.h" // NV_SEMAPHORE_SURFACE_CTRL_*
}

#include <ErrorUtils.h>

#include "NvChannel.h"


NvSemSurf::NvSemSurf(NvRmDevice &dev, NvHandle hVaSpace, NvU64 numEntries):
	fDev(&dev)
{
	// Pull the layout from the subdevice first so we know how many bytes to
	// reserve per slot and whether the GPU needs a separate max-submitted
	// tracking region.
	dev.Subdevice().Control(NV2080_CTRL_CMD_FB_GET_SEMAPHORE_SURFACE_LAYOUT, &fLayout, sizeof(fLayout));

	fNumEntries = numEntries;

	const NvU64 pageSize = 0x1000;
	NvU64 size = fLayout.size * numEntries;
	size = (size + pageSize - 1) & ~(pageSize - 1);
	if (size < pageSize) {
		size = pageSize;
	}

	fMemory.AllocSystem(dev, hVaSpace, size, pageSize, true);

	// Initialize slots to zero so the first GPU release (value = 1) is a
	// strict monotonic step rather than aliasing whatever the page allocator
	// handed us.
	memset((void*)fMemory.CpuAddr(), 0, size);

	NV_SEMAPHORE_SURFACE_ALLOC_PARAMETERS params {
		.hSemaphoreMem = fMemory.PhysHandle(),
	};
	if (!(fLayout.caps & NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_CAPS_64BIT_SEMAPHORES_SUPPORTED)) {
		// GPUs without native 64-bit semaphores synthesize them from the
		// 32-bit GPU-visible value plus a CPU-maintained high half kept in
		// the max-submitted region.
		params.hMaxSubmittedMem = fMemory.PhysHandle();
	}
	fSemSurf = dev.Subdevice().Alloc(NV_SEMAPHORE_SURFACE, &params, sizeof(params));
}

NvU64 NvSemSurf::GpuAddr(NvU64 index) const
{
	return fMemory.GpuAddr() + index * fLayout.size;
}

void NvSemSurf::WriteRelease(NvCommandBuffer &cmdBuf, NvU64 index, NvU64 value, bool waitForIdle, bool releaseTimestamp) const
{
	NvU64 addr = GpuAddr(index);
	if (Is64Bit()) {
		WriteSemRelease64(cmdBuf, addr, value, waitForIdle, releaseTimestamp);
	} else {
		WriteSemRelease32(cmdBuf, addr, (NvU32)value, waitForIdle, releaseTimestamp);
	}
}

void NvSemSurf::WriteAcquire(NvCommandBuffer &cmdBuf, NvU64 index, NvU64 value) const
{
	NvU64 addr = GpuAddr(index);
	if (Is64Bit()) {
		WriteSemAcquire64(cmdBuf, addr, value);
	} else {
		WriteSemAcquire32(cmdBuf, addr, (NvU32)value);
	}
}

NvU64 NvSemSurf::ReadValue(NvU64 index) const
{
	NvU8 *slot = (NvU8*)fMemory.CpuAddr() + index * fLayout.size;

	if (Is64Bit()) {
		// Single-shot 64-bit read; the GPU writes the full value atomically.
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		return __atomic_load_n((volatile NvU64*)slot, __ATOMIC_ACQUIRE);
	}

	// On non-64-bit hardware the GPU only writes the low 32 bits. The
	// high 32 bits come from CPU-maintained max-submitted; we may race
	// against an in-flight max-submitted update by the next submission,
	// in which case the GPU's 32-bit value may already have wrapped past
	// the low half of max-submitted. Detect and correct that, following
	// the NvTimeSemFermiGetPayload convention.
	volatile NvU32 *pSem = (volatile NvU32*)slot;
	volatile NvU64 *pMax = (volatile NvU64*)(slot + fLayout.maxSubmittedSemaphoreValueOffset);

	NvU32 semVal = __atomic_load_n(pSem, __ATOMIC_ACQUIRE);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	NvU64 maxSubmitted = __atomic_load_n(pMax, __ATOMIC_ACQUIRE);

	if ((maxSubmitted & 0xFFFFFFFFull) < semVal) {
		maxSubmitted -= 0x100000000ull;
	}
	return (NvU64)semVal | (maxSubmitted & 0xFFFFFFFF00000000ull);
}

void NvSemSurf::RecordSubmission(NvU64 index, NvU64 value)
{
	if (Is64Bit()) {
		return;
	}
	volatile NvU64 *pMax = (volatile NvU64*)(
		(NvU8*)fMemory.CpuAddr() + index * fLayout.size + fLayout.maxSubmittedSemaphoreValueOffset);

	// Semaphore values are monotonically non-decreasing per slot, so a
	// straight release-ordered store is enough; the corresponding ReadValue
	// uses acquire loads.
	__atomic_store_n(pMax, value, __ATOMIC_RELEASE);
}

volatile NvU64 *NvSemSurf::Timestamp(NvU64 index) const
{
	return (volatile NvU64*)((NvU8*)fMemory.CpuAddr() + index * fLayout.size + 8);
}

void NvSemSurf::BindChannel(NvHandle hChannel, std::initializer_list<NvU32> notifyIndices)
{
	NV_SEMAPHORE_SURFACE_CTRL_BIND_CHANNEL_PARAMS params {
		.hChannel = hChannel,
		.numNotifyIndices = (NvU32)notifyIndices.size(),
	};
	NvU32 i = 0;
	for (NvU32 ni : notifyIndices) {
		params.notifyIndices[i++] = ni;
	}
	fSemSurf.Control(NV_SEMAPHORE_SURFACE_CTRL_CMD_BIND_CHANNEL, &params, sizeof(params));
}

void NvSemSurf::UnbindChannel(NvHandle hChannel, std::initializer_list<NvU32> notifyIndices)
{
	NV_SEMAPHORE_SURFACE_CTRL_UNBIND_CHANNEL_PARAMS params {
		.hChannel = hChannel,
		.numNotifyIndices = (NvU32)notifyIndices.size(),
	};
	NvU32 i = 0;
	for (NvU32 ni : notifyIndices) {
		params.notifyIndices[i++] = ni;
	}
	fSemSurf.Control(NV_SEMAPHORE_SURFACE_CTRL_CMD_UNBIND_CHANNEL, &params, sizeof(params));
}

void NvSemSurf::RegisterWaiter(NvU64 index, NvU64 waitValue, NvU64 newValue, NvU64 notificationHandle)
{
	NV_SEMAPHORE_SURFACE_CTRL_REGISTER_WAITER_PARAMS params {
		.index = index,
		.waitValue = waitValue,
		.newValue = newValue,
		.notificationHandle = notificationHandle,
	};
	fSemSurf.Control(NV_SEMAPHORE_SURFACE_CTRL_CMD_REGISTER_WAITER, &params, sizeof(params));
}

void NvSemSurf::UnregisterWaiter(NvU64 index, NvU64 waitValue, NvU64 notificationHandle)
{
	NV_SEMAPHORE_SURFACE_CTRL_UNREGISTER_WAITER_PARAMS params {
		.index = index,
		.waitValue = waitValue,
		.notificationHandle = notificationHandle,
	};
	fSemSurf.Control(NV_SEMAPHORE_SURFACE_CTRL_CMD_UNREGISTER_WAITER, &params, sizeof(params));
}

void NvSemSurf::SetValue(NvU64 index, NvU64 newValue)
{
	NV_SEMAPHORE_SURFACE_CTRL_SET_VALUE_PARAMS params {
		.index = index,
		.newValue = newValue,
	};
	fSemSurf.Control(NV_SEMAPHORE_SURFACE_CTRL_CMD_SET_VALUE, &params, sizeof(params));
}
