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

#include <OS.h>

extern "C" {
#include "nvtypes.h"
#include "nvos.h"
#include "nv-haiku.h"
#include "class/cl2080_notification.h"
#include "class/clb8b0.h"
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

	// Clock boost while pictures are being decoded (see RenewBoost). Off with
	// NVDEC_BOOST=0, or once resman refused it.
	bool boost {};
	bigtime_t boostRenewed {};

	NvdecEngine();
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

	// The channel raises a non-stall interrupt when work is done, which
	// resman reports as FIFO_EVENT_MTHD; a channel kill reports RC_ERROR. Both
	// wake the same fd.
	osEvent = NvOsEvent(rm);
	completionEvent = AllocSubdeviceOsEvent(dev, osEvent, NV2080_NOTIFIERS_FIFO_EVENT_MTHD, true);
	errorEvent = AllocSubdeviceOsEvent(dev, osEvent, NV2080_NOTIFIERS_RC_ERROR, false);

	const char *boostSetting = getenv("NVDEC_BOOST");
	boost = boostSetting == nullptr || strcmp(boostSetting, "0") != 0;
}


// Sleep until `done()`, woken by the channel's interrupts. Throws if
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
	(void)engine;
	delete static_cast<NvGpuMemory*>(buffer->driverState);
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

		done = WaitUntil(engine, engine->channel, timeoutMs, "the decoder", [&]() {
			return *semaphore == want;
		});
	} catch (const std::exception &e) {
		fprintf(stderr, "nvdec: %s\n", e.what());
	} catch (...) {
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
