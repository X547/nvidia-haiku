/* The card's video decoder, as much of it as is not about any one codec.
 *
 * This opens resman, takes an address space, allocates buffers both sides can
 * see, and puts a channel on the decoder engine with the newest NVDEC class the
 * GPU has (NVC4B0 on Turing onwards). Work is a list of methods ending in
 * EXECUTE; the engine answers by releasing a semaphore, which the channel
 * waits for before raising the interrupt that wakes the waiting thread.
 *
 * The channel is the Volta-and-later kind (NvChannel from NvRmDemos): work is
 * kicked through the usermode doorbell, so Pascal and older GPUs are not
 * supported.
 */
#ifndef NVDEC_ENGINE_H
#define NVDEC_ENGINE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Codecs the engine can be asked for; the numbers are its own. */
#define NVDEC_CODEC_MPEG2	1
#define NVDEC_CODEC_VC1		2
#define NVDEC_CODEC_H264	3
#define NVDEC_CODEC_MPEG4	4
#define NVDEC_CODEC_VP8		5
#define NVDEC_CODEC_HEVC	7
#define NVDEC_CODEC_VP9		9

typedef struct NvdecEngine NvdecEngine;

/* A buffer with a place in the GPU's address space and in ours. */
typedef struct {
	uint64_t	gpuAddress;
	void		*data;
	uint64_t	size;
	bool		inSystemMemory;
	void		*driverState;
} NvdecBuffer;

/* Open the decoder. Returns NULL and, if `reason` is not NULL, leaves a line
 * there saying what failed. */
NvdecEngine *nvdecOpen(char *reason, size_t reasonSize);
void nvdecClose(NvdecEngine *engine);

bool nvdecAlloc(NvdecEngine *engine, NvdecBuffer *buffer, uint64_t size,
	bool inSystemMemory);
void nvdecFree(NvdecEngine *engine, NvdecBuffer *buffer);

/* Building a list of methods. `nvdecBegin` starts one, the rest add to it, and
 * `nvdecExecute` hands it over and waits. */
void nvdecBegin(NvdecEngine *engine);
void nvdecMethod(NvdecEngine *engine, uint32_t method, uint32_t value);
/* Addresses the engine is given are always in units of 256 bytes. */
void nvdecAddress(NvdecEngine *engine, uint32_t method, uint64_t gpuAddress);
bool nvdecExecute(NvdecEngine *engine, int timeoutMs);

/* What the engine said about the last picture. */
typedef struct {
	uint32_t	macroblocksDecoded;
	uint32_t	macroblocksInError;
	uint32_t	errorStatus;
	uint32_t	sliceHeaderError;
	uint32_t	cycles;
} NvdecStatus;

void nvdecGetStatus(const NvdecEngine *engine, NvdecStatus *status);
uint64_t nvdecStatusAddress(const NvdecEngine *engine);

/* Have the copy engine copy the picture the list being built decodes out of
 * the card's arrangement into plain lines, once the decoder is done with it:
 * the copy waits for the decoder on the card, not on this thread. `luma` and
 * `chroma` are the planes' card addresses, `lumaSize` and `chromaSize` their
 * sizes, whole sixteen-line blocks of `pitch` bytes. Call it before
 * nvdecExecute. Without a copy engine it does nothing. */
void nvdecQueueLinearCopy(NvdecEngine *engine, uint64_t luma, uint64_t chroma,
	int pitch, size_t lumaSize, size_t chromaSize);

/* The plain-line copy of the picture whose card-arranged luma plane is at
 * `tiledLuma` (an address in one of the engine's buffers), once the copy is
 * done: false if there is none, and the picture has to be read in the card's
 * arrangement. Lines are as far apart as in the card's arrangement. The copy
 * is pinned: it stays valid, even after the picture's surface is given back
 * and decoded into again, until nvdecReleaseLinearPlanes(`*pin`). */
bool nvdecLinearPlanes(NvdecEngine *engine, const uint8_t *tiledLuma,
	const uint8_t **luma, const uint8_t **chroma, void **pin);
void nvdecReleaseLinearPlanes(NvdecEngine *engine, void *pin);

/* Where the decoder puts the pixel at (x, y) of a plane whose lines are
 * `pitch` bytes apart. A 512 byte group covers 64 columns and 8 lines; two of
 * them stack into a block sixteen lines tall, and blocks run across the
 * picture and then down it. Measured on a GP102, by decoding a picture whose
 * every line was a different value and then one whose every column was. On
 * an Ampere GA106 this is the copy engine's block-linear layout with blocks
 * two GOBs tall (tools/ceuntiletest.cpp). */
size_t nvdecTileOffset(int x, int y, int pitch);

/* Copy one plane out of the card's arrangement into consecutive lines. */
void nvdecUntile(uint8_t *out, size_t outPitch, const uint8_t *tiled,
	int width, int height, int pitch);

/* The methods of NVC2B0, which every NVDEC class shares. */
#define NVC2B0_VIDEO_DECODER			0x0000c2b0
#define NVC2B0_SET_OBJECT			0x0000
#define NVC2B0_NOP				0x0100
#define NVC2B0_SET_APPLICATION_ID		0x0200
#define NVC2B0_SET_WATCHDOG_TIMER		0x0204
#define NVC2B0_SEMAPHORE_A			0x0240
#define NVC2B0_SEMAPHORE_B			0x0244
#define NVC2B0_SEMAPHORE_C			0x0248
#define NVC2B0_EXECUTE				0x0300
#define NVC2B0_SEMAPHORE_D			0x0304
#define NVC2B0_SEMAPHORE_D_STRUCTURE_SIZE_ONE	(0u << 0)
#define NVC2B0_SEMAPHORE_D_OPERATION_RELEASE	(0u << 16)
#define NVC2B0_SET_CONTROL_PARAMS		0x0400
#define NVC2B0_SET_DRV_PIC_SETUP_OFFSET		0x0404
#define NVC2B0_SET_IN_BUF_BASE_OFFSET		0x0408
#define NVC2B0_SET_PICTURE_INDEX		0x040c
#define NVC2B0_SET_SLICE_OFFSETS_BUF_OFFSET	0x0410
#define NVC2B0_SET_COLOC_DATA_OFFSET		0x0414
#define NVC2B0_SET_HISTORY_OFFSET		0x0418
#define NVC2B0_SET_DISPLAY_BUF_SIZE		0x041c
#define NVC2B0_SET_HISTOGRAM_OFFSET		0x0420
#define NVC2B0_SET_NVDEC_STATUS_OFFSET		0x0424
#define NVC2B0_SET_DISPLAY_BUF_LUMA_OFFSET	0x0428
#define NVC2B0_SET_DISPLAY_BUF_CHROMA_OFFSET	0x042c
#define NVC2B0_SET_PICTURE_LUMA_OFFSET0		0x0430
#define NVC2B0_SET_PICTURE_CHROMA_OFFSET0	0x0474
#define NVC2B0_SET_PIC_SCRATCH_BUF_OFFSET	0x04b8
#define NVC2B0_SET_EXTERNAL_MVBUFFER_OFFSET	0x04bc
#define NVC2B0_H264_SET_MBHIST_BUF_OFFSET	0x0500

#ifdef __cplusplus
}
#endif

#endif	/* NVDEC_ENGINE_H */
