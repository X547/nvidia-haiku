#pragma once

#include <initializer_list>

#include <NvRmApi.h>
#include <NvRmDevice.h>

extern "C" {
#include "ctrl/ctrl2080/ctrl2080fb.h" // NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_PARAMS
}

#include "NvGpuMemory.h"


class NvCommandBuffer;


// RAII wrapper around an NV_SEMAPHORE_SURFACE: a CPU+GPU-mapped block of
// semaphore slots whose updates RM watches on behalf of registered waiters.
// Compared to attaching an NV01_EVENT_OS_EVENT to a subdevice notifier,
// semsurf only fires the registered OS event when a specific slot reaches
// a specific value, so callers don't need to spin on the in-memory value to
// distinguish the right wakeup.
//
// The wrapped surface is parented to the device's subdevice, owns its
// backing system memory, and uses the same memory handle for both the
// semaphore values and (on GPUs that need it) the max-submitted tracking
// region. Slot layout is taken from
// NV2080_CTRL_CMD_FB_GET_SEMAPHORE_SURFACE_LAYOUT.
class NvSemSurf {
private:
	NvRmDevice *fDev {};
	NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_PARAMS fLayout {};
	NvU64 fNumEntries {};
	NvGpuMemory fMemory;
	NvRmObject fSemSurf;

public:
	NvSemSurf() = default;
	NvSemSurf(NvSemSurf &&) = default;
	NvSemSurf &operator=(NvSemSurf &&) = default;
	NvSemSurf(const NvSemSurf &) = delete;
	NvSemSurf &operator=(const NvSemSurf &) = delete;

	// Allocate the semsurf and back it with `numEntries` slots of size
	// dictated by the device's layout, mapped into both CPU space and
	// `hVaSpace` so the GPU can write semaphore releases directly.
	NvSemSurf(NvRmDevice &dev, NvHandle hVaSpace, NvU64 numEntries = 1);

	bool IsSet() const {return fSemSurf.Get() != 0;}
	NvHandle Handle() const {return fSemSurf.Get();}
	const NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_PARAMS &Layout() const {return fLayout;}
	NvU64 NumEntries() const {return fNumEntries;}

	// True if the GPU writes the full 64-bit semaphore value directly.
	// False on older GPUs where the GPU only writes 32 bits and a 64-bit
	// monotonic semantic has to be synthesized by tracking the high half
	// CPU-side in the max-submitted region of the slot.
	bool Is64Bit() const
	{
		return (fLayout.caps & NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_CAPS_64BIT_SEMAPHORES_SUPPORTED) != 0;
	}

	// GPU virtual address of the semaphore value field of slot `index`.
	// Pass this to a SEM_RELEASE method to update the slot from the
	// channel. Use a 64-bit payload on 64-bit-capable hardware (see
	// Is64Bit()) and a 32-bit payload otherwise.
	NvU64 GpuAddr(NvU64 index) const;

	// Emit a SEM_RELEASE on slot `index` into `cmdBuf` that signals `value`,
	// picking the 64- or 32-bit release method to match the surface's payload
	// width (see Is64Bit()) so callers don't have to branch themselves. On
	// non-64-bit hardware pair this with RecordSubmission(index, value) so the
	// CPU-side high half stays in sync. See WriteSemRelease64/32 for the
	// meaning of `waitForIdle` and `releaseTimestamp`.
	void WriteRelease(NvCommandBuffer &cmdBuf, NvU64 index, NvU64 value, bool waitForIdle, bool releaseTimestamp = false) const;

	// Block the channel's host until slot `index` reaches `value`, picking the
	// 64- or 32-bit acquire method to match the surface's payload width (see
	// Is64Bit()). The counterpart of WriteRelease() on the consuming side.
	void WriteAcquire(NvCommandBuffer &cmdBuf, NvU64 index, NvU64 value) const;

	// Synthesized 64-bit semaphore value at `index`, in a representation
	// safe to compare against the wait values passed to RegisterWaiter.
	// On 64-bit-capable hardware this is just the value the GPU wrote; on
	// other hardware it combines the GPU-written 32-bit value with the
	// CPU-side max-submitted high half. The semaphore is monotonic, so
	// polling this is a valid completion check; the registered OS-event
	// notification only suspends the CPU while waiting.
	NvU64 ReadValue(NvU64 index) const;

	// On non-64-bit hardware, bump the CPU-side max-submitted tracker for
	// `index` to `value` so that ReadValue() (and RM, which uses the same
	// memory) can recover the full 64-bit semantic from the 32-bit on-GPU
	// value. Must be called before the corresponding submit and before
	// RegisterWaiter. No-op on 64-bit-capable hardware.
	void RecordSubmission(NvU64 index, NvU64 value);

	// Pointer to the 64-bit GPU PTIMER timestamp captured by a
	// SEM_RELEASE with RELEASE_TIMESTAMP=EN. The hardware writes it at
	// slot_base + 8. Units are GPU ticks; on Turing+ this is 1 ns/tick.
	// Only meaningful for slots whose release was emitted with the
	// timestamp flag and only safe to read once the slot's value has
	// been observed >= the corresponding submission value.
	volatile NvU64 *Timestamp(NvU64 index) const;

	// Channels that signal semsurf slots from the GPU must be bound first
	// so RM knows which notifiers to scan for waiter wakeups when the
	// channel raises an interrupt. notifyIndices comes from
	// cl2080_notification.h (typically NV2080_NOTIFIERS_FIFO_EVENT_MTHD
	// for the non-stall interrupt path). Wait-only channels can pass an
	// empty list.
	void BindChannel(NvHandle hChannel, std::initializer_list<NvU32> notifyIndices);
	void UnbindChannel(NvHandle hChannel, std::initializer_list<NvU32> notifyIndices);

	// Ask RM to signal `notificationHandle` (the fd of an NvOsEvent
	// previously registered against the same client) once the value at
	// `index` reaches `waitValue`. If `newValue` is non-zero it is written
	// back into the slot atomically with the wakeup. RM may signal
	// immediately if the value is already at or past waitValue.
	void RegisterWaiter(NvU64 index, NvU64 waitValue, NvU64 newValue, NvU64 notificationHandle);
	void UnregisterWaiter(NvU64 index, NvU64 waitValue, NvU64 notificationHandle);

	// CPU-side update: bumps the value and wakes any matching waiters.
	void SetValue(NvU64 index, NvU64 newValue);
};
