/* As much of an H.264 stream as the card's decoder cannot work out for itself.
 *
 * The decoder parses slice headers and every entropy-coded bit in hardware, so
 * nothing here touches picture data. What it does read is the sequence and
 * picture parameter sets, and enough of each slice header to know which
 * picture a slice belongs to, where it sits in display order, and what it says
 * about which pictures are still needed as references.
 */
#ifndef H264_PARSE_H
#define H264_PARSE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H264_MAX_SPS		32
#define H264_MAX_PPS		256
#define H264_MAX_MMCO		32
#define H264_MAX_LIST_MODS	64

typedef struct {
	const uint8_t	*data;
	size_t		size;
	size_t		bytePos;
	int		bitPos;
	bool		failed; /* sticky: truncated or unrepresentable syntax */
} H264Bits;

void h264BitsInit(H264Bits *br, const uint8_t *data, size_t size);
uint32_t h264Bit(H264Bits *br);
uint32_t h264Bits(H264Bits *br, int count);
uint32_t h264UE(H264Bits *br);
int32_t h264SE(H264Bits *br);
bool h264MoreData(const H264Bits *br);

typedef struct {
	bool		valid;
	int		id;
	int		profileIdc, levelIdc;
	int		chromaFormatIdc;
	int		separateColourPlane;
	int		bitDepthLuma, bitDepthChroma;
	int		qpprimeYZeroTransformBypass;
	int		log2MaxFrameNumMinus4;
	int		picOrderCntType;
	int		log2MaxPocLsbMinus4;
	int		deltaPicOrderAlwaysZero;
	int		offsetForNonRefPic;
	int		offsetForTopToBottomField;
	int		numRefFramesInPocCycle;
	int		offsetForRefFrame[256];
	int		maxNumRefFrames;
	int		gapsInFrameNumAllowed;
	int		picWidthInMbs;
	int		picHeightInMapUnits;
	int		frameMbsOnly;
	int		mbAdaptiveFrameField;
	int		direct8x8Inference;
	int		cropLeft, cropRight, cropTop, cropBottom;
	bool		hasVui;
	int		fullRange;
	int		matrixCoefficients;	/* 2: unspecified */
	bool		hasReorderFrames;
	int		maxNumReorderFrames;
	int		maxDecFrameBuffering;
	uint8_t		scaling4x4[6][16];
	uint8_t		scaling8x8[2][64];
} H264Sps;

typedef struct {
	bool		valid;
	int		id;
	int		spsId;
	int		entropyCodingMode;
	int		picOrderPresent;
	int		numRefIdxL0Minus1, numRefIdxL1Minus1;
	int		weightedPred, weightedBipredIdc;
	int		picInitQpMinus26;
	int		chromaQpIndexOffset, secondChromaQpIndexOffset;
	int		deblockingFilterControlPresent;
	int		constrainedIntraPred;
	int		redundantPicCntPresent;
	int		transform8x8Mode;
	bool		scalingPresent;
	uint8_t		scaling4x4[6][16];
	uint8_t		scaling8x8[2][64];
} H264Pps;

typedef struct {
	int	idc;			/* modification_of_pic_nums_idc */
	int	value;			/* abs_diff_pic_num_minus1, or the
					   long term picture number */
} H264ListMod;

typedef struct {
	int	op;
	int	differenceOfPicNumsMinus1;
	int	longTermPicNum;
	int	longTermFrameIdx;
	int	maxLongTermFrameIdxPlus1;
} H264Mmco;

typedef struct {
	int		firstMbInSlice;
	int		sliceType;		/* 0 P, 1 B, 2 I, 3 SP, 4 SI */
	int		ppsId;
	int		frameNum;
	int		fieldPic, bottomField;
	int		idrPicId;
	int		pocLsb;
	int		deltaPocBottom;
	int		deltaPoc[2];
	int		numRefIdxL0Minus1, numRefIdxL1Minus1;
	int		nalRefIdc, nalType;
	bool		idr;
	/* Reference list modification, 7.3.3.1 */
	int		listModCount[2];
	H264ListMod	listMod[2][H264_MAX_LIST_MODS];
	/* Reference picture marking */
	int		noOutputOfPriorPics;
	int		longTermReference;
	int		adaptiveRefPicMarking;
	int		mmcoCount;
	H264Mmco	mmco[H264_MAX_MMCO];
} H264Slice;

typedef struct {
	H264Sps	sps[H264_MAX_SPS];
	H264Pps	pps[H264_MAX_PPS];
} H264ParamSets;

/* Each returns false if the unit is malformed or uses something unsupported. */
bool h264ParseSps(const uint8_t *rbsp, size_t size, H264Sps *sps);
bool h264ParsePps(const uint8_t *rbsp, size_t size, const H264ParamSets *sets, H264Pps *pps);
bool h264ParseSliceHeader(const uint8_t *rbsp, size_t size, const H264ParamSets *sets,
	int nalType, int nalRefIdc, H264Slice *slice);

/* Copy a NAL unit's payload with emulation prevention bytes removed. `out`
 * must have room for `size` bytes. Returns how many were written. */
size_t h264ToRbsp(const uint8_t *in, size_t size, uint8_t *out);

/* How many frames this level allows the decoder to hold, which is how far
 * pictures may be reordered before one has to be shown. */
int h264MaxDpbFrames(const H264Sps *sps);

#ifdef __cplusplus
}
#endif

#endif	/* H264_PARSE_H */
