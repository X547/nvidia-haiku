#include "NvChannel.h"
#include "NvCommandBuffer.h"

#include <string.h>
#include <sstream>
#include <utility>

extern "C" {
#include "nvos.h"

#include "class/cl0002.h" // NV01_CONTEXT_DMA
#include "class/cl90f1.h" // FERMI_VASPACE_A
#include "class/cl906fsw.h" // NV906F_NOTIFIERS_RC, NV906f_NOTIFICATION_STATUS_ERROR_*
#include "class/cla16f.h" // KeplerBControlGPFifo, NVA16F_*
#include "class/clc361.h" // VOLTA_USERMODE_A, NVC361_NOTIFY_CHANNEL_PENDING
#include "class/clc36f.h" // VOLTA_CHANNEL_GPFIFO_A methods (NVC36F_SEM_*, NVC36F_NON_STALL_INTERRUPT)
#include "class/clc461.h" // TURING_USERMODE_A
#include "class/clc46f.h" // TURING_CHANNEL_GPFIFO_A
#include "class/clc5b5.h" // NVC5B5_* (TURING_DMA_COPY_A methods)
#include "class/cl2080_notification.h" // NV2080_ENGINE_TYPE_GRAPHICS, NV_CHANNELGPFIFO_NOTIFICATION_TYPE_*

#include "alloc/alloc_channel.h" // NV_CHANNEL_ALLOC_PARAMS

#include "ctrl/ctrla06f/ctrla06fgpfifo.h" // NVA06F_CTRL_CMD_BIND / SCHEDULE
#include "ctrl/ctrlc36f.h" // NVC36F_CTRL_*

#include "nverror.h" // ROBUST_CHANNEL_*
}

#include <ErrorUtils.h>


// Each progress-tracker entry is a Volta semaphore RELEASE: a method header
// plus five data dwords (SEM_ADDR_LO/HI, SEM_PAYLOAD_LO/HI, SEM_EXECUTE).
// Slots are padded to 8 dwords so each starts on a tidy 32-byte boundary.
static const NvU32 kProgressMethodDwords = 6;
static const NvU32 kProgressSlotDwords   = 8;


NvChannel::NvChannel(NvRmDevice &dev, NvU32 engineType):
	fDev(&dev)
{
	NvRmApi &rm = *dev.Device().Rm();

	// 1. Per-channel VA space + usermode mapping for the doorbell.
	{
		NV_VASPACE_ALLOCATION_PARAMETERS params {
			.flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS,
		};
		fVaSpace = dev.Device().Alloc(FERMI_VASPACE_A, &params, sizeof(params));
	}
	fUsermode = dev.Subdevice().Alloc(TURING_USERMODE_A, nullptr);
	fUsermodeMap = dev.MapMemory(fUsermode.Get(), false, 0, 4096, 0);

	// 2. Channel notifier memory + matching CtxDma (for the error notifier).
	// Zero the slots so the RC slot's `status` reads as NV_OK until RM
	// writes a non-zero NV_STATUS on a robust-channel error. RM fills
	// info16, info32, timeStamp, then status (in that order), so a CPU
	// reading `status != 0` is guaranteed to see consistent payload.
	fNotifier.AllocSystem(dev, fVaSpace.Get(), 0x1000, 0x1000, true);
	memset(fNotifier.CpuAddr(), 0, fNotifier.Size());
	{
		NV_CONTEXT_DMA_ALLOCATION_PARAMS params {
			.flags = DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL) |
				DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE),
			.hMemory = fNotifier.PhysHandle(),
			.offset = 0,
			.limit = fNotifier.Size() - 1,
		};
		fCtxDma = dev.Device().Alloc(NV01_CONTEXT_DMA, &params, sizeof(params));
	}

	// 3. UserD (channel control registers) and GP FIFO. UserD wants
	// 64KB-aligned; we only need a few bytes of it but the kernel allocates
	// a fixed slot. 256 GP entries (2KB) gives the pipeline depth multiple
	// submissions can be in flight against. Command buffers themselves are
	// owned by the caller (see NvCommandBuffer).
	fUserD.AllocSystem(dev, fVaSpace.Get(), 0x10000, 0x10000, true);
	fGpFifo.AllocSystem(dev, fVaSpace.Get(), 0x800, 0x1000, true);

	fNumGpEntries = (NvU32)(fGpFifo.Size() / 8);

	// 3b. Progress tracker: a monotonic semaphore plus a ring of method
	// blocks (one per GP slot) that release it. Zero the semaphore so the
	// first release (the running GP total, which starts above 0) is a clean
	// monotonic step. See the header for how the value is consumed.
	fProgress.AllocSystem(dev, fVaSpace.Get(), 0x1000, 0x1000, true);
	memset(fProgress.CpuAddr(), 0, fProgress.Size());
	fProgressPush.AllocSystem(dev, fVaSpace.Get(),
		(NvU64)fNumGpEntries * kProgressSlotDwords * sizeof(NvU32), 0x1000, true);

	// 4. Allocate the GPFIFO channel itself.
	{
		NV_CHANNEL_ALLOC_PARAMS params {
			.hObjectError = fCtxDma.Get(),
			.gpFifoOffset = fGpFifo.GpuAddr(),
			.gpFifoEntries = fNumGpEntries,
			.flags = 0,
			.hVASpace = fVaSpace.Get(),
			.hUserdMemory = {fUserD.PhysHandle()},
			.userdOffset = {0},
			.engineType = engineType,
		};
		fChannel = dev.Device().Alloc(TURING_CHANNEL_GPFIFO_A, &params, sizeof(params));

		// Bind to the requested engine and put the channel into the runlist.
		NVA06F_CTRL_BIND_PARAMS bindParams {.engineType = engineType};
		fChannel.Control(NVA06F_CTRL_CMD_BIND, &bindParams, sizeof(bindParams));

		NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS schedParams {.bEnable = NV_TRUE};
		fChannel.Control(NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &schedParams, sizeof(schedParams));
	}

	// 5. Ask RM to publish this channel's work-submit token into the
	// notifier slot, then read it out for the doorbell.
	{
		NVC36F_CTRL_GPFIFO_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX_PARAMS params {
			.index = NV_CHANNELGPFIFO_NOTIFICATION_TYPE_WORK_SUBMIT_TOKEN,
		};
		fChannel.Control(NVC36F_CTRL_CMD_GPFIFO_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX, &params, sizeof(params));
	}
	{
		NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS params {};
		fChannel.Control(NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN, &params, sizeof(params));
		fSubmitToken = params.workSubmitToken;
	}
	if (fSubmitToken == 0) {
		auto notifiers = (NvNotification*)fNotifier.CpuAddr();
		fSubmitToken = notifiers[NV_CHANNELGPFIFO_NOTIFICATION_TYPE_WORK_SUBMIT_TOKEN].info32;
	}
}


static void NvCpuMemFence()
{
#if defined(__i386__) || defined(__x86_64__)
	__asm__ __volatile__("sfence" : : : "memory");
#else
	__sync_synchronize();
#endif
}


void NvChannel::EmitProgressRelease(NvU32 slot, NvU32 payload)
{
	NvU32 *p = (NvU32*)fProgressPush.CpuAddr() + (std::size_t)slot * kProgressSlotDwords;
	NvU64 semAddr = fProgress.GpuAddr();

	p[0] =
		DRF_DEF(A16F, _DMA, _SEC_OP,            _INC_METHOD) |
		DRF_NUM(A16F, _DMA, _METHOD_COUNT,      5) |
		DRF_NUM(A16F, _DMA, _METHOD_SUBCHANNEL, 0) |
		DRF_NUM(A16F, _DMA, _METHOD_ADDRESS,    NVC36F_SEM_ADDR_LO >> 2);
	p[1] = NvU64_LO32(semAddr);  // NVC36F_SEM_ADDR_LO
	p[2] = NvU64_HI32(semAddr);  // NVC36F_SEM_ADDR_HI
	p[3] = payload;              // NVC36F_SEM_PAYLOAD_LO
	p[4] = 0;                    // NVC36F_SEM_PAYLOAD_HI
	p[5] =                       // NVC36F_SEM_EXECUTE
		DRF_DEF(C36F, _SEM_EXECUTE, _OPERATION,         _RELEASE) |
		DRF_DEF(C36F, _SEM_EXECUTE, _PAYLOAD_SIZE,      _32BIT) |
		DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_TIMESTAMP, _DIS) |
		// WFI=DIS: the host releases as soon as it has fetched the preceding
		// GP entries, which is all we need to reclaim ring slots and command
		// buffers — not full engine idle.
		DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_WFI,       _DIS);
}


NvU32 NvChannel::CompletedGpTotal() const
{
	return __atomic_load_n((volatile NvU32*)fProgress.CpuAddr(), __ATOMIC_ACQUIRE);
}

bool NvChannel::SeqReached(NvU32 target) const
{
	return (NvS32)(CompletedGpTotal() - target) >= 0;
}

void NvChannel::WaitIdle()
{
	while (!SeqReached(fGpPutTotal)) {
		CheckError();
	}
}


NvU32 NvChannel::Submit(const NvCmdEntry *entries, std::size_t count)
{
	if (count == 0) {
		return fGpPutTotal;
	}
	// One extra GP entry per submission carries the progress-tracker release.
	// Keep one slot reserved so a full ring is distinguishable from an empty
	// one; a request that can't fit even a fully drained ring is a
	// programmer error.
	NvU32 needed = (NvU32)count + 1;
	assert(needed <= fNumGpEntries - 1);

	// Refuse to submit on a channel RM has already killed: the host
	// engine won't execute the GP entries, the doorbell write is wasted,
	// and the caller would otherwise hang waiting on a sem that never fires.
	CheckError();

	// Block until the ring has room. CompletedGpTotal() is a conservative
	// lower bound on the host's GP_GET — it only advances when the host
	// retires a progress entry — so this never lets GPPut lap an unconsumed
	// entry. CheckError() each pass so a channel kill aborts the wait.
	while ((fNumGpEntries - 1) - (fGpPutTotal - CompletedGpTotal()) < needed) {
		CheckError();
	}

	NvU32 *gpFifo = (NvU32*)fGpFifo.CpuAddr();
	NvU32 slot = fGpPut;

	// Caller's work batches.
	for (std::size_t i = 0; i < count; i++) {
		NvU32 *entry = &gpFifo[slot * 2];
		entry[0] = DRF_NUM(A16F, _GP_ENTRY0, _GET, NvU64_LO32(entries[i].gpuAddr) >> 2);
		entry[1] = DRF_NUM(A16F, _GP_ENTRY1, _GET_HI, NvU64_HI32(entries[i].gpuAddr)) |
			DRF_NUM(A16F, _GP_ENTRY1, _LENGTH, entries[i].lengthDwords);
		slot = (slot + 1) % fNumGpEntries;
	}

	// Channel-owned progress entry, last in the submission. Its payload is
	// the GP total once the host has retired everything up to and including
	// this entry; the methods live in the progress-push slot matching the GP
	// slot they occupy (unique while in flight thanks to the reservation
	// above).
	NvU32 payload = fGpPutTotal + needed;
	EmitProgressRelease(slot, payload);
	{
		NvU64 addr = fProgressPush.GpuAddr() + (NvU64)slot * kProgressSlotDwords * sizeof(NvU32);
		NvU32 *entry = &gpFifo[slot * 2];
		entry[0] = DRF_NUM(A16F, _GP_ENTRY0, _GET, NvU64_LO32(addr) >> 2);
		entry[1] = DRF_NUM(A16F, _GP_ENTRY1, _GET_HI, NvU64_HI32(addr)) |
			DRF_NUM(A16F, _GP_ENTRY1, _LENGTH, kProgressMethodDwords);
		slot = (slot + 1) % fNumGpEntries;
	}

	// Make sure all GP FIFO entries (including wrapped ones), the progress
	// methods, and the referenced command buffers are visible before GPPut.
	NvCpuMemFence();

	auto userD = (KeplerBControlGPFifo*)fUserD.CpuAddr();
	userD->GPPut = slot;

	NvCpuMemFence();

	auto doorbell = (volatile NvU32*)((NvU8*)fUsermodeMap.Address() + NVC361_NOTIFY_CHANNEL_PENDING);
	*doorbell = fSubmitToken;

	fGpPut = slot;
	fGpPutTotal += needed;
	return fGpPutTotal;
}


// Name table for the ROBUST_CHANNEL_* exception types we're likely to see
// from a userspace channel kill (MMU faults, illegal methods, GR/CE faults).
// Anything else falls through to a numeric "ROBUST_CHANNEL_? (N)" rendering.
static const char *RobustChannelExceptionName(NvU32 exceptType)
{
	switch (exceptType) {
		case ROBUST_CHANNEL_GR_EXCEPTION:           return "GR_EXCEPTION";
		case ROBUST_CHANNEL_FAKE_ERROR:             return "FAKE_ERROR";
		case ROBUST_CHANNEL_FIFO_ERROR_MMU_ERR_FLT: return "FIFO_ERROR_MMU_ERR_FLT";
		case ROBUST_CHANNEL_PBDMA_ERROR:            return "PBDMA_ERROR";
		case ROBUST_CHANNEL_CE0_ERROR:              return "CE0_ERROR";
		case ROBUST_CHANNEL_CE1_ERROR:              return "CE1_ERROR";
		case ROBUST_CHANNEL_CE2_ERROR:              return "CE2_ERROR";
		case ROBUST_CHANNEL_RESETCHANNEL_VERIF_ERROR: return "RESETCHANNEL_VERIF_ERROR";
		case ROBUST_CHANNEL_GR_FAULT_DURING_CTXSW:  return "GR_FAULT_DURING_CTXSW";
		case ROBUST_CHANNEL_PREEMPTIVE_REMOVAL:     return "PREEMPTIVE_REMOVAL";
		case ROBUST_CHANNEL_GPU_ECC_DBE:            return "GPU_ECC_DBE";
		case ROBUST_CHANNEL_GR_CLASS_ERROR:         return "GR_CLASS_ERROR";
		case ROBUST_CHANNEL_CONTAINED_ERROR:        return "CONTAINED_ERROR";
		case ROBUST_CHANNEL_UNCONTAINED_ERROR:      return "UNCONTAINED_ERROR";
		case ROBUST_CHANNEL_GPU_HAS_FALLEN_OFF_THE_BUS: return "GPU_HAS_FALLEN_OFF_THE_BUS";
		default:                                    return nullptr;
	}
}

bool NvChannel::HasError() const
{
	auto notifiers = (volatile NvNotification*)fNotifier.CpuAddr();
	return notifiers[NV906F_NOTIFIERS_RC].status != 0;
}

void NvChannel::CheckError() const
{
	auto notifier = &((volatile NvNotification*)fNotifier.CpuAddr())[NV906F_NOTIFIERS_RC];
	NvV16 status = notifier->status;
	if (status == 0) {
		return;
	}
	// status is written last by RM (see notifyFillNvNotification), so the
	// info32/info16 reads below are guaranteed consistent at this point.
	NvU32 exceptType = (NvU32)notifier->info32;
	NvU16 engineId   = (NvU16)notifier->info16;

	std::ostringstream msg;
	msg << "channel " << std::hex << "0x" << fChannel.Get()
	    << " killed by RM: exceptType=";
	const char *name = RobustChannelExceptionName(exceptType);
	if (name != nullptr) {
		msg << "ROBUST_CHANNEL_" << name;
	} else {
		msg << "ROBUST_CHANNEL_? (" << std::dec << exceptType << ")";
	}
	msg << " engineId=" << std::dec << engineId;

	// status field is NV_STATUS-shaped; surface it via the same error
	// category the rest of the SDK uses so callers can match on it.
	throw std::system_error((int)status, NvStatusErrorCategory::Instance(), msg.str());
}


// ---------------------------------------------------------------------------

void NvPushWriter::IncMethod(NvU32 subch, NvU32 offset, NvU32 count)
{
	U32(
		DRF_DEF(A16F, _DMA, _SEC_OP,            _INC_METHOD) |
		DRF_NUM(A16F, _DMA, _METHOD_COUNT,      count) |
		DRF_NUM(A16F, _DMA, _METHOD_SUBCHANNEL, subch) |
		DRF_NUM(A16F, _DMA, _METHOD_ADDRESS,    offset >> 2)
	);
}

void NvPushWriter::NonIncMethod(NvU32 subch, NvU32 offset, NvU32 count)
{
	U32(
		DRF_DEF(A16F, _DMA, _SEC_OP,            _NON_INC_METHOD) |
		DRF_NUM(A16F, _DMA, _METHOD_COUNT,      count) |
		DRF_NUM(A16F, _DMA, _METHOD_SUBCHANNEL, subch) |
		DRF_NUM(A16F, _DMA, _METHOD_ADDRESS,    offset >> 2)
	);
}


void WriteSemRelease64(NvCommandBuffer &cmdBuf, NvU64 gpuAddr, NvU64 value, bool waitForIdle, bool releaseTimestamp)
{
	NvPushWriter wr = cmdBuf.Push(6);
	wr.IncMethod(0, NVC36F_SEM_ADDR_LO, 5);
	wr.U64(gpuAddr);
	wr.U64(value);
	wr.U32(
		DRF_DEF(C36F, _SEM_EXECUTE, _OPERATION, _RELEASE) |
		DRF_DEF(C36F, _SEM_EXECUTE, _PAYLOAD_SIZE, _64BIT) |
		(releaseTimestamp
			? DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_TIMESTAMP, _EN)
			: DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_TIMESTAMP, _DIS)) |
		(waitForIdle
			? DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_WFI, _EN)
			: DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_WFI, _DIS))
	);
	cmdBuf.EndPush(wr);
}

void WriteSemRelease32(NvCommandBuffer &cmdBuf, NvU64 gpuAddr, NvU32 value, bool waitForIdle, bool releaseTimestamp)
{
	// Method layout is the same as the 64-bit release; only the PAYLOAD_SIZE
	// bit changes and the high payload dword is ignored by the host.
	NvPushWriter wr = cmdBuf.Push(6);
	wr.IncMethod(0, NVC36F_SEM_ADDR_LO, 5);
	wr.U64(gpuAddr);
	wr.U32(value);
	wr.U32(0);
	wr.U32(
		DRF_DEF(C36F, _SEM_EXECUTE, _OPERATION, _RELEASE) |
		DRF_DEF(C36F, _SEM_EXECUTE, _PAYLOAD_SIZE, _32BIT) |
		(releaseTimestamp
			? DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_TIMESTAMP, _EN)
			: DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_TIMESTAMP, _DIS)) |
		(waitForIdle
			? DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_WFI, _EN)
			: DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_WFI, _DIS))
	);
	cmdBuf.EndPush(wr);
}

void WriteSemAcquire64(NvCommandBuffer &cmdBuf, NvU64 gpuAddr, NvU64 value)
{
	NvPushWriter wr = cmdBuf.Push(6);
	wr.IncMethod(0, NVC36F_SEM_ADDR_LO, 5);
	wr.U64(gpuAddr);
	wr.U64(value);
	wr.U32(
		DRF_DEF(C36F, _SEM_EXECUTE, _OPERATION, _ACQ_STRICT_GEQ) |
		DRF_DEF(C36F, _SEM_EXECUTE, _ACQUIRE_SWITCH_TSG, _EN) |
		DRF_DEF(C36F, _SEM_EXECUTE, _PAYLOAD_SIZE, _64BIT)
	);
	cmdBuf.EndPush(wr);
}

void WriteSemAcquire32(NvCommandBuffer &cmdBuf, NvU64 gpuAddr, NvU32 value)
{
	NvPushWriter wr = cmdBuf.Push(6);
	wr.IncMethod(0, NVC36F_SEM_ADDR_LO, 5);
	wr.U64(gpuAddr);
	wr.U32(value);
	wr.U32(0);
	wr.U32(
		DRF_DEF(C36F, _SEM_EXECUTE, _OPERATION, _ACQ_STRICT_GEQ) |
		DRF_DEF(C36F, _SEM_EXECUTE, _ACQUIRE_SWITCH_TSG, _EN) |
		DRF_DEF(C36F, _SEM_EXECUTE, _PAYLOAD_SIZE, _32BIT)
	);
	cmdBuf.EndPush(wr);
}

void WriteNonStallInterrupt(NvCommandBuffer &cmdBuf)
{
	NvPushWriter wr = cmdBuf.Push(2);
	wr.IncMethod(0, NVC36F_NON_STALL_INTERRUPT, 1);
	wr.U32(0);
	cmdBuf.EndPush(wr);
}

void WriteSetObject(NvCommandBuffer &cmdBuf, NvU32 subch, NvU32 classId, NvU32 engineId)
{
	NvPushWriter wr = cmdBuf.Push(2);
	wr.IncMethod(subch, NVC36F_SET_OBJECT, 1);
	wr.U32(
		DRF_NUM(C36F, _SET_OBJECT, _NVCLASS, classId) |
		DRF_NUM(C36F, _SET_OBJECT, _ENGINE,  engineId)
	);
	cmdBuf.EndPush(wr);
}

void WriteCopyDma(NvCommandBuffer &cmdBuf, NvU32 subch, NvU64 srcGpuAddr, NvU64 dstGpuAddr, NvU64 size)
{
	// Linear pitch copy: just OFFSET_IN/OUT, then LINE_LENGTH_IN + LINE_COUNT
	// (single line of `bytes`), then LAUNCH_DMA. PITCH_IN/OUT are only
	// meaningful for true multi-line transfers and are intentionally
	// omitted here — mesa's nvk_CmdCopyBuffer2 uses the same shape.
	//
	// Each 128 KB iteration reserves its own 10-dword Push so chunk swaps
	// happen on safe boundaries and arbitrarily large copies fit even
	// though a single Push is capped at kMaxPushDwords.
	const NvU64 kChunk = 1ull << 17; // 128 KB, matching mesa's safe limit.

	while (size > 0) {
		NvU32 bytes = (NvU32)((size > kChunk) ? kChunk : size);

		NvPushWriter wr = cmdBuf.Push(10);
		wr.IncMethod(subch, NVC5B5_OFFSET_IN_UPPER, 4);
		wr.U32((NvU32)(srcGpuAddr >> 32));
		wr.U32((NvU32)(srcGpuAddr & 0xFFFFFFFFu));
		wr.U32((NvU32)(dstGpuAddr >> 32));
		wr.U32((NvU32)(dstGpuAddr & 0xFFFFFFFFu));

		wr.IncMethod(subch, NVC5B5_LINE_LENGTH_IN, 2);
		wr.U32(bytes);
		wr.U32(1);

		wr.IncMethod(subch, NVC5B5_LAUNCH_DMA, 1);
		wr.U32(
			DRF_DEF(C5B5, _LAUNCH_DMA, _DATA_TRANSFER_TYPE, _NON_PIPELINED) |
			DRF_DEF(C5B5, _LAUNCH_DMA, _FLUSH_ENABLE,       _TRUE) |
			DRF_DEF(C5B5, _LAUNCH_DMA, _SEMAPHORE_TYPE,     _NONE) |
			DRF_DEF(C5B5, _LAUNCH_DMA, _INTERRUPT_TYPE,     _NONE) |
			DRF_DEF(C5B5, _LAUNCH_DMA, _SRC_MEMORY_LAYOUT,  _PITCH) |
			DRF_DEF(C5B5, _LAUNCH_DMA, _DST_MEMORY_LAYOUT,  _PITCH) |
			// MULTI_LINE_ENABLE=TRUE with LINE_COUNT=1 is what mesa uses
			// for a single-line linear copy; some configurations don't
			// accept the MULTI_LINE_ENABLE=FALSE path here.
			DRF_DEF(C5B5, _LAUNCH_DMA, _MULTI_LINE_ENABLE,  _TRUE)
		);
		cmdBuf.EndPush(wr);

		srcGpuAddr += bytes;
		dstGpuAddr += bytes;
		size -= bytes;
	}
}
