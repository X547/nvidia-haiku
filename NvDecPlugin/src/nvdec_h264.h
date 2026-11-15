/* H.264 on the card's video decoder: whole streams, not single pictures.
 *
 * The engine decodes one picture at a time and knows nothing about what came
 * before it, so what is here is the bookkeeping around that - which pictures
 * are still needed as references, what order pictures are shown in, and which
 * of the decoder's surfaces each one lives in.
 */
#ifndef NVDEC_H264_H
#define NVDEC_H264_H

#include "nvdec_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NvdecH264 NvdecH264;

typedef struct {
	int		width;		/* what should be shown, after cropping */
	int		height;
	int		codedWidth;	/* what was decoded, a whole number of
					   macroblocks */
	int		codedHeight;
	int		cropLeft, cropTop;
	int		pitch;		/* bytes between lines of the planes */
	const uint8_t	*luma;		/* as the card arranges it; see
					   nvdecUntile */
	const uint8_t	*chroma;	/* two samples a pixel, Cb then Cr */
	int		bitDepth;	/* 8, or more in sixteen-bit samples
					   with the value in the top bits */
	int		pictureOrder;	/* display order within the stream */
	int64_t		time;		/* whatever was passed in with it */
	int		handle;		/* give this back with ReleaseFrame */
} NvdecFrame;

NvdecH264 *nvdecH264Create(NvdecEngine *engine, char *reason, size_t reasonSize);
void nvdecH264Destroy(NvdecH264 *decoder);

/* Decode one access unit: the NAL units of one picture, in Annex B form with
 * start codes, along with any parameter sets that came with it. */
bool nvdecH264Decode(NvdecH264 *decoder, const uint8_t *data, size_t size, int64_t time);

/* Take the next picture in display order, if one is ready. After
 * `nvdecH264DrainAll` every picture still held is ready. A picture stays
 * valid, and its surface is not decoded into again, until it is given back. */
bool nvdecH264NextFrame(NvdecH264 *decoder, NvdecFrame *frame);
void nvdecH264ReleaseFrame(NvdecH264 *decoder, const NvdecFrame *frame);
void nvdecH264DrainAll(NvdecH264 *decoder);

/* Forget everything, as after a seek. Nothing held is output. */
void nvdecH264Reset(NvdecH264 *decoder);

/* True once a sequence header has been seen and the picture size is known. */
bool nvdecH264HasFormat(const NvdecH264 *decoder, int *width, int *height);

const char *nvdecH264LastError(const NvdecH264 *decoder);
void nvdecH264GetStatus(const NvdecH264 *decoder, NvdecStatus *status);

#ifdef __cplusplus
}
#endif

#endif	/* NVDEC_H264_H */
