/* VP9 on the card's video decoder: whole streams, not single frames.
 *
 * The engine decodes the tile data of one frame. Everything before it - the
 * uncompressed and the compressed header, the probabilities they update -
 * is read here (by sunxi_cedar's VP9 parser, vp9_parse.c), and so is the
 * backward adaptation of the probabilities after the frame, from the symbol
 * counts the engine leaves behind. What is kept here otherwise is which
 * surface each of the eight reference slots holds.
 *
 * Profile 0: eight-bit 4:2:0, from 65x65 to 4096x2304 on this generation.
 * The GTX 1070's GP104 does not decode ten bits (profile 2), and those
 * streams are refused; on a chip that does, NVDEC_VP9_TEN_BIT=1 lets them
 * through, to come out as sixteen-bit samples with the value in the top
 * bits, as from the H.265 decoder (not measured). There is no reordering:
 * frames are shown in the order they are decoded, and a chunk - one frame,
 * or a superframe of a hidden one and a shown one - shows at most one.
 */
#ifndef NVDEC_VP9_H
#define NVDEC_VP9_H

#include "nvdec_h264.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NvdecVp9 NvdecVp9;

NvdecVp9 *nvdecVp9Create(NvdecEngine *engine, char *reason, size_t reasonSize);
void nvdecVp9Destroy(NvdecVp9 *decoder);

/* Decode one chunk: a frame, or a superframe of several. A false return
 * with nvdecVp9Refused() true means the stream is one the engine does not
 * decode (profiles 1 to 3, too large or too small) and nothing of it
 * will. */
bool nvdecVp9Decode(NvdecVp9 *decoder, const uint8_t *data, size_t size,
	int64_t time);
bool nvdecVp9Refused(const NvdecVp9 *decoder);

bool nvdecVp9NextFrame(NvdecVp9 *decoder, NvdecFrame *frame);
void nvdecVp9ReleaseFrame(NvdecVp9 *decoder, const NvdecFrame *frame);
void nvdecVp9DrainAll(NvdecVp9 *decoder);
void nvdecVp9Reset(NvdecVp9 *decoder);

bool nvdecVp9HasFormat(const NvdecVp9 *decoder, int *width, int *height,
	int *bitDepth);

const char *nvdecVp9LastError(const NvdecVp9 *decoder);
void nvdecVp9GetStatus(const NvdecVp9 *decoder, NvdecStatus *status);

#ifdef __cplusplus
}
#endif

#endif	/* NVDEC_VP9_H */
