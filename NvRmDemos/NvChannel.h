#pragma once

#include <cassert>
#include <cstddef>

#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvGpuMemory.h"


class NvCommandBuffer;


// A single GPFIFO entry: GPU address + length (in dwords) of an already-
// composed batch of methods sitting in some channel-mapped buffer. Submit()
// takes an array of these so a caller can chain several physically separate
// command buffers behind one doorbell ring.
struct NvCmdEntry {
	NvU64 gpuAddr;
	NvU32 lengthDwords;
};


// A bound, scheduled GPFIFO channel with everything it needs to submit work:
// its own VA space, a usermode mapping for the doorbell, error notifier,
// UserD, and GP FIFO. Command buffers are owned externally — see
// NvCommandBuffer — and Submit() takes the GPU address + length of each
// already-composed batch.
class NvChannel {
private:
	NvRmDevice *fDev {};

	NvRmObject fVaSpace;
	NvRmObject fUsermode;
	NvRmMemoryMapping fUsermodeMap;

	NvGpuMemory fNotifier;
	NvGpuMemory fUserD;
	NvGpuMemory fGpFifo;

	// Per-submit progress tracker. fProgress holds a single monotonic NvU32
	// the GPU releases after every submission's GP entries; fProgressPush is
	// a ring of method blocks (one per GP slot) holding those release
	// methods. Reading fProgress tells us how many GP entries the host has
	// consumed — driving GPFIFO free-slot tracking here and command-buffer
	// chunk reclamation in NvCommandBuffer, so no CPU wait is needed between
	// submissions.
	NvGpuMemory fProgress;
	NvGpuMemory fProgressPush;

	NvRmObject fCtxDma;
	NvRmObject fChannel;

	NvU32 fNumGpEntries {};
	NvU32 fGpPut {};         // physical GP ring slot (0..fNumGpEntries-1)
	NvU32 fGpPutTotal {};    // monotonic count of GP entries ever written
	NvU32 fSubmitToken {};

	// Write the progress-tracker semaphore release (a 32-bit RELEASE, no WFI)
	// into fProgressPush slot `slot`, releasing `payload`.
	void EmitProgressRelease(NvU32 slot, NvU32 payload);

public:
	NvChannel() = default;
	NvChannel(NvChannel &&) = default;
	NvChannel &operator=(NvChannel &&) = default;
	NvChannel(const NvChannel &) = delete;
	NvChannel &operator=(const NvChannel &) = delete;

	// `engineType` is an NV2080_ENGINE_TYPE_* value (GRAPHICS by default).
	// Use COPY0..COPYn for DMA copy engine channels.
	NvChannel(NvRmDevice &dev, NvU32 engineType = 1 /* NV2080_ENGINE_TYPE_GRAPHICS */);

	NvRmDevice &Device() const {return *fDev;}
	const NvRmObject &Channel() const {return fChannel;}
	NvHandle Handle() const {return fChannel.Get();}
	NvHandle VaSpace() const {return fVaSpace.Get();}

	// Submit `count` batches as `count` GP entries plus one channel-owned
	// progress-tracker entry, then publish them via a single GPPut update +
	// doorbell ring. Blocks only if the GPFIFO ring is full, advancing as
	// the GPU consumes prior submissions — no CPU wait is required between
	// submissions. Calls CheckError() first (and while waiting) and refuses
	// to submit if the channel is dead.
	//
	// Returns the cumulative GP-entry total after this submission; the same
	// value the GPU will publish via the progress tracker once it has
	// retired this submission's entries. Pass it to SeqReached() to test
	// completion. Returns the current total unchanged if count == 0.
	NvU32 Submit(const NvCmdEntry *entries, std::size_t count);

	// Monotonic count of GP entries the host has consumed, read from the
	// progress-tracker semaphore. Conservative: only advances when the host
	// retires a progress entry, so it never overstates progress.
	NvU32 CompletedGpTotal() const;

	// Cumulative GP-entry total of the most recent submission (the value its
	// Submit() returned).
	NvU32 SubmittedGpTotal() const {return fGpPutTotal;}

	// True once the host has consumed through `target` (a value returned by
	// Submit). Wrap-safe over the monotonic 32-bit counter.
	bool SeqReached(NvU32 target) const;

	// Spin until every submitted entry has been consumed by the host,
	// calling CheckError() each pass so a channel kill aborts the wait.
	void WaitIdle();

	// Robust-channel error inspection. RM populates the channel's error
	// notifier (slot NV906F_NOTIFIERS_RC of fNotifier) when it kills the
	// channel; both methods read that slot.
	//
	// HasError() returns true once RM has reported an error. The notifier
	// is sticky — once set it stays set for the channel's lifetime.
	//
	// CheckError() throws std::system_error with the NV_STATUS from the
	// notifier and a message containing the ROBUST_CHANNEL_* exception
	// type and engine id (matching info32/info16 in the notifier).
	bool HasError() const;
	void CheckError() const;
};


// Push-buffer command helpers writing into a bounded NvU32* cursor. The
// writer asserts every U32 against `limit` so callers that overrun their
// reserved range trip immediately rather than corrupting memory past the
// buffer.
class NvPushWriter {
private:
	NvU32 *fPtr;
	NvU32 *fLimit;

public:
	NvPushWriter(NvU32 *ptr, NvU32 *limit): fPtr(ptr), fLimit(limit) {}

	NvU32 *Ptr() const {return fPtr;}
	NvU32 *Limit() const {return fLimit;}

	void U32(NvU32 val) {assert(fPtr < fLimit); *fPtr++ = val;}
	void U64(NvU64 val) {U32((NvU32)val); U32((NvU32)(val >> 32));}
	void IncMethod(NvU32 subch, NvU32 offset, NvU32 count);
	// Like IncMethod but the `count` data dwords are all dispatched to the
	// same method address (the engine does not auto-increment). Used to stream
	// repeated writes of a trigger method — e.g. solid-prim vertex points.
	void NonIncMethod(NvU32 subch, NvU32 offset, NvU32 count);
};


// Each helper opens its own NvCommandBuffer::Push() reservation, writes
// the methods, and closes via EndPush() — callers don't need to track
// reservations or compose around the 512-dword per-Push cap. For helpers
// with size-dependent emission (WriteCopyDma) the reservation happens per
// internal batch, so arbitrarily large operations work and chunk swaps
// happen at safe boundaries.

// Write a 64-bit semaphore release at GPFIFO subchannel 0 (host class):
// the GPU writes `value` to `gpuAddr` (and, if waitForIdle, only after all
// preceding channel work has retired). When `releaseTimestamp` is true the
// GPU additionally writes the current PTIMER value as an NvU64 at
// gpuAddr+8 — useful for measuring per-stage GPU time between two
// timestamped releases.
void WriteSemRelease64(NvCommandBuffer &cmdBuf, NvU64 gpuAddr, NvU64 value, bool waitForIdle, bool releaseTimestamp = false);

// 32-bit variant for use against semaphore slots on GPUs that do not
// natively support 64-bit semaphores (see NvSemSurf::Is64Bit). The full
// 64-bit semantic is reconstructed CPU-side via NvSemSurf::RecordSubmission.
void WriteSemRelease32(NvCommandBuffer &cmdBuf, NvU64 gpuAddr, NvU32 value, bool waitForIdle, bool releaseTimestamp = false);

// Block the channel's host until the 64-bit semaphore at `gpuAddr` reaches
// `value` (ACQ_STRICT_GEQ). Methods after this on any subchannel are held
// in the host until the value lands, then dispatched normally. Use to gate
// engine work on an external GPU signal (e.g. an NV_MEMORY_MAPPER signal
// recorded into the same NV_SEMAPHORE_SURFACE this channel later releases
// into).
void WriteSemAcquire64(NvCommandBuffer &cmdBuf, NvU64 gpuAddr, NvU64 value);

// 32-bit ACQ_STRICT_GEQ variant for slots on hardware without native
// 64-bit semaphores. The on-GPU value is compared as 32-bit, so the caller
// is responsible for ensuring values cannot wrap past UINT32_MAX over the
// channel's lifetime (in practice this is fine for per-iteration counters).
void WriteSemAcquire32(NvCommandBuffer &cmdBuf, NvU64 gpuAddr, NvU32 value);

// Bind subchannel `subch` to an engine class so subsequent method writes
// at addresses >= 0x100 on that subchannel are routed to the class. The
// channel must have been allocated against an engine that exposes this
// class. `engineId` is 0 for the channel's primary engine and 0x1f for SW.
void WriteSetObject(NvCommandBuffer &cmdBuf, NvU32 subch, NvU32 classId, NvU32 engineId = 0);

// Emit a contiguous host-DMA copy of `size` bytes from `srcGpuAddr` to
// `dstGpuAddr` against a TURING_DMA_COPY_A (or compatible) object already
// bound to `subch`. Both addresses are GPU virtual; both buffers must be
// mapped in the channel's VA space. The helper chunks large copies into
// 128 KB LAUNCH_DMA invocations internally (matching mesa's safe limit),
// reserving a fresh Push per batch so copies of any size fit.
void WriteCopyDma(NvCommandBuffer &cmdBuf, NvU32 subch, NvU64 srcGpuAddr, NvU64 dstGpuAddr, NvU64 size);

// Emit a non-stall interrupt method: the channel signals the host interrupt
// notifier without blocking. Use this to wake a subdevice OS event.
void WriteNonStallInterrupt(NvCommandBuffer &cmdBuf);
