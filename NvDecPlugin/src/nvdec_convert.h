/* Turning what the decoder produces into something that can be drawn.
 *
 * The engine writes NV12 in its own arrangement of memory, which no part of
 * Haiku knows how to draw, so a copy has to happen somewhere. This does it in
 * bands, so that the work can be split between processors.
 */
#ifndef NVDEC_CONVERT_H
#define NVDEC_CONVERT_H

#include "nvdec_h264.h"

/* The widest picture this will convert; a line of each plane is gathered on
 * the stack. */
#define NVDEC_MAX_WIDTH		8192

#ifdef __cplusplus
extern "C" {
#endif

/* Which way the samples were meant to be read. Standard definition and high
 * definition disagree, and a picture converted with the wrong one is visibly
 * wrong in the greens. */
typedef enum {
	NVDEC_RANGE_BT601,
	NVDEC_RANGE_BT709
} NvdecColorRange;

static inline NvdecColorRange
nvdecRangeForHeight(int height)
{
	return height > 576 ? NVDEC_RANGE_BT709 : NVDEC_RANGE_BT601;
}

/* Lines `fromLine` up to `toLine` of the picture, as 32 bit pixels: blue,
 * green, red, unused - which is what Haiku calls B_RGB32 on a little endian
 * machine. */
void nvdecFrameToRGB32(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	int fromLine, int toLine, NvdecColorRange range);

/* The same lines as B_YCbCr422: Cb Y0 Cr Y1, two pixels to four bytes. No
 * colour conversion happens, so this is both faster and exact. */
void nvdecFrameToYCbCr422(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	int fromLine, int toLine);

/* The same lines as P010: a plane of sixteen-bit luma samples with the value
 * in the top bits, then one of Cb and Cr samples in pairs, half as many
 * lines, both `outPitch` bytes a line. Ten-bit pictures are copied as they
 * are; eight-bit ones are widened. */
void nvdecFrameToP010(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	int fromLine, int toLine);

/* The same lines as NV12: a plane of eight-bit luma samples, then one of Cb
 * and Cr samples in pairs, half as many lines, both `outPitch` bytes a line.
 * What the engine wrote, rearranged; ten-bit pictures keep their top eight
 * bits. */
void nvdecFrameToNV12(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	int fromLine, int toLine);

/* Convert a whole picture, using as many threads as the machine has. */
void nvdecFrameToRGB32Threaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	NvdecColorRange range);
void nvdecFrameToYCbCr422Threaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch);
void nvdecFrameToP010Threaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch);
void nvdecFrameToNV12Threaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch);

#ifdef __cplusplus
}
#endif

#endif	/* NVDEC_CONVERT_H */
