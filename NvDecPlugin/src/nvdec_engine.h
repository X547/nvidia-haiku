/* The card's video decoder, as much of it as is not about any one codec.
 *
 * This opens resman, takes an address space, allocates buffers both sides can
 * see, and puts a channel on the decoder engine with an NVC2B0 object on it.
 * Work is a list of methods ending in EXECUTE; the engine answers by releasing
 * a semaphore.
 *
 * The channel is the pre-Volta kind: resman owns the USERD page, which is
 * reached by mapping the channel itself, and there is no doorbell, so work is
 * kicked by writing GPPut.
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

/* Where the decoder puts the pixel at (x, y) of a plane whose lines are
 * `pitch` bytes apart. A 512 byte group covers 64 columns and 8 lines; two of
 * them stack into a block sixteen lines tall, and blocks run across the
 * picture and then down it. Measured on a GP102, by decoding a picture whose
 * every line was a different value and then one whose every column was: the
 * arrangement inside a group is not the one used for graphics surfaces. */
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
