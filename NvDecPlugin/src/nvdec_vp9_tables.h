/* VP9's key frame probabilities (nvdec_vp9_tables.c, from libvpx under its
 * BSD licence): the engine is given them with every frame. The rest of VP9's
 * tables are the parser's, shared with sunxi_cedar (vp9_tables.c there). */
#ifndef NVDEC_VP9_TABLES_H
#define NVDEC_VP9_TABLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* By the mode above, the mode to the left; libvpx's order of the modes,
 * which is the engine's. */
extern const uint8_t kNvdecVp9KfYModeProbs[10][10][9];
/* By the luma mode. */
extern const uint8_t kNvdecVp9KfUvModeProbs[10][9];
/* By partition context: 8x8 blocks first. */
extern const uint8_t kNvdecVp9KfPartitionProbs[16][3];

#ifdef __cplusplus
}
#endif

#endif	/* NVDEC_VP9_TABLES_H */
