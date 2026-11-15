/* See nvdec_engine.h. */

#include "nvdec_engine.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nvRmApi.h"
#include "nvos.h"
#include "nvstatus.h"
#include "class/cl0000.h"
#include "class/cl0002.h"
#include "class/cl003e.h"
#include "class/cl0040.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl2080_notification.h"
#include "class/cl50a0.h"
#include "class/cl90f1.h"
#include "class/cla16f.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrla06f/ctrla06fgpfifo.h"
#include "video/nvdec_drv.h"

#define NVC06F_CHANNEL_GPFIFO	0x0000c06f
#define USERD_GPGET		(0x88 / 4)
#define USERD_GPPUT		(0x8c / 4)
#define GPFIFO_ENTRIES		0x100
#define PUSHBUFFER_SIZE		0x10000

typedef struct {
	uint32_t	physical;
	uint32_t	virt;
	NvRmApiMapping	mapping;
} BufferState;

struct NvdecEngine {
	NvRmApi		rm;
	int		deviceFd;
	uint32_t	client, device, subdevice, vaSpace;
	uint32_t	errorNotifier, channel, decoder;
	NvRmApiMapping	userdMapping;
	volatile uint32_t *userd;
	NvdecBuffer	notifier, gpFifo, pushBuffer, semaphore, status;
	uint32_t	*pushAt;
	uint32_t	gpPut;
	uint32_t	semaphoreValue;
	char		*reason;
	size_t		reasonSize;
};

static bool
fail(NvdecEngine *engine, const char *what, uint32_t status)
{
	if (engine->reason != NULL && engine->reasonSize > 0)
		snprintf(engine->reason, engine->reasonSize, "%s failed: %#x", what, status);
	return false;
}

bool
nvdecAlloc(NvdecEngine *engine, NvdecBuffer *buffer, uint64_t size, bool inSystemMemory)
{
	memset(buffer, 0, sizeof(*buffer));
	size = (size + 0xfff) & ~0xfffull;
	buffer->size = size;
	buffer->inSystemMemory = inSystemMemory;

	BufferState *state = calloc(1, sizeof(BufferState));
	if (state == NULL)
		return fail(engine, "buffer bookkeeping", 0);
	buffer->driverState = state;

	NV_MEMORY_ALLOCATION_PARAMS physical = {
		.owner = engine->client,
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB),
		.size = size,
		.alignment = 0x1000,
	};
	if (inSystemMemory) {
		physical.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _PCI);
		physical.attr |= DRF_DEF(OS32, _ATTR, _COHERENCY, _CACHED);
	} else {
		physical.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM);
		physical.attr |= DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED);
		physical.flags |= NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM;
	}
	uint32_t status = nvRmApiAlloc(&engine->rm, engine->device, &state->physical,
		inSystemMemory ? NV01_MEMORY_SYSTEM : NV01_MEMORY_LOCAL_USER, &physical);
	if (status != NV_OK)
		return fail(engine, "memory", status);

	/* Let resman place the range. Asking for an address of our own choosing
	 * runs it out of page tables once a picture is big enough to need more
	 * than a megabyte or two. */
	NV_MEMORY_ALLOCATION_PARAMS virt = {
		.owner = engine->client,
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_VIRTUAL,
		.size = size,
		.alignment = 0x10000,
		.hVASpace = engine->vaSpace,
	};
	status = nvRmApiAlloc(&engine->rm, engine->device, &state->virt,
		NV50_MEMORY_VIRTUAL, &virt);
	if (status != NV_OK)
		return fail(engine, "address range", status);

	uint32_t mapFlags = DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL)
		| (inSystemMemory ? DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE)
				  : DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE));
	NvU64 dmaOffset = 0;
	status = nvRmApiMapMemoryDma(&engine->rm, engine->device, state->virt,
		state->physical, 0, size, mapFlags, &dmaOffset);
	if (status != NV_OK)
		return fail(engine, "mapping for the card", status);
	buffer->gpuAddress = dmaOffset;

	status = nvRmApiMapMemory(&engine->rm, engine->device, state->physical, 0, size,
		inSystemMemory, 0, &state->mapping);
	if (status != NV_OK)
		return fail(engine, "mapping for us", status);
	buffer->data = state->mapping.address;
	memset(buffer->data, 0, size);
	return true;
}

void
nvdecFree(NvdecEngine *engine, NvdecBuffer *buffer)
{
	BufferState *state = buffer->driverState;
	if (state == NULL)
		return;
	if (buffer->data != NULL)
		nvRmApiUnmapMemory(&engine->rm, engine->device, state->physical, 0, &state->mapping);
	if (state->virt != 0) {
		nvRmApiUnmapMemoryDma(&engine->rm, engine->device, state->virt,
			state->physical, 0, buffer->gpuAddress);
		nvRmApiFree(&engine->rm, state->virt);
	}
	if (state->physical != 0)
		nvRmApiFree(&engine->rm, state->physical);
	free(state);
	memset(buffer, 0, sizeof(*buffer));
}

NvdecEngine *
nvdecOpen(char *reason, size_t reasonSize)
{
	NvdecEngine *engine = calloc(1, sizeof(NvdecEngine));
	if (engine == NULL)
		return NULL;
	engine->reason = reason;
	engine->reasonSize = reasonSize;
	engine->deviceFd = -1;

	/* Every request goes through the control node; memory on the card is
	 * mapped through the card's own node, which is what nodeName is for. */
	engine->rm.fd = open("/dev/nvidiactl", O_RDWR);
	if (engine->rm.fd < 0) {
		fail(engine, "opening /dev/nvidiactl", 0);
		goto error;
	}
	engine->rm.nodeName = "/dev/graphics/nvidia0";
	engine->deviceFd = open(engine->rm.nodeName, O_RDWR);
	if (engine->deviceFd < 0) {
		fail(engine, "opening /dev/graphics/nvidia0", 0);
		goto error;
	}

	nv_ioctl_card_info_t cards[8];
	memset(cards, 0, sizeof(cards));
	uint32_t status = nvRmApiCardInfo(&engine->rm, cards, sizeof(cards));
	if (status != NV_OK) {
		fail(engine, "asking about the card", status);
		goto error;
	}
	status = nvRmApiAlloc(&engine->rm, 0, &engine->client, NV01_ROOT_CLIENT, NULL);
	if (status != NV_OK) {
		fail(engine, "client", status);
		goto error;
	}
	engine->rm.hClient = engine->client;

	NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = cards[0].gpu_id };
	status = nvRmApiControl(&engine->rm, engine->client,
		NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof(id));
	if (status != NV_OK) {
		fail(engine, "identifying the card", status);
		goto error;
	}
	NV0080_ALLOC_PARAMETERS deviceParams = {
		.deviceId = id.deviceInstance,
		.hClientShare = engine->client,
	};
	status = nvRmApiAlloc(&engine->rm, engine->client, &engine->device,
		NV01_DEVICE_0, &deviceParams);
	if (status != NV_OK) {
		fail(engine, "device", status);
		goto error;
	}
	NV2080_ALLOC_PARAMETERS subdeviceParams = { .subDeviceId = id.subDeviceInstance };
	status = nvRmApiAlloc(&engine->rm, engine->device, &engine->subdevice,
		NV20_SUBDEVICE_0, &subdeviceParams);
	if (status != NV_OK) {
		fail(engine, "subdevice", status);
		goto error;
	}
	NV_VASPACE_ALLOCATION_PARAMETERS vaParams = {
		.flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS,
	};
	status = nvRmApiAlloc(&engine->rm, engine->device, &engine->vaSpace,
		FERMI_VASPACE_A, &vaParams);
	if (status != NV_OK) {
		fail(engine, "address space", status);
		goto error;
	}

	if (!nvdecAlloc(engine, &engine->notifier, 0x1000, true)
		|| !nvdecAlloc(engine, &engine->gpFifo, GPFIFO_ENTRIES * 8, true)
		|| !nvdecAlloc(engine, &engine->pushBuffer, PUSHBUFFER_SIZE, true)
		|| !nvdecAlloc(engine, &engine->semaphore, 0x1000, true)
		|| !nvdecAlloc(engine, &engine->status, 0x1000, true)) {
		goto error;
	}

	NV_CONTEXT_DMA_ALLOCATION_PARAMS notifierParams = {
		.flags = DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL)
			| DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE),
		.hMemory = ((BufferState *)engine->notifier.driverState)->physical,
		.offset = 0,
		.limit = engine->notifier.size - 1,
	};
	status = nvRmApiAlloc(&engine->rm, engine->device, &engine->errorNotifier,
		NV01_CONTEXT_DMA, &notifierParams);
	if (status != NV_OK) {
		fail(engine, "error notifier", status);
		goto error;
	}

	NV_CHANNEL_ALLOC_PARAMS channelParams = {
		.hObjectError = engine->errorNotifier,
		.gpFifoOffset = engine->gpFifo.gpuAddress,
		.gpFifoEntries = GPFIFO_ENTRIES,
		.hVASpace = engine->vaSpace,
		.engineType = NV2080_ENGINE_TYPE_NVDEC0,
	};
	status = nvRmApiAlloc(&engine->rm, engine->device, &engine->channel,
		NVC06F_CHANNEL_GPFIFO, &channelParams);
	if (status != NV_OK) {
		fail(engine, "channel on the decoder", status);
		goto error;
	}
	/* Before Volta resman owns USERD, reached by mapping the channel. */
	status = nvRmApiMapMemory(&engine->rm, engine->subdevice, engine->channel, 0, 0x1000,
		false, DRF_DEF(OS33, _FLAGS, _FIFO_MAPPING, _ENABLE), &engine->userdMapping);
	if (status != NV_OK) {
		fail(engine, "the channel's control page", status);
		goto error;
	}
	engine->userd = engine->userdMapping.address;

	status = nvRmApiAlloc(&engine->rm, engine->channel, &engine->decoder,
		NVC2B0_VIDEO_DECODER, NULL);
	if (status != NV_OK) {
		fail(engine, "decoder object", status);
		goto error;
	}
	NVA06F_CTRL_BIND_PARAMS bind = { .engineType = NV2080_ENGINE_TYPE_NVDEC0 };
	status = nvRmApiControl(&engine->rm, engine->channel, NVA06F_CTRL_CMD_BIND,
		&bind, sizeof(bind));
	if (status != NV_OK) {
		fail(engine, "binding the channel", status);
		goto error;
	}
	NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS schedule = { .bEnable = NV_TRUE };
	status = nvRmApiControl(&engine->rm, engine->channel,
		NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &schedule, sizeof(schedule));
	if (status != NV_OK) {
		fail(engine, "scheduling the channel", status);
		goto error;
	}
	return engine;

error:
	nvdecClose(engine);
	return NULL;
}

void
nvdecClose(NvdecEngine *engine)
{
	if (engine == NULL)
		return;
	if (engine->decoder != 0)
		nvRmApiFree(&engine->rm, engine->decoder);
	if (engine->userd != NULL) {
		nvRmApiUnmapMemory(&engine->rm, engine->subdevice, engine->channel, 0,
			&engine->userdMapping);
	}
	if (engine->channel != 0)
		nvRmApiFree(&engine->rm, engine->channel);
	if (engine->errorNotifier != 0)
		nvRmApiFree(&engine->rm, engine->errorNotifier);
	nvdecFree(engine, &engine->status);
	nvdecFree(engine, &engine->semaphore);
	nvdecFree(engine, &engine->pushBuffer);
	nvdecFree(engine, &engine->gpFifo);
	nvdecFree(engine, &engine->notifier);
	if (engine->vaSpace != 0)
		nvRmApiFree(&engine->rm, engine->vaSpace);
	if (engine->subdevice != 0)
		nvRmApiFree(&engine->rm, engine->subdevice);
	if (engine->device != 0)
		nvRmApiFree(&engine->rm, engine->device);
	if (engine->client != 0)
		nvRmApiFree(&engine->rm, engine->client);
	if (engine->deviceFd >= 0)
		close(engine->deviceFd);
	if (engine->rm.fd >= 0)
		close(engine->rm.fd);
	free(engine);
}

void
nvdecBegin(NvdecEngine *engine)
{
	engine->pushAt = engine->pushBuffer.data;
	nvdecMethod(engine, NVC2B0_SET_OBJECT, NVC2B0_VIDEO_DECODER);
}

void
nvdecMethod(NvdecEngine *engine, uint32_t method, uint32_t value)
{
	uint32_t *end = (uint32_t *)engine->pushBuffer.data + PUSHBUFFER_SIZE / 4;
	if (engine->pushAt + 2 > end)
		return;
	/* A Fermi method header: increasing methods, one argument, subchannel 0. */
	*engine->pushAt++ = (0x2u << 28) | (1u << 16) | (method >> 2);
	*engine->pushAt++ = value;
}

void
nvdecAddress(NvdecEngine *engine, uint32_t method, uint64_t gpuAddress)
{
	nvdecMethod(engine, method, (uint32_t)(gpuAddress >> 8));
}

uint64_t
nvdecStatusAddress(const NvdecEngine *engine)
{
	return engine->status.gpuAddress;
}

void
nvdecGetStatus(const NvdecEngine *engine, NvdecStatus *out)
{
	const nvdec_status_s *status = engine->status.data;
	out->macroblocksDecoded = status->mbs_correctly_decoded;
	out->macroblocksInError = status->mbs_in_error;
	out->errorStatus = status->error_status;
	out->sliceHeaderError = status->slice_header_error_code;
	out->cycles = status->cycle_count;
}

bool
nvdecExecute(NvdecEngine *engine, int timeoutMs)
{
	volatile uint32_t *semaphore = engine->semaphore.data;
	uint32_t want = ++engine->semaphoreValue;
	*semaphore = 0;
	memset(engine->status.data, 0, sizeof(nvdec_status_s));

	nvdecMethod(engine, NVC2B0_SEMAPHORE_A,
		(uint32_t)(engine->semaphore.gpuAddress >> 32) & 0xff);
	nvdecMethod(engine, NVC2B0_SEMAPHORE_B, (uint32_t)engine->semaphore.gpuAddress);
	nvdecMethod(engine, NVC2B0_SEMAPHORE_C, want);
	nvdecMethod(engine, NVC2B0_SEMAPHORE_D, 0);

	uint32_t words = (uint32_t)(engine->pushAt - (uint32_t *)engine->pushBuffer.data);
	uint32_t *entry = (uint32_t *)engine->gpFifo.data + 2 * engine->gpPut;
	entry[0] = (uint32_t)engine->pushBuffer.gpuAddress & ~3u;
	entry[1] = ((uint32_t)(engine->pushBuffer.gpuAddress >> 32) & 0xff) | (words << 10);
	engine->gpPut = (engine->gpPut + 1) % GPFIFO_ENTRIES;
	engine->userd[USERD_GPPUT] = engine->gpPut;

	for (int waited = 0; waited < timeoutMs * 10; waited++) {
		if (*semaphore == want)
			return true;
		usleep(100);
	}
	return false;
}

size_t
nvdecTileOffset(int x, int y, int pitch)
{
	const int blockHeight = 2;		/* groups of eight lines per block */
	int blocksPerRow = pitch / 64;
	int blockY = (y / 8) / blockHeight;
	int groupInBlock = (y / 8) % blockHeight;
	size_t base = ((size_t)(blockY * blocksPerRow + x / 64) * blockHeight
		+ groupInBlock) * 512;
	int xx = x % 64, yy = y % 8;
	return base + (xx / 32) * 256 + (yy / 4) * 128
		+ ((xx % 32) / 16) * 64 + (yy % 4) * 16 + (xx % 16);
}

void
nvdecUntile(uint8_t *out, size_t outPitch, const uint8_t *tiled,
	int width, int height, int pitch)
{
	/* Sixteen consecutive columns are consecutive bytes, so copy those in
	 * one go rather than a byte at a time. */
	for (int y = 0; y < height; y++) {
		uint8_t *line = out + (size_t)y * outPitch;
		int x = 0;
		for (; x + 16 <= width; x += 16)
			memcpy(line + x, tiled + nvdecTileOffset(x, y, pitch), 16);
		for (; x < width; x++)
			line[x] = tiled[nvdecTileOffset(x, y, pitch)];
	}
}
