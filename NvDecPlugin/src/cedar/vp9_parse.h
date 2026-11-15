/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CEDAR_VP9_PARSE_H
#define CEDAR_VP9_PARSE_H

/*	VP9 parsing for a decoder whose hardware does the tile data: the
	uncompressed and the compressed frame header, the probability tables
	and their backward adaptation from the symbol counts the hardware
	gathers, the loop filter levels and dequantisers of the segments, and
	what a frame keeps for the next ones (the four saved probability
	contexts, loop filter deltas, segmentation, the eight reference slots).
	Section numbers: the VP9 Bitstream & Decoding Process Specification
	v0.6; where it and libvpx differ, libvpx is followed (the test vectors'
	hashes are libvpx's).

	The probability and count structures have the member order of libvpx's
	FRAME_CONTEXT and FRAME_COUNTS, so that the host check can compare with
	an instrumented libvpx byte for byte.

	Not decoded (reported as unsupported): profiles 1 and 3 (4:4:4, 4:2:2,
	4:4:0) and twelve bit frames. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VP9_REF_SLOTS		8
#define VP9_FRAME_CONTEXTS	4
#define VP9_MAX_SEGMENTS	8
#define VP9_MAX_FRAMES		8	/* in a superframe */

enum {
	VP9_KEY_FRAME = 0,
	VP9_INTER_FRAME = 1
};

/* the reference kinds; LAST..ALTREF are the frame's references 0..2 */
enum {
	VP9_INTRA = 0,
	VP9_LAST = 1,
	VP9_GOLDEN = 2,
	VP9_ALTREF = 3
};

/* interpolation filters, in libvpx's (and the engine's) numbering */
enum {
	VP9_EIGHTTAP = 0,
	VP9_EIGHTTAP_SMOOTH = 1,
	VP9_EIGHTTAP_SHARP = 2,
	VP9_BILINEAR = 3,
	VP9_SWITCHABLE = 4
};

enum {
	VP9_ONLY_4X4 = 0,
	VP9_ALLOW_8X8 = 1,
	VP9_ALLOW_16X16 = 2,
	VP9_ALLOW_32X32 = 3,
	VP9_TX_MODE_SELECT = 4
};

enum {
	VP9_SINGLE_REFERENCE = 0,
	VP9_COMPOUND_REFERENCE = 1,
	VP9_REFERENCE_MODE_SELECT = 2
};

/* segment features */
enum {
	VP9_SEG_ALT_Q = 0,
	VP9_SEG_ALT_LF = 1,
	VP9_SEG_REF_FRAME = 2,
	VP9_SEG_SKIP = 3
};

typedef struct {
	uint8_t	sign;
	uint8_t	classes[10];
	uint8_t	class0[1];
	uint8_t	bits[10];
	uint8_t	class0Fp[2][3];
	uint8_t	fp[3];
	uint8_t	class0Hp;
	uint8_t	hp;
} Vp9MvComponentProbs;

/* libvpx's FRAME_CONTEXT without "initialized" */
typedef struct {
	uint8_t	yMode[4][9];
	uint8_t	uvMode[10][9];
	uint8_t	partition[16][3];
	uint8_t	coef[4][2][2][6][6][3];	/* tx size, plane, ref, band, ctx */
	uint8_t	interp[4][2];
	uint8_t	interMode[7][3];
	uint8_t	intraInter[4];
	uint8_t	compInter[5];
	uint8_t	singleRef[5][2];
	uint8_t	compRef[5];
	uint8_t	tx32[2][3];
	uint8_t	tx16[2][2];
	uint8_t	tx8[2][1];
	uint8_t	skip[3];
	uint8_t	mvJoints[3];
	Vp9MvComponentProbs mv[2];		/* 0 rows (vertical), 1 columns */
} Vp9Probs;

typedef struct {
	uint32_t	sign[2];
	uint32_t	classes[11];
	uint32_t	class0[2];
	uint32_t	bits[10][2];
	uint32_t	class0Fp[2][4];
	uint32_t	fp[4];
	uint32_t	class0Hp[2];
	uint32_t	hp[2];
} Vp9MvComponentCounts;

/* libvpx's FRAME_COUNTS */
typedef struct {
	uint32_t	yMode[4][10];
	uint32_t	uvMode[10][10];
	uint32_t	partition[16][4];
	uint32_t	coef[4][2][2][6][6][4];	/* zero, one, more, EOB model */
	uint32_t	eobBranch[4][2][2][6][6];
	uint32_t	interp[4][3];
	uint32_t	interMode[7][4];	/* nearest, near, zero, new */
	uint32_t	intraInter[4][2];
	uint32_t	compInter[5][2];
	uint32_t	singleRef[5][2][2];
	uint32_t	compRef[5][2];
	uint32_t	tx32[2][4];
	uint32_t	tx16[2][3];
	uint32_t	tx8[2][2];
	uint32_t	txTotals[4];
	uint32_t	skip[3][2];
	uint32_t	mvJoints[4];
	Vp9MvComponentCounts mv[2];
} Vp9Counts;

typedef struct {
	int	enabled;
	int	updateMap;
	int	temporalUpdate;
	int	updateData;
	int	absDelta;
	uint8_t	treeProbs[7];
	uint8_t	predProbs[3];
	uint8_t	featureEnabled[VP9_MAX_SEGMENTS];	/* a bit per feature */
	int16_t	featureData[VP9_MAX_SEGMENTS][4];
} Vp9Segmentation;

/* What a reference slot holds, as far as the parser needs it. */
typedef struct {
	int	valid;
	int	width, height;
	int	bitDepth;
	int	subsamplingX, subsamplingY;
} Vp9Slot;

/* One frame's header, and what follows from it for the engine. */
typedef struct {
	int	profile;
	int	showExistingFrame;
	int	frameToShow;			/* slot */
	int	frameType;
	int	showFrame;
	int	errorResilient;
	int	intraOnly;
	int	resetFrameContext;
	int	bitDepth;
	int	colorSpace, colorRange;
	int	subsamplingX, subsamplingY;
	int	refreshFrameFlags;
	int	refSlot[3];				/* LAST, GOLDEN, ALTREF */
	int	signBias[4];			/* by reference kind */
	int	width, height;
	int	renderWidth, renderHeight;
	int	allowHighPrecisionMv;
	int	interpFilter;
	int	refreshFrameContext;
	int	frameParallel;
	int	frameContextIdx;		/* after setup_past_independence() */

	int	filterLevel, sharpness;
	int	modeRefDeltaEnabled;
	int	refDeltas[4], modeDeltas[2];	/* as in effect for this frame */

	int	baseQIdx, deltaQYDc, deltaQUvDc, deltaQUvAc;
	int	lossless;
	Vp9Segmentation seg;

	int	log2TileCols, log2TileRows;
	size_t	uncompressedHeaderSize;	/* bytes */
	size_t	compressedHeaderSize;

	/* the compressed header */
	int	txMode;
	int	referenceMode;
	int	compFixedRef, compVarRef[2];

	/* derived */
	int	miCols, miRows;
	int	usePrevFrameMvs;
	int	resetSegmentMap;		/* the map earlier frames left is void */
	int	refreshProbs;			/* backward adaptation after the frame */
	int	lastFrameWasKey;		/* (for the coefficient adaptation) */
	uint16_t	yDequant[VP9_MAX_SEGMENTS][2];	/* DC, AC */
	uint16_t	uvDequant[VP9_MAX_SEGMENTS][2];
	uint8_t	filterLevels[VP9_MAX_SEGMENTS][4][2];	/* ref kind, mode */

	Vp9Probs	probs;				/* to decode the frame with */
} Vp9Frame;

typedef struct {
	Vp9Probs	contexts[VP9_FRAME_CONTEXTS];
	int		contextsValid;
	Vp9Slot		slots[VP9_REF_SLOTS];
	/* what lasts from frame to frame */
	int		refDeltas[4], modeDeltas[2];
	Vp9Segmentation seg;
	int		bitDepth;
	int		subsamplingX, subsamplingY;
	int		colorSpace, colorRange;
	/* the last decoded frame (show_existing_frame is none) */
	int		haveLast;
	int		lastWidth, lastHeight;
	int		lastShowFrame;
	int		lastFrameType;
	int		lastIntraOnly;
	int		intraOnly;			/* as libvpx keeps it over key frames */
	int		frameType;
} Vp9State;

void vp9_init(Vp9State *state);
/* Forgets the references (a seek): decoding resumes at a key frame. */
void vp9_reset(Vp9State *state);

/* The frames of a chunk: a superframe's (Annex B), or the chunk. Returns
 * their number (0: none), sets offsets and sizes. */
int vp9_split_superframe(const uint8_t *data, size_t size,
	size_t offsets[VP9_MAX_FRAMES], size_t sizes[VP9_MAX_FRAMES]);

/* Parses a frame's headers into *frame and updates what the state keeps of
 * them (the saved contexts a reset puts back, loop filter deltas,
 * segmentation). Returns 0, -1 for a broken or out-of-order frame, -2 for
 * one the engine does not decode; error gets a reason. */
int vp9_parse_frame(Vp9State *state, const uint8_t *data, size_t size,
	Vp9Frame *frame, char *error);

/* After the engine decoded the frame: the backward adaptation from its
 * counts (NULL: the frame did not decode, the contexts stay), the saved
 * context, and the last-frame state. The reference slots are updated
 * too. */
void vp9_finish_frame(Vp9State *state, Vp9Frame *frame,
	const Vp9Counts *counts);

/* The adaptation alone (refresh_probs()): probs from the saved context
 * pre and the counts. */
void vp9_adapt_probs(Vp9Probs *probs, const Vp9Probs *pre,
	const Vp9Counts *counts, const Vp9Frame *frame);

/* Whether a chunk (its first frame) is a key frame. */
int vp9_is_key_frame(const uint8_t *data, size_t size);

/* The first column (row) of 8x8 blocks of a tile. */
int vp9_tile_offset(int tile, int mis, int log2Tiles);

extern const Vp9Probs kVp9DefaultProbs;

#ifdef __cplusplus
}
#endif

#endif	/* CEDAR_VP9_PARSE_H */
