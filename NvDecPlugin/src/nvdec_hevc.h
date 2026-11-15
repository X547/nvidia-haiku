/* H.265 on the card's video decoder: whole streams, not single pictures.
 *
 * The same shape as nvdec_h264.h - the engine decodes a picture at a time,
 * and this keeps track of which pictures are references, which are still to
 * be shown, and which surface each one is in. Main and Main 10 are decoded;
 * ten-bit pictures come out as sixteen-bit samples with the value in the top
 * bits, as P010 has them.
 */
#ifndef NVDEC_HEVC_H
#define NVDEC_HEVC_H

#include "nvdec_h264.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NvdecHevc NvdecHevc;

NvdecHevc *nvdecHevcCreate(NvdecEngine *engine, char *reason, size_t reasonSize);
void nvdecHevcDestroy(NvdecHevc *decoder);

/* Decode one access unit: the NAL units of one picture in Annex B form, along
 * with any parameter sets that came with it. */
bool nvdecHevcDecode(NvdecHevc *decoder, const uint8_t *data, size_t size,
	int64_t time);

bool nvdecHevcNextFrame(NvdecHevc *decoder, NvdecFrame *frame);
void nvdecHevcReleaseFrame(NvdecHevc *decoder, const NvdecFrame *frame);
void nvdecHevcDrainAll(NvdecHevc *decoder);
void nvdecHevcReset(NvdecHevc *decoder);

bool nvdecHevcHasFormat(const NvdecHevc *decoder, int *width, int *height,
	int *bitDepth);

const char *nvdecHevcLastError(const NvdecHevc *decoder);
void nvdecHevcGetStatus(const NvdecHevc *decoder, NvdecStatus *status);

#ifdef __cplusplus
}
#endif

#endif	/* NVDEC_HEVC_H */
