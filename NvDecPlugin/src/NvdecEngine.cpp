/*
 * The NVDEC engine (nvdec_engine.h) on the GSP resource manager: the nvrm C++
 * SDK for the client, device and memory, and NvRmDemos' channel helpers for
 * submission, so the channel uses the Volta-and-later doorbell.
 */

#include "nvdec_engine.h"

#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <new>
#include <system_error>
#include <vector>

#include <OS.h>

extern "C" {
#include "nvtypes.h"
#include "nvos.h"
#include "nv-haiku.h"
#include "class/cl2080_notification.h"
#include "class/clb8b0.h"
#include "class/clc5b5.h"
#include "class/clc6b5.h"
#include "class/clc7b5.h"
#include "class/clc8b5.h"
#include "class/clc9b5.h"
#include "class/clcab5.h"
#include "class/clc4b0.h"
#include "class/clc6b0.h"
#include "class/clc7b0.h"
#include "class/clc9b0.h"
#include "class/clcdb0.h"
#include "class/clcfb0.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl2080/ctrl2080perf.h"
}

#include <ErrorUtils.h>
#include <FileDesc.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include <nvrm-helpers/NvChannel.h>
#include <nvrm-helpers/NvCommandBuffer.h>
#include <nvrm-helpers/NvGpuMemory.h>
#include <nvrm-helpers/NvOsEvent.h>

#include "video/nvdec_drv.h"


static const NvU64 kBufferAlign = 0x10000;

// SET_INTRA_TOP_BUF_OFFSET, from NVC7B0 (Ampere GA10x) on: the engine keeps
// the line above the current block row there for H.265, VP9 and AV1, and
// faults on address 0 without it. NVK hands it 64 KiB per picture whatever
// the picture size; the size it needs is not documented.
static const uint32_t kSetIntraTopBufOffset = 0x04c8;
static const NvU64 kIntraTopSize = 1 << 20;

// Newest first. NvChannel needs the usermode doorbell, so Turing is the oldest.
static const NvU32 kDecoderClasses[] = {
	NVCFB0_VIDEO_DECODER,
	NVCDB0_VIDEO_DECODER,
	NVB8B0_VIDEO_DECODER,
	NVC9B0_VIDEO_DECODER,
	NVC7B0_VIDEO_DECODER,
	NVC6B0_VIDEO_DECODER,
	NVC4B0_VIDEO_DECODER,
};

static const NvU32 kCopyClasses[] = {
	BLACKWELL_DMA_COPY_B,
	BLACKWELL_DMA_COPY_A,
	HOPPER_DMA_COPY_A,
	AMPERE_DMA_COPY_B,
	AMPERE_DMA_COPY_A,
	TURING_DMA_COPY_A,
};

static const NvU32 kCopySubchannel = 4;

// The decoder's surfaces are block linear with blocks two GOBs tall
// (tools/ceuntiletest.cpp).
static const NvU32 kSurfaceBlockHeightLog2 = 1;

static const int kCopyTimeoutMs = 2000;


// A buffer holding the plain-line copy of a decoded picture. It belongs to
// the surface the picture was decoded into (found by the card address of the
// surface's luma plane) until another picture is decoded into that surface;
// pinned, it is not reused even then, so a picture can be converted from it
// while the next ones are decoded.
struct LinearCopy {
	uint64_t tiledLuma {}; // 0: belongs to no surface
	NvGpuMemory memory;
	size_t chromaOffset {};
	// The copy semaphore value once the latest copy into it is done; 0 when
	// it holds nothing usable.
	uint64_t ticket {};
	bool pinned {};
};

struct PendingCopy {
	LinearCopy *target;
	uint64_t luma;
	uint64_t chroma;
	NvU32 pitch;
	NvU32 lumaLines;
	NvU32 chromaLines;
};


struct NvdecEngine {
	FileDesc devFd;
	NvRmApi rm;
	NvRmDevice dev;
	NvU32 decoderClass;
	NvChannel channel;
	NvRmObject decoder;
	NvCommandBuffer cmdBuf;
	NvGpuMemory semaphore;
	NvGpuMemory status;
	NvGpuMemory intraTop;
	NvOsEvent osEvent;
	NvRmObject completionEvent;
	NvRmObject errorEvent;
	uint32_t semaphoreValue {};
	// Set when building the method list failed; the next nvdecExecute()
	// reports it instead of submitting a partial list.
	bool methodsLost {};

	// Every buffer nvdecAlloc() made, to find a card address from a pointer.
	std::vector<const NvGpuMemory*> allocations;

	// Untiling on the copy engine, in the decoder channel's VA space. Absent
	// when the GPU has no copy class this knows or NVDEC_CPU_UNTILE is set;
	// the pictures are then untiled on the CPU while converting.
	std::unique_ptr<NvChannel> copyChannel;
	std::unique_ptr<NvCommandBuffer> copyCmdBuf;
	NvRmObject copyObject;
	NvU32 copyClass {};
	NvGpuMemory copySemaphore;
	uint64_t copySequence {};
	std::vector<std::unique_ptr<LinearCopy>> linearCopies;
	std::vector<PendingCopy> pendingCopies;

	// Clock boost while pictures are being decoded (see RenewBoost). Off with
	// NVDEC_BOOST=0, or once resman refused it.
	bool boost {};
	bigtime_t boostRenewed {};

	NvdecEngine();

	void SetUpCopyEngine();
};


// Resman expects the card's node to stay open while a client uses the card.
static FileDesc OpenDeviceNode()
{
	FileDesc fd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
	CheckErrno(fd.Get());
	return fd;
}


// The first of `candidates` the GPU has, or 0.
static NvU32 FindClass(NvRmDevice &dev, const NvU32 *candidates, size_t count)
{
	NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS params {};
	dev.Device().Control(NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &params, sizeof(params));

	for (size_t c = 0; c < count; c++) {
		for (NvU32 i = 0; i < params.numClasses; i++) {
			if (params.classList[i] == candidates[c]) {
				return candidates[c];
			}
		}
	}
	return 0;
}


static NvU32 PickDecoderClass(NvRmDevice &dev)
{
	const NvU32 decoderClass = FindClass(dev, kDecoderClasses, std::size(kDecoderClasses));
	if (decoderClass == 0) {
		throw std::system_error(ENOTSUP, std::generic_category(), "no NVDEC class this driver can use (Turing or later)");
	}
	return decoderClass;
}


NvdecEngine::NvdecEngine():
	devFd(OpenDeviceNode()),
	rm(),
	dev(rm, 0),
	decoderClass(PickDecoderClass(dev)),
	channel(dev, NV2080_ENGINE_TYPE_NVDEC0),
	decoder(),
	cmdBuf(channel)
{
	NV_BSP_ALLOCATION_PARAMETERS bspParams {
		.size = sizeof(NV_BSP_ALLOCATION_PARAMETERS),
		.prohibitMultipleInstances = NV_FALSE,
		.engineInstance = 0,
	};
	decoder = channel.Channel().Alloc(decoderClass, &bspParams, sizeof(bspParams));

	semaphore.AllocSystem(dev, channel.VaSpace(), 0x1000, 0x1000, true);
	memset(semaphore.CpuAddr(), 0, semaphore.Size());
	status.AllocSystem(dev, channel.VaSpace(), 0x1000, 0x1000, true);
	memset(status.CpuAddr(), 0, status.Size());

	if (decoderClass != NVC4B0_VIDEO_DECODER && decoderClass != NVC6B0_VIDEO_DECODER) {
		intraTop.AllocLocal(dev, channel.VaSpace(), kIntraTopSize, kBufferAlign);
	}

	// The channels raise a non-stall interrupt when work is done, which
	// resman reports as FIFO_EVENT_MTHD; a channel kill reports RC_ERROR. Both
	// wake the same fd.
	osEvent = NvOsEvent(rm);
	completionEvent = AllocSubdeviceOsEvent(dev, osEvent, NV2080_NOTIFIERS_FIFO_EVENT_MTHD, true);
	errorEvent = AllocSubdeviceOsEvent(dev, osEvent, NV2080_NOTIFIERS_RC_ERROR, false);

	SetUpCopyEngine();

	const char *boostSetting = getenv("NVDEC_BOOST");
	boost = boostSetting == nullptr || strcmp(boostSetting, "0") != 0;
}


void NvdecEngine::SetUpCopyEngine()
{
	if (getenv("NVDEC_CPU_UNTILE") != nullptr) {
		return;
	}
	copyClass = FindClass(dev, kCopyClasses, std::size(kCopyClasses));
	if (copyClass == 0) {
		return;
	}
	try {
		// An asynchronous copy engine of its own if resman gives one, the
		// graphics engine's otherwise.
		try {
			copyChannel = std::make_unique<NvChannel>(dev, NV2080_ENGINE_TYPE_COPY0, channel.VaSpace());
			copyObject = copyChannel->Channel().Alloc(copyClass, nullptr, 0);
		} catch (...) {
			copyObject = NvRmObject();
			copyChannel = std::make_unique<NvChannel>(dev, NV2080_ENGINE_TYPE_GRAPHICS, channel.VaSpace());
			copyObject = copyChannel->Channel().Alloc(copyClass, nullptr, 0);
		}
		copyCmdBuf = std::make_unique<NvCommandBuffer>(*copyChannel);
		copySemaphore.AllocSystem(dev, channel.VaSpace(), 0x1000, 0x1000, true);
		memset(copySemaphore.CpuAddr(), 0, copySemaphore.Size());
	} catch (const std::exception &e) {
		fprintf(stderr, "nvdec: no copy engine, untiling on the CPU: %s\n", e.what());
		copyCmdBuf.reset();
		copyObject = NvRmObject();
		copyChannel.reset();
	}
}


// Sleep until `done()`, woken by the channels' interrupts. Throws if
// `channel` was killed.
template <typename Done>
static bool WaitUntil(NvdecEngine *engine, NvChannel &channel, int timeoutMs, const char *what, Done done)
{
	const bigtime_t deadline = system_time() + (bigtime_t)timeoutMs * 1000;
	while (!done()) {
		channel.CheckError();
		const bigtime_t left = deadline - system_time();
		if (left <= 0) {
			fprintf(stderr, "nvdec: %s did not finish within %d ms\n", what, timeoutMs);
			return false;
		}
		// The wake-up can race the check above; a short poll bounds that.
		struct pollfd pfd {
			.fd = engine->osEvent.Fd(),
			.events = POLLIN | POLLPRI,
		};
		poll(&pfd, 1, (int)std::min<bigtime_t>(left / 1000 + 1, 10));
	}
	return true;
}


// Resman leaves the GPU in its idle P-state while a picture is decoded every
// few tens of milliseconds with the CPU converting in between, and NVDEC then
// runs at a third of its clock (a 4K picture takes 19 ms on an RTX A2000
// instead of 8.6). Ask for the highest P-state for two seconds, renewed at most
// once a second while pictures keep coming, so the clocks drop again soon
// after playback stops or pauses. Resman tracks the boost per client and
// accepts only the boost to the highest P-state on GSP firmware.
static void RenewBoost(NvdecEngine *engine)
{
	if (!engine->boost) {
		return;
	}
	const bigtime_t now = system_time();
	if (engine->boostRenewed != 0 && now - engine->boostRenewed < 1000000) {
		return;
	}
	try {
		NV2080_CTRL_PERF_BOOST_PARAMS params {
			.flags = NV2080_CTRL_PERF_BOOST_FLAGS_CMD_BOOST_TO_MAX,
			.duration = 2,
		};
		engine->dev.Subdevice().Control(NV2080_CTRL_CMD_PERF_BOOST, &params, sizeof(params));
		engine->boostRenewed = now;
	} catch (const std::exception &e) {
		fprintf(stderr, "nvdec: decoding at the GPU's current clocks, no boost: %s\n", e.what());
		engine->boost = false;
	}
}


static void ClearBoost(NvdecEngine *engine)
{
	if (engine->boostRenewed == 0) {
		return;
	}
	try {
		NV2080_CTRL_PERF_BOOST_PARAMS params {
			.flags = NV2080_CTRL_PERF_BOOST_FLAGS_CMD_CLEAR,
			.duration = 0,
		};
		engine->dev.Subdevice().Control(NV2080_CTRL_CMD_PERF_BOOST, &params, sizeof(params));
	} catch (...) {
	}
	engine->boostRenewed = 0;
}


static bool WaitForCopy(NvdecEngine *engine, uint64_t ticket)
{
	if (ticket == 0) {
		return true;
	}
	const volatile uint64_t *done = static_cast<const volatile uint64_t*>(engine->copySemaphore.CpuAddr());
	return WaitUntil(engine, *engine->copyChannel, kCopyTimeoutMs, "the copy engine", [&]() {
		return *done >= ticket;
	});
}


static void SetReason(char *reason, size_t reasonSize, const char *text)
{
	if (reason == nullptr || reasonSize == 0) {
		return;
	}
	snprintf(reason, reasonSize, "%s", text);
}


//#pragma mark - Engine

NvdecEngine *nvdecOpen(char *reason, size_t reasonSize)
{
	try {
		return new NvdecEngine();
	} catch (const std::exception &e) {
		SetReason(reason, reasonSize, e.what());
	} catch (...) {
		SetReason(reason, reasonSize, "opening the decoder failed");
	}
	return nullptr;
}


void nvdecClose(NvdecEngine *engine)
{
	if (engine == nullptr) {
		return;
	}
	try {
		engine->channel.WaitIdle();
	} catch (...) {
		// A dead channel has nothing left to wait for.
	}
	// The copies write into buffers about to be freed.
	if (engine->copyChannel != nullptr) {
		try {
			WaitForCopy(engine, engine->copySequence);
		} catch (...) {
		}
	}
	ClearBoost(engine);
	delete engine;
}


//#pragma mark - Buffers

bool nvdecAlloc(NvdecEngine *engine, NvdecBuffer *buffer, uint64_t size, bool inSystemMemory)
{
	memset(buffer, 0, sizeof(*buffer));
	size = (size + 0xfff) & ~0xfffull;

	try {
		auto memory = std::make_unique<NvGpuMemory>();
		if (inSystemMemory) {
			memory->AllocSystem(engine->dev, engine->channel.VaSpace(), size, kBufferAlign, true);
		} else {
			memory->AllocLocal(engine->dev, engine->channel.VaSpace(), size, kBufferAlign, true);
		}
		memset(memory->CpuAddr(), 0, size);

		buffer->gpuAddress = memory->GpuAddr();
		buffer->data = memory->CpuAddr();
		buffer->size = size;
		buffer->inSystemMemory = inSystemMemory;
		engine->allocations.push_back(memory.get());
		buffer->driverState = memory.release();
		return true;
	} catch (const std::exception &e) {
		fprintf(stderr, "nvdec: allocating %llu bytes of %s memory: %s\n",
			(unsigned long long)size,
			inSystemMemory ? "system" : "video",
			e.what()
		);
	} catch (...) {
	}
	return false;
}


void nvdecFree(NvdecEngine *engine, NvdecBuffer *buffer)
{
	auto *memory = static_cast<NvGpuMemory*>(buffer->driverState);
	auto &allocations = engine->allocations;
	allocations.erase(std::remove(allocations.begin(), allocations.end(), memory), allocations.end());
	delete memory;
	memset(buffer, 0, sizeof(*buffer));
}


//#pragma mark - Methods

static void EmitMethod(NvdecEngine *engine, uint32_t method, uint32_t value)
{
	NvPushWriter wr = engine->cmdBuf.Push(2);
	wr.IncMethod(0, method, 1);
	wr.U32(value);
	engine->cmdBuf.EndPush(wr);
}


void nvdecBegin(NvdecEngine *engine)
{
	engine->cmdBuf.Reset();
	engine->methodsLost = false;
	nvdecMethod(engine, NVC2B0_SET_OBJECT, engine->decoderClass);
	if (engine->intraTop.IsSet()) {
		nvdecAddress(engine, kSetIntraTopBufOffset, engine->intraTop.GpuAddr());
	}
}


void nvdecMethod(NvdecEngine *engine, uint32_t method, uint32_t value)
{
	if (engine->methodsLost) {
		return;
	}
	try {
		EmitMethod(engine, method, value);
	} catch (...) {
		engine->methodsLost = true;
	}
}


void nvdecAddress(NvdecEngine *engine, uint32_t method, uint64_t gpuAddress)
{
	nvdecMethod(engine, method, (uint32_t)(gpuAddress >> 8));
}


//#pragma mark - Untiling

void nvdecQueueLinearCopy(NvdecEngine *engine, uint64_t luma, uint64_t chroma, int pitch, size_t lumaSize, size_t chromaSize)
{
	if (engine->copyChannel == nullptr || pitch <= 0) {
		return;
	}
	try {
		const size_t needed = (lumaSize + chromaSize + 0xfff) & ~(size_t)0xfff;

		// The surface's previous picture is gone; its buffer is free unless
		// it is pinned.
		LinearCopy *target = nullptr;
		for (const auto &copy : engine->linearCopies) {
			if (copy->tiledLuma == luma) {
				copy->tiledLuma = 0;
				if (!copy->pinned) {
					target = copy.get();
				}
				break;
			}
		}
		if (target == nullptr) {
			for (const auto &copy : engine->linearCopies) {
				if (copy->tiledLuma == 0 && !copy->pinned) {
					target = copy.get();
					break;
				}
			}
		}
		if (target == nullptr) {
			engine->linearCopies.push_back(std::make_unique<LinearCopy>());
			target = engine->linearCopies.back().get();
		}

		// The copy engine may still be writing the buffer's last picture.
		if (!WaitForCopy(engine, target->ticket)) {
			return;
		}
		target->ticket = 0;
		if (target->memory.Size() < needed) {
			target->memory.AllocSystem(engine->dev, engine->channel.VaSpace(), needed, kBufferAlign, true);
		}
		target->tiledLuma = luma;
		target->chromaOffset = lumaSize;

		engine->pendingCopies.push_back(PendingCopy {
			.target = target,
			.luma = luma,
			.chroma = chroma,
			.pitch = (NvU32)pitch,
			.lumaLines = (NvU32)(lumaSize / pitch),
			.chromaLines = (NvU32)(chromaSize / pitch),
		});
	} catch (const std::exception &e) {
		fprintf(stderr, "nvdec: no linear copy of this picture: %s\n", e.what());
	} catch (...) {
	}
}


// Forget the copies queued for a picture that is not going to be decoded.
static void DropPendingCopies(NvdecEngine *engine)
{
	for (const PendingCopy &copy : engine->pendingCopies) {
		copy.target->ticket = 0;
	}
	engine->pendingCopies.clear();
}


// Submit the copies queued for the picture just submitted to the decoder. The
// copy channel waits on the card for the decoder's semaphore to reach
// `decodeValue`, so nothing waits on this thread in between.
static void SubmitPendingCopies(NvdecEngine *engine, uint64_t decodeSemaphore, uint32_t decodeValue)
{
	if (engine->pendingCopies.empty()) {
		return;
	}
	try {
		NvCommandBuffer &cmd = *engine->copyCmdBuf;
		WriteSemAcquire32(cmd, decodeSemaphore, decodeValue);
		WriteSetObject(cmd, kCopySubchannel, engine->copyClass);
		for (const PendingCopy &copy : engine->pendingCopies) {
			const uint64_t out = copy.target->memory.GpuAddr();
			WriteCopyBlockLinearToPitch(
				cmd, kCopySubchannel,
				copy.luma, copy.pitch, copy.lumaLines, kSurfaceBlockHeightLog2,
				out, copy.pitch,
				copy.pitch, copy.lumaLines
			);
			WriteCopyBlockLinearToPitch(
				cmd, kCopySubchannel,
				copy.chroma, copy.pitch, copy.chromaLines, kSurfaceBlockHeightLog2,
				out + copy.target->chromaOffset, copy.pitch,
				copy.pitch, copy.chromaLines
			);
		}
		const uint64_t ticket = ++engine->copySequence;
		WriteSemRelease64(cmd, engine->copySemaphore.GpuAddr(), ticket, true);
		WriteNonStallInterrupt(cmd);
		cmd.SubmitTo(*engine->copyChannel);

		for (const PendingCopy &copy : engine->pendingCopies) {
			copy.target->ticket = ticket;
		}
		engine->pendingCopies.clear();
	} catch (const std::exception &e) {
		fprintf(stderr, "nvdec: queueing the linear copy: %s\n", e.what());
		engine->copyCmdBuf->Reset();
		DropPendingCopies(engine);
	}
}


bool nvdecLinearPlanes(NvdecEngine *engine, const uint8_t *tiledLuma, const uint8_t **luma, const uint8_t **chroma, void **pin)
{
	if (engine->copyChannel == nullptr) {
		return false;
	}

	uint64_t tiledAddress = 0;
	for (const NvGpuMemory *memory : engine->allocations) {
		const auto *base = static_cast<const uint8_t*>(memory->CpuAddr());
		if (base != nullptr && tiledLuma >= base && tiledLuma < base + memory->Size()) {
			tiledAddress = memory->GpuAddr() + (uint64_t)(tiledLuma - base);
			break;
		}
	}
	if (tiledAddress == 0) {
		return false;
	}

	for (const auto &copy : engine->linearCopies) {
		if (copy->tiledLuma != tiledAddress || copy->ticket == 0) {
			continue;
		}
		try {
			if (!WaitForCopy(engine, copy->ticket)) {
				return false;
			}
		} catch (const std::exception &e) {
			fprintf(stderr, "nvdec: %s\n", e.what());
			return false;
		}
		*luma = static_cast<const uint8_t*>(copy->memory.CpuAddr());
		*chroma = *luma + copy->chromaOffset;
		copy->pinned = true;
		*pin = copy.get();
		return true;
	}
	return false;
}


void nvdecReleaseLinearPlanes(NvdecEngine *engine, void *pin)
{
	(void)engine;
	if (pin != nullptr) {
		static_cast<LinearCopy*>(pin)->pinned = false;
	}
}


bool nvdecExecute(NvdecEngine *engine, int timeoutMs)
{
	volatile uint32_t *semaphore = static_cast<volatile uint32_t*>(engine->semaphore.CpuAddr());
	const uint32_t want = ++engine->semaphoreValue;
	memset(engine->status.CpuAddr(), 0, sizeof(nvdec_status_s));

	const uint64_t semaphoreAddress = engine->semaphore.GpuAddr();
	nvdecMethod(engine, NVC2B0_SEMAPHORE_A, (uint32_t)(semaphoreAddress >> 32) & 0xff);
	nvdecMethod(engine, NVC2B0_SEMAPHORE_B, (uint32_t)semaphoreAddress);
	nvdecMethod(engine, NVC2B0_SEMAPHORE_C, want);
	nvdecMethod(engine, NVC2B0_SEMAPHORE_D,
		NVC2B0_SEMAPHORE_D_STRUCTURE_SIZE_ONE |
		NVC2B0_SEMAPHORE_D_OPERATION_RELEASE
	);

	if (engine->methodsLost) {
		engine->cmdBuf.Reset();
		DropPendingCopies(engine);
		fprintf(stderr, "nvdec: no room for the method list\n");
		return false;
	}

	bool done = false;
	try {
		// The engine's own AWAKEN interrupt does not reach FIFO_EVENT_MTHD, so
		// the host waits for the engine's release and raises the interrupt
		// itself: it then cannot fire before the picture is done.
		WriteSemAcquire32(engine->cmdBuf, semaphoreAddress, want);
		WriteNonStallInterrupt(engine->cmdBuf);
		RenewBoost(engine);
		engine->cmdBuf.SubmitTo(engine->channel);
		SubmitPendingCopies(engine, semaphoreAddress, want);

		done = WaitUntil(engine, engine->channel, timeoutMs, "the decoder", [&]() {
			return *semaphore == want;
		});
	} catch (const std::exception &e) {
		fprintf(stderr, "nvdec: %s\n", e.what());
	} catch (...) {
	}
	if (!done) {
		DropPendingCopies(engine);
		// Let a copy waiting for this picture go ahead; what it copies is
		// not used.
		*semaphore = want;
	}
	return done;
}


//#pragma mark - Status

uint64_t nvdecStatusAddress(const NvdecEngine *engine)
{
	return engine->status.GpuAddr();
}


void nvdecGetStatus(const NvdecEngine *engine, NvdecStatus *out)
{
	const auto *status = static_cast<const nvdec_status_s*>(engine->status.CpuAddr());
	out->macroblocksDecoded = status->mbs_correctly_decoded;
	out->macroblocksInError = status->mbs_in_error;
	out->errorStatus = status->error_status;
	out->sliceHeaderError = status->slice_header_error_code;
	out->cycles = status->cycle_count;
}


//#pragma mark - Picture layout

size_t nvdecTileOffset(int x, int y, int pitch)
{
	const int blockHeight = 2; // groups of eight lines per block
	const int blocksPerRow = pitch / 64;
	const int blockY = (y / 8) / blockHeight;
	const int groupInBlock = (y / 8) % blockHeight;
	const size_t base = ((size_t)(blockY * blocksPerRow + x / 64) * blockHeight + groupInBlock) * 512;
	const int xx = x % 64;
	const int yy = y % 8;
	return base + (xx / 32) * 256 + (yy / 4) * 128 + ((xx % 32) / 16) * 64 + (yy % 4) * 16 + (xx % 16);
}


void nvdecUntile(uint8_t *out, size_t outPitch, const uint8_t *tiled, int width, int height, int pitch)
{
	// Sixteen consecutive columns are consecutive bytes, so copy those in one
	// go rather than a byte at a time.
	for (int y = 0; y < height; y++) {
		uint8_t *line = out + (size_t)y * outPitch;
		int x = 0;
		for (; x + 16 <= width; x += 16) {
			memcpy(line + x, tiled + nvdecTileOffset(x, y, pitch), 16);
		}
		for (; x < width; x++) {
			line[x] = tiled[nvdecTileOffset(x, y, pitch)];
		}
	}
}
