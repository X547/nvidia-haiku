/* As much of an H.265 stream as the card's decoder cannot work out for itself.
 *
 * As with H.264, the engine reads the slice data and most of each slice header
 * itself. What it is told is the sequence and picture parameter sets, which
 * pictures are references and where they sit in display order - all of which
 * comes from here - and how many bits of the slice header to step over: the
 * part that says which pictures are references, which the engine leaves to
 * the driver.
 */
#ifndef HEVC_PARSE_H
#define HEVC_PARSE_H

#include "h264_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HEVC_MAX_SPS		16
#define HEVC_MAX_PPS		64
#define HEVC_MAX_SHORT_TERM_SETS	65	/* 64 in the SPS, one in a slice */
#define HEVC_MAX_DELTA_POCS	16
#define HEVC_MAX_LONG_TERM_SPS	32
#define HEVC_MAX_LONG_TERM	33
#define HEVC_MAX_TILE_COLUMNS	20
#define HEVC_MAX_TILE_ROWS	22

/* NAL unit types, Table 7-1 */
enum {
	HEVC_NAL_TRAIL_N	= 0,
	HEVC_NAL_TRAIL_R	= 1,
	HEVC_NAL_RADL_N		= 6,
	HEVC_NAL_RADL_R		= 7,
	HEVC_NAL_RASL_N		= 8,
	HEVC_NAL_RASL_R		= 9,
	HEVC_NAL_BLA_W_LP	= 16,
	HEVC_NAL_BLA_W_RADL	= 17,
	HEVC_NAL_BLA_N_LP	= 18,
	HEVC_NAL_IDR_W_RADL	= 19,
	HEVC_NAL_IDR_N_LP	= 20,
	HEVC_NAL_CRA		= 21,
	HEVC_NAL_VPS		= 32,
	HEVC_NAL_SPS		= 33,
	HEVC_NAL_PPS		= 34,
	HEVC_NAL_AUD		= 35,
	HEVC_NAL_EOS		= 36,
	HEVC_NAL_EOB		= 37
};

static inline bool hevcIsVcl(int type) { return type < 32; }
static inline bool hevcIsIrap(int type) { return type >= 16 && type <= 23; }
static inline bool hevcIsIdr(int type)
	{ return type == HEVC_NAL_IDR_W_RADL || type == HEVC_NAL_IDR_N_LP; }
static inline bool hevcIsBla(int type) { return type >= 16 && type <= 18; }
static inline bool hevcIsRasl(int type)
	{ return type == HEVC_NAL_RASL_N || type == HEVC_NAL_RASL_R; }
static inline bool hevcIsRadl(int type)
	{ return type == HEVC_NAL_RADL_N || type == HEVC_NAL_RADL_R; }
/* A sub-layer non-reference picture: no picture of the same temporal layer
 * refers to it. */
static inline bool hevcIsSubLayerNonReference(int type)
	{ return type <= 14 && (type & 1) == 0; }

/* A short-term reference picture set, 7.3.7, as the deltas it works out to. */
typedef struct {
	int	numNegative;
	int	numPositive;
	int	numDeltaPocsOfRefRpsIdx; /* UVD needs the predicted set's source size */
	int	deltaPoc[2][HEVC_MAX_DELTA_POCS];	/* [0] before, [1] after */
	bool	used[2][HEVC_MAX_DELTA_POCS];
} HevcShortTermSet;

/* Scaling factors in raster order within each block, as the engine takes
 * them; the bitstream sends them in up-right diagonal order. */
typedef struct {
	uint8_t	list4x4[6][16];
	uint8_t	list8x8[6][64];
	uint8_t	list16x16[6][64];
	uint8_t	list32x32[6][64];
	uint8_t	dc16x16[6];
	uint8_t	dc32x32[6];
} HevcScalingList;

typedef struct {
	bool		valid;
	int		id;
	int		maxSubLayersMinus1;
	int		profileIdc, tierFlag, levelIdc;
	int		chromaFormatIdc;
	int		separateColourPlane;
	int		width, height;			/* pic_*_in_luma_samples */
	int		confLeft, confRight, confTop, confBottom;	/* in luma samples */
	int		bitDepthLuma, bitDepthChroma;
	int		log2MaxPocLsb;
	int		maxDecPicBuffering;		/* for the highest sub-layer */
	int		maxNumReorderPics;
	int		maxLatencyIncreasePlus1;
	int		log2MinCbSize, log2CtbSize;
	int		log2MinTbSize, log2MaxTbSize;
	int		maxTransformHierarchyDepthInter;
	int		maxTransformHierarchyDepthIntra;
	int		scalingListEnabled;
	HevcScalingList	scaling;			/* the SPS's, or the defaults */
	int		ampEnabled;
	int		saoEnabled;
	int		pcmEnabled;
	int		pcmBitDepthLuma, pcmBitDepthChroma;
	int		log2MinPcmCbSize, log2MaxPcmCbSize;
	int		pcmLoopFilterDisabled;
	int		numShortTermSets;
	HevcShortTermSet shortTerm[HEVC_MAX_SHORT_TERM_SETS];
	int		longTermRefsPresent;
	int		numLongTermRefsSps;
	int		longTermPocLsbSps[HEVC_MAX_LONG_TERM_SPS];
	bool		longTermUsedSps[HEVC_MAX_LONG_TERM_SPS];
	int		temporalMvpEnabled;
	int		strongIntraSmoothing;
	/* Colour, from the VUI; 2 (unspecified) when it says nothing */
	int		colourPrimaries, transferCharacteristics, matrixCoefficients;
	int		fullRange;
	int		fieldSeq;
	/* sps_range_extension */
	int		transformSkipRotation, transformSkipContext;
	int		implicitRdpcm, explicitRdpcm;
	int		extendedPrecision, intraSmoothingDisabled;
	int		highPrecisionOffsets, persistentRiceAdaptation;
	int		cabacBypassAlignment;
	/* Derived */
	int		ctbWidth, ctbHeight;		/* the picture in CTBs */
} HevcSps;

typedef struct {
	bool		valid;
	int		id;
	int		spsId;
	int		dependentSliceSegmentsEnabled;
	int		outputFlagPresent;
	int		numExtraSliceHeaderBits;
	int		signDataHiding;
	int		cabacInitPresent;
	int		numRefIdxL0DefaultActive, numRefIdxL1DefaultActive;
	int		initQpMinus26;
	int		constrainedIntraPred;
	int		transformSkipEnabled;
	int		cuQpDeltaEnabled;
	int		diffCuQpDeltaDepth;
	int		cbQpOffset, crQpOffset;
	int		sliceChromaQpOffsetsPresent;
	int		weightedPred, weightedBipred;
	int		transquantBypassEnabled;
	int		tilesEnabled;
	int		entropyCodingSync;
	int		numTileColumns, numTileRows;
	int		uniformSpacing;
	int		columnWidth[HEVC_MAX_TILE_COLUMNS];	/* in CTBs, explicit only */
	int		rowHeight[HEVC_MAX_TILE_ROWS];
	int		loopFilterAcrossTiles;
	int		loopFilterAcrossSlices;
	int		deblockingControlPresent;
	int		deblockingOverrideEnabled;
	int		deblockingDisabled;
	int		betaOffsetDiv2, tcOffsetDiv2;
	int		scalingListPresent;
	HevcScalingList	scaling;
	int		listsModificationPresent;
	int		log2ParallelMergeLevel;
	int		sliceHeaderExtensionPresent;
	/* pps_range_extension */
	int		log2MaxTransformSkipSizeMinus2;
	int		crossComponentPrediction;
	int		chromaQpOffsetListEnabled;
	int		diffCuChromaQpOffsetDepth;
	int		chromaQpOffsetListLen;
	int		cbQpOffsetList[6], crQpOffsetList[6];
	int		log2SaoOffsetScaleLuma, log2SaoOffsetScaleChroma;
} HevcPps;

typedef struct {
	HevcSps	sps[HEVC_MAX_SPS];
	HevcPps	pps[HEVC_MAX_PPS];
} HevcParamSets;

/* What the first slice segment of a picture says, up to the end of the part
 * the engine wants stepped over. */
typedef struct {
	int		nalType;
	int		temporalId;
	int		firstSliceInPicture;
	int		noOutputOfPriorPics;
	int		ppsId;
	int		dependentSliceSegment;
	int		sliceType;			/* 0 B, 1 P, 2 I */
	int		picOutputFlag;
	int		pocLsb;
	/* The short-term set in use: one of the SPS's, or the slice's own. */
	int		shortTermSetSpsFlag;
	int		shortTermSetIndex;
	HevcShortTermSet shortTerm;
	int		shortTermSetBits;		/* st_ref_pic_set() in the slice */
	/* Long-term entries, already resolved against the SPS */
	int		numLongTerm;
	int		longTermPocLsb[HEVC_MAX_LONG_TERM];
	bool		longTermUsed[HEVC_MAX_LONG_TERM];
	bool		longTermMsbPresent[HEVC_MAX_LONG_TERM];
	int		longTermMsbCycle[HEVC_MAX_LONG_TERM];	/* DeltaPocMsbCycleLt */
	/* Bits from just after slice_type to the end of the long-term
	 * references: what the engine is told to skip. */
	int		skipBits;
} HevcSlice;

bool hevcParseSps(const uint8_t *rbsp, size_t size, HevcSps *sps);
bool hevcParsePps(const uint8_t *rbsp, size_t size, const HevcParamSets *sets,
	HevcPps *pps);
/* `rbsp` starts at the two-byte NAL unit header. */
bool hevcParseSliceHeader(const uint8_t *rbsp, size_t size,
	const HevcParamSets *sets, HevcSlice *slice);

/* Column widths and row heights of the tiles, in CTBs, worked out for
 * uniform spacing (6-3, 6-4) or taken from the PPS. */
void hevcTileSizes(const HevcSps *sps, const HevcPps *pps, int *columnWidth,
	int *rowHeight);

/* The number of pictures the current one refers to: NumPicTotalCurr. */
int hevcNumPicTotalCurr(const HevcSlice *slice);

#ifdef __cplusplus
}
#endif

#endif	/* HEVC_PARSE_H */
