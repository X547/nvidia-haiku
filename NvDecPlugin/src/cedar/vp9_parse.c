/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "vp9_parse.h"

#include <stdio.h>
#include <string.h>

#include "bits.h"
#include "vp9_tables.h"


#define FAIL(code, ...) do { \
		if (error != NULL) \
			snprintf(error, 160, __VA_ARGS__); \
		return code; \
	} while (0)


static inline int
clamp_int(int value, int low, int high)
{
	return value < low ? low : value > high ? high : value;
}


//	#pragma mark - the boolean decoder (9.2)


typedef struct {
	const uint8_t	*data;
	size_t		size;
	size_t		position;	/* bits read */
	uint32_t	value;
	uint32_t	range;
} BoolDecoder;


static int
bd_bit(BoolDecoder *bd)
{
	size_t byte = bd->position >> 3;
	int bit = 0;
	if (byte < bd->size)
		bit = (bd->data[byte] >> (7 - (bd->position & 7))) & 1;
	bd->position++;
	return bit;
}


static int
bd_read(BoolDecoder *bd, int probability)
{
	uint32_t split = 1 + (((bd->range - 1) * (uint32_t)probability) >> 8);
	int bit;
	if (bd->value < split << 24) {
		bd->range = split;
		bit = 0;
	} else {
		bd->range -= split;
		bd->value -= split << 24;
		bit = 1;
	}
	while (bd->range < 128) {
		bd->value = (bd->value << 1) | ((uint32_t)bd_bit(bd) << 24);
		bd->range <<= 1;
	}
	return bit;
}


/*!	The value is kept in bits 31-24 (and below the bits not yet shifted
	in): bd_init() reads the first byte, and every renormalisation one more
	bit. */
static int
bd_init(BoolDecoder *bd, const uint8_t *data, size_t size)
{
	bd->data = data;
	bd->size = size;
	bd->position = 0;
	bd->range = 255;
	bd->value = 0;
	for (int i = 0; i < 8; i++)
		bd->value |= (uint32_t)bd_bit(bd) << (31 - i);
	/* the marker bit */
	return bd_read(bd, 128) == 0 ? 0 : -1;
}


static int
bd_literal(BoolDecoder *bd, int bits)
{
	int value = 0;
	for (int i = 0; i < bits; i++)
		value = (value << 1) | bd_read(bd, 128);
	return value;
}


/*	diff_update_prob() (6.3.5): inv_remap_prob() of decode_term_subexp() */


static int
inv_recenter_nonneg(int v, int m)
{
	if (v > 2 * m)
		return v;
	return (v & 1) != 0 ? m - ((v + 1) >> 1) : m + (v >> 1);
}


static int
decode_term_subexp(BoolDecoder *bd)
{
	if (!bd_literal(bd, 1))
		return bd_literal(bd, 4);
	if (!bd_literal(bd, 1))
		return bd_literal(bd, 4) + 16;
	if (!bd_literal(bd, 1))
		return bd_literal(bd, 5) + 32;
	int v = bd_literal(bd, 7);
	if (v < 65)
		return v + 64;
	return (v << 1) - 1 + bd_literal(bd, 1);
}


static void
diff_update_prob(BoolDecoder *bd, uint8_t *probability)
{
	if (!bd_read(bd, 252))
		return;
	int delta = decode_term_subexp(bd);
	if (delta > 253)
		delta = 253;
	int v = kVp9InverseMap[delta];
	int m = *probability - 1;
	if ((m << 1) <= 255)
		*probability = (uint8_t)(1 + inv_recenter_nonneg(v, m));
	else
		*probability = (uint8_t)(255 - inv_recenter_nonneg(v, 254 - m));
}


static void
update_mv_prob(BoolDecoder *bd, uint8_t *probability, int count)
{
	for (int i = 0; i < count; i++) {
		if (bd_read(bd, 252))
			probability[i] = (uint8_t)((bd_literal(bd, 7) << 1) | 1);
	}
}


//	#pragma mark - state


static void
default_lf_deltas(int refDeltas[4], int modeDeltas[2])
{
	refDeltas[VP9_INTRA] = 1;
	refDeltas[VP9_LAST] = 0;
	refDeltas[VP9_GOLDEN] = -1;
	refDeltas[VP9_ALTREF] = -1;
	modeDeltas[0] = modeDeltas[1] = 0;
}


void
vp9_init(Vp9State *state)
{
	memset(state, 0, sizeof(*state));
	default_lf_deltas(state->refDeltas, state->modeDeltas);
	state->bitDepth = 8;
	state->subsamplingX = state->subsamplingY = 1;
}


void
vp9_reset(Vp9State *state)
{
	for (int i = 0; i < VP9_REF_SLOTS; i++)
		state->slots[i].valid = 0;
	state->haveLast = 0;
}


int
vp9_split_superframe(const uint8_t *data, size_t size,
	size_t offsets[VP9_MAX_FRAMES], size_t sizes[VP9_MAX_FRAMES])
{
	if (size == 0)
		return 0;
	uint8_t marker = data[size - 1];
	if ((marker & 0xe0) == 0xc0) {
		int frames = (marker & 7) + 1;
		int bytes = ((marker >> 3) & 3) + 1;
		size_t indexSize = 2 + (size_t)bytes * frames;
		if (size >= indexSize && data[size - indexSize] == marker) {
			const uint8_t *index = data + size - indexSize + 1;
			size_t offset = 0;
			int count = 0;
			for (int i = 0; i < frames; i++) {
				size_t frameSize = 0;
				for (int b = 0; b < bytes; b++)
					frameSize |= (size_t)index[i * bytes + b] << (8 * b);
				if (offset + frameSize > size - indexSize)
					break;
				/* a frame of nothing is skipped (libvpx does) */
				if (frameSize > 0) {
					offsets[count] = offset;
					sizes[count] = frameSize;
					count++;
				}
				offset += frameSize;
			}
			return count;
		}
	}
	offsets[0] = 0;
	sizes[0] = size;
	return 1;
}


int
vp9_is_key_frame(const uint8_t *data, size_t size)
{
	size_t offsets[VP9_MAX_FRAMES], sizes[VP9_MAX_FRAMES];
	if (vp9_split_superframe(data, size, offsets, sizes) < 1)
		return 0;
	BitReader br;
	br_init(&br, data + offsets[0], sizes[0]);
	if (br_u(&br, 2) != 2)
		return 0;
	int profile = br_flag(&br);
	profile |= br_flag(&br) << 1;
	if (profile == 3)
		br_u(&br, 1);
	if (br_flag(&br))		/* show_existing_frame */
		return 0;
	return br_flag(&br) == VP9_KEY_FRAME && !br.overrun;
}


int
vp9_tile_offset(int tile, int mis, int log2Tiles)
{
	int sb64s = (mis + 7) >> 3;
	int offset = ((tile * sb64s) >> log2Tiles) << 3;
	return offset < mis ? offset : mis;
}


//	#pragma mark - the uncompressed header (6.2)


static int
read_signed(BitReader *br, int bits)
{
	int value = (int)br_u(br, bits);
	return br_flag(br) ? -value : value;
}


static int
read_color_config(Vp9State *state, Vp9Frame *frame, BitReader *br,
	char *error)
{
	if (frame->profile >= 2)
		frame->bitDepth = br_flag(br) ? 12 : 10;
	else
		frame->bitDepth = 8;
	frame->colorSpace = (int)br_u(br, 3);
	if (frame->colorSpace != 7) {	/* not sRGB */
		frame->colorRange = br_flag(br);
		if (frame->profile == 1 || frame->profile == 3) {
			frame->subsamplingX = br_flag(br);
			frame->subsamplingY = br_flag(br);
			br_u(br, 1);
		} else
			frame->subsamplingX = frame->subsamplingY = 1;
	} else {
		frame->colorRange = 1;
		if (frame->profile == 1 || frame->profile == 3) {
			frame->subsamplingX = frame->subsamplingY = 0;
			br_u(br, 1);
		} else
			FAIL(-1, "4:4:4 (sRGB) in profile %d", frame->profile);
	}
	state->bitDepth = frame->bitDepth;
	state->subsamplingX = frame->subsamplingX;
	state->subsamplingY = frame->subsamplingY;
	state->colorSpace = frame->colorSpace;
	state->colorRange = frame->colorRange;
	return 0;
}


static void
read_frame_size(Vp9Frame *frame, BitReader *br)
{
	frame->width = (int)br_u(br, 16) + 1;
	frame->height = (int)br_u(br, 16) + 1;
}


static void
read_render_size(Vp9Frame *frame, BitReader *br)
{
	frame->renderWidth = frame->width;
	frame->renderHeight = frame->height;
	if (br_flag(br)) {
		frame->renderWidth = (int)br_u(br, 16) + 1;
		frame->renderHeight = (int)br_u(br, 16) + 1;
	}
}


/*!	setup_past_independence() (7.2) */
static void
setup_past_independence(Vp9State *state, Vp9Frame *frame)
{
	memset(state->seg.featureEnabled, 0, sizeof(state->seg.featureEnabled));
	memset(state->seg.featureData, 0, sizeof(state->seg.featureData));
	state->seg.absDelta = 0;
	default_lf_deltas(state->refDeltas, state->modeDeltas);

	if (frame->frameType == VP9_KEY_FRAME || frame->errorResilient
		|| frame->resetFrameContext == 3) {
		for (int i = 0; i < VP9_FRAME_CONTEXTS; i++)
			state->contexts[i] = kVp9DefaultProbs;
		state->contextsValid = (1 << VP9_FRAME_CONTEXTS) - 1;
	} else if (frame->resetFrameContext == 2) {
		state->contexts[frame->frameContextIdx] = kVp9DefaultProbs;
		state->contextsValid |= 1 << frame->frameContextIdx;
	}
	memset(frame->signBias, 0, sizeof(frame->signBias));
	frame->frameContextIdx = 0;
}


static void
read_segmentation(Vp9Segmentation *seg, BitReader *br)
{
	static const int kBits[4] = { 8, 6, 2, 0 };
	static const int kMax[4] = { 255, 63, 3, 0 };

	seg->updateMap = 0;
	seg->updateData = 0;
	seg->enabled = br_flag(br);
	if (!seg->enabled)
		return;
	seg->updateMap = br_flag(br);
	if (seg->updateMap) {
		for (int i = 0; i < 7; i++)
			seg->treeProbs[i] = br_flag(br) ? (uint8_t)br_u(br, 8) : 255;
		seg->temporalUpdate = br_flag(br);
		for (int i = 0; i < 3; i++) {
			seg->predProbs[i] = 255;
			if (seg->temporalUpdate && br_flag(br))
				seg->predProbs[i] = (uint8_t)br_u(br, 8);
		}
	}
	seg->updateData = br_flag(br);
	if (seg->updateData) {
		seg->absDelta = br_flag(br);
		memset(seg->featureEnabled, 0, sizeof(seg->featureEnabled));
		memset(seg->featureData, 0, sizeof(seg->featureData));
		for (int i = 0; i < VP9_MAX_SEGMENTS; i++) {
			for (int j = 0; j < 4; j++) {
				int data = 0;
				if (br_flag(br)) {
					seg->featureEnabled[i] |= 1 << j;
					data = kBits[j] > 0 ? (int)br_u(br, kBits[j]) : 0;
					if (data > kMax[j])
						data = kMax[j];
					if (j < 2 && br_flag(br))
						data = -data;
				}
				seg->featureData[i][j] = (int16_t)data;
			}
		}
	}
}


static inline int
seg_feature_active(const Vp9Segmentation *seg, int segment, int feature)
{
	return seg->enabled && (seg->featureEnabled[segment] & (1 << feature));
}


static int
parse_uncompressed(Vp9State *state, Vp9Frame *frame, BitReader *br,
	char *error)
{
	state->lastFrameType = state->frameType;
	state->lastIntraOnly = state->intraOnly;

	if (br_u(br, 2) != 2)
		FAIL(-1, "no frame marker");
	frame->profile = br_flag(br);
	frame->profile |= br_flag(br) << 1;
	if (frame->profile == 3)
		frame->profile += br_flag(br);
	if (frame->profile > 3)
		FAIL(-1, "profile %d", frame->profile);

	frame->showExistingFrame = br_flag(br);
	if (frame->showExistingFrame) {
		frame->frameToShow = (int)br_u(br, 3);
		if (!state->slots[frame->frameToShow].valid)
			FAIL(-1, "slot %d to show is empty", frame->frameToShow);
		frame->showFrame = 1;
		frame->refreshFrameFlags = 0;
		frame->filterLevel = 0;
		return 0;
	}

	frame->frameType = br_flag(br);
	frame->showFrame = br_flag(br);
	frame->errorResilient = br_flag(br);
	state->frameType = frame->frameType;
	frame->bitDepth = state->bitDepth;
	frame->subsamplingX = state->subsamplingX;
	frame->subsamplingY = state->subsamplingY;
	frame->colorSpace = state->colorSpace;
	frame->colorRange = state->colorRange;
	frame->intraOnly = 0;
	frame->resetFrameContext = 0;

	if (frame->frameType == VP9_KEY_FRAME) {
		if (br_u(br, 24) != 0x498342)
			FAIL(-1, "no sync code");
		if (read_color_config(state, frame, br, error) != 0)
			return -1;
		frame->refreshFrameFlags = 0xff;
		read_frame_size(frame, br);
		read_render_size(frame, br);
		/* a key frame makes up for everything lost before */
		for (int i = 0; i < VP9_REF_SLOTS; i++)
			state->slots[i].valid = 0;
	} else {
		frame->intraOnly = frame->showFrame ? 0 : br_flag(br);
		state->intraOnly = frame->intraOnly;
		frame->resetFrameContext = frame->errorResilient ? 0
			: (int)br_u(br, 2);
		if (frame->intraOnly) {
			if (br_u(br, 24) != 0x498342)
				FAIL(-1, "no sync code");
			if (frame->profile > 0) {
				if (read_color_config(state, frame, br, error) != 0)
					return -1;
			} else {
				frame->bitDepth = 8;
				frame->colorSpace = 2;	/* BT.601 */
				frame->colorRange = 0;
				frame->subsamplingX = frame->subsamplingY = 1;
				state->bitDepth = 8;
				state->subsamplingX = state->subsamplingY = 1;
				state->colorSpace = 2;
				state->colorRange = 0;
			}
			frame->refreshFrameFlags = (int)br_u(br, 8);
			read_frame_size(frame, br);
			read_render_size(frame, br);
		} else {
			frame->refreshFrameFlags = (int)br_u(br, 8);
			for (int i = 0; i < 3; i++) {
				frame->refSlot[i] = (int)br_u(br, 3);
				frame->signBias[VP9_LAST + i] = br_flag(br);
				if (!state->slots[frame->refSlot[i]].valid)
					FAIL(-1, "reference %d (slot %d) is missing", i,
						frame->refSlot[i]);
			}
			int found = 0;
			for (int i = 0; i < 3 && !found; i++) {
				if (br_flag(br)) {
					const Vp9Slot *slot = &state->slots[frame->refSlot[i]];
					frame->width = slot->width;
					frame->height = slot->height;
					found = 1;
				}
			}
			if (!found)
				read_frame_size(frame, br);
			read_render_size(frame, br);
			for (int i = 0; i < 3; i++) {
				const Vp9Slot *slot = &state->slots[frame->refSlot[i]];
				if (slot->bitDepth != frame->bitDepth
					|| slot->subsamplingX != frame->subsamplingX
					|| slot->subsamplingY != frame->subsamplingY) {
					FAIL(-1, "reference %d has another format", i);
				}
			}
			frame->allowHighPrecisionMv = br_flag(br);
			if (br_flag(br))
				frame->interpFilter = VP9_SWITCHABLE;
			else {
				static const int kFilters[4] = { VP9_EIGHTTAP_SMOOTH,
					VP9_EIGHTTAP, VP9_EIGHTTAP_SHARP, VP9_BILINEAR };
				frame->interpFilter = kFilters[br_u(br, 2)];
			}
		}
	}

	if (!frame->errorResilient) {
		frame->refreshFrameContext = br_flag(br);
		frame->frameParallel = br_flag(br);
	} else {
		frame->refreshFrameContext = 0;
		frame->frameParallel = 1;
	}
	frame->frameContextIdx = (int)br_u(br, 2);

	if (frame->frameType == VP9_KEY_FRAME || frame->intraOnly
		|| frame->errorResilient) {
		setup_past_independence(state, frame);
	}

	/* loop_filter_params() */
	frame->filterLevel = (int)br_u(br, 6);
	frame->sharpness = (int)br_u(br, 3);
	frame->modeRefDeltaEnabled = br_flag(br);
	if (frame->modeRefDeltaEnabled && br_flag(br)) {
		for (int i = 0; i < 4; i++) {
			if (br_flag(br))
				state->refDeltas[i] = read_signed(br, 6);
		}
		for (int i = 0; i < 2; i++) {
			if (br_flag(br))
				state->modeDeltas[i] = read_signed(br, 6);
		}
	}
	memcpy(frame->refDeltas, state->refDeltas, sizeof(frame->refDeltas));
	memcpy(frame->modeDeltas, state->modeDeltas, sizeof(frame->modeDeltas));

	/* quantization_params() */
	frame->baseQIdx = (int)br_u(br, 8);
	frame->deltaQYDc = br_flag(br) ? read_signed(br, 4) : 0;
	frame->deltaQUvDc = br_flag(br) ? read_signed(br, 4) : 0;
	frame->deltaQUvAc = br_flag(br) ? read_signed(br, 4) : 0;
	frame->lossless = frame->baseQIdx == 0 && frame->deltaQYDc == 0
		&& frame->deltaQUvDc == 0 && frame->deltaQUvAc == 0;

	read_segmentation(&state->seg, br);
	frame->seg = state->seg;

	/* tile_info() */
	frame->miCols = (frame->width + 7) >> 3;
	frame->miRows = (frame->height + 7) >> 3;
	int sb64Cols = (frame->miCols + 7) >> 3;
	int minLog2 = 0;
	while ((64 << minLog2) < sb64Cols)
		minLog2++;
	int maxLog2 = 1;
	while ((sb64Cols >> maxLog2) >= 4)
		maxLog2++;
	maxLog2--;
	frame->log2TileCols = minLog2;
	while (frame->log2TileCols < maxLog2 && br_flag(br))
		frame->log2TileCols++;
	frame->log2TileRows = br_flag(br);
	if (frame->log2TileRows)
		frame->log2TileRows += br_flag(br);

	frame->compressedHeaderSize = br_u(br, 16);
	if (br->overrun)
		FAIL(-1, "the uncompressed header is cut off");
	frame->uncompressedHeaderSize = (br_pos(br) + 7) / 8;
	if (frame->compressedHeaderSize == 0)
		FAIL(-1, "no compressed header");
	return 0;
}


//	#pragma mark - the compressed header (6.3)


static void
read_coef_probs(Vp9Frame *frame, BoolDecoder *bd)
{
	static const int kMaxTxSize[5] = { 0, 1, 2, 3, 3 };
	for (int tx = 0; tx <= kMaxTxSize[frame->txMode]; tx++) {
		if (!bd_literal(bd, 1))
			continue;
		for (int i = 0; i < 2; i++) {
			for (int j = 0; j < 2; j++) {
				for (int k = 0; k < 6; k++) {
					for (int l = 0; l < (k == 0 ? 3 : 6); l++) {
						for (int m = 0; m < 3; m++)
							diff_update_prob(bd, &frame->probs.coef[tx][i][j][k][l][m]);
					}
				}
			}
		}
	}
}


static int
parse_compressed(Vp9Frame *frame, const uint8_t *data, size_t size,
	char *error)
{
	Vp9Probs *p = &frame->probs;
	BoolDecoder bd;
	if (bd_init(&bd, data, size) != 0)
		FAIL(-1, "the compressed header's marker bit is set");

	if (frame->lossless)
		frame->txMode = VP9_ONLY_4X4;
	else {
		frame->txMode = bd_literal(&bd, 2);
		if (frame->txMode == VP9_ALLOW_32X32)
			frame->txMode += bd_literal(&bd, 1);
	}
	if (frame->txMode == VP9_TX_MODE_SELECT) {
		for (int i = 0; i < 2; i++)
			diff_update_prob(&bd, &p->tx8[i][0]);
		for (int i = 0; i < 2; i++) {
			for (int j = 0; j < 2; j++)
				diff_update_prob(&bd, &p->tx16[i][j]);
		}
		for (int i = 0; i < 2; i++) {
			for (int j = 0; j < 3; j++)
				diff_update_prob(&bd, &p->tx32[i][j]);
		}
	}
	read_coef_probs(frame, &bd);
	for (int i = 0; i < 3; i++)
		diff_update_prob(&bd, &p->skip[i]);

	frame->referenceMode = VP9_SINGLE_REFERENCE;
	if (frame->frameType != VP9_KEY_FRAME && !frame->intraOnly) {
		for (int i = 0; i < 7; i++) {
			for (int j = 0; j < 3; j++)
				diff_update_prob(&bd, &p->interMode[i][j]);
		}
		if (frame->interpFilter == VP9_SWITCHABLE) {
			for (int i = 0; i < 4; i++) {
				for (int j = 0; j < 2; j++)
					diff_update_prob(&bd, &p->interp[i][j]);
			}
		}
		for (int i = 0; i < 4; i++)
			diff_update_prob(&bd, &p->intraInter[i]);

		/* frame_reference_mode() */
		int compoundAllowed = frame->signBias[VP9_GOLDEN]
				!= frame->signBias[VP9_LAST]
			|| frame->signBias[VP9_ALTREF] != frame->signBias[VP9_LAST];
		if (compoundAllowed && bd_literal(&bd, 1)) {
			frame->referenceMode = bd_literal(&bd, 1)
				? VP9_REFERENCE_MODE_SELECT : VP9_COMPOUND_REFERENCE;
		}
		if (frame->referenceMode != VP9_SINGLE_REFERENCE) {
			if (frame->signBias[VP9_LAST] == frame->signBias[VP9_GOLDEN]) {
				frame->compFixedRef = VP9_ALTREF;
				frame->compVarRef[0] = VP9_LAST;
				frame->compVarRef[1] = VP9_GOLDEN;
			} else if (frame->signBias[VP9_LAST]
					== frame->signBias[VP9_ALTREF]) {
				frame->compFixedRef = VP9_GOLDEN;
				frame->compVarRef[0] = VP9_LAST;
				frame->compVarRef[1] = VP9_ALTREF;
			} else {
				frame->compFixedRef = VP9_LAST;
				frame->compVarRef[0] = VP9_GOLDEN;
				frame->compVarRef[1] = VP9_ALTREF;
			}
		}
		if (frame->referenceMode == VP9_REFERENCE_MODE_SELECT) {
			for (int i = 0; i < 5; i++)
				diff_update_prob(&bd, &p->compInter[i]);
		}
		if (frame->referenceMode != VP9_COMPOUND_REFERENCE) {
			for (int i = 0; i < 5; i++) {
				diff_update_prob(&bd, &p->singleRef[i][0]);
				diff_update_prob(&bd, &p->singleRef[i][1]);
			}
		}
		if (frame->referenceMode != VP9_SINGLE_REFERENCE) {
			for (int i = 0; i < 5; i++)
				diff_update_prob(&bd, &p->compRef[i]);
		}

		for (int i = 0; i < 4; i++) {
			for (int j = 0; j < 9; j++)
				diff_update_prob(&bd, &p->yMode[i][j]);
		}
		for (int i = 0; i < 16; i++) {
			for (int j = 0; j < 3; j++)
				diff_update_prob(&bd, &p->partition[i][j]);
		}

		/* mv_probs() */
		update_mv_prob(&bd, p->mvJoints, 3);
		for (int i = 0; i < 2; i++) {
			Vp9MvComponentProbs *c = &p->mv[i];
			update_mv_prob(&bd, &c->sign, 1);
			update_mv_prob(&bd, c->classes, 10);
			update_mv_prob(&bd, c->class0, 1);
			update_mv_prob(&bd, c->bits, 10);
		}
		for (int i = 0; i < 2; i++) {
			Vp9MvComponentProbs *c = &p->mv[i];
			for (int j = 0; j < 2; j++)
				update_mv_prob(&bd, c->class0Fp[j], 3);
			update_mv_prob(&bd, c->fp, 3);
		}
		if (frame->allowHighPrecisionMv) {
			for (int i = 0; i < 2; i++) {
				update_mv_prob(&bd, &p->mv[i].class0Hp, 1);
				update_mv_prob(&bd, &p->mv[i].hp, 1);
			}
		}
	}
	return 0;
}


//	#pragma mark - per segment values


static void
derive_segment_values(Vp9Frame *frame)
{
	const uint16_t *dc = frame->bitDepth > 8 ? kVp9DcQ10 : kVp9DcQ8;
	const uint16_t *ac = frame->bitDepth > 8 ? kVp9AcQ10 : kVp9AcQ8;
	const Vp9Segmentation *seg = &frame->seg;

	memset(frame->yDequant, 0, sizeof(frame->yDequant));
	memset(frame->uvDequant, 0, sizeof(frame->uvDequant));
	int segments = seg->enabled ? VP9_MAX_SEGMENTS : 1;
	for (int i = 0; i < segments; i++) {
		int q = frame->baseQIdx;
		if (seg_feature_active(seg, i, VP9_SEG_ALT_Q)) {
			int data = seg->featureData[i][VP9_SEG_ALT_Q];
			q = clamp_int(seg->absDelta ? data : q + data, 0, 255);
		}
		frame->yDequant[i][0] = dc[clamp_int(q + frame->deltaQYDc, 0, 255)];
		frame->yDequant[i][1] = ac[q];
		frame->uvDequant[i][0] = dc[clamp_int(q + frame->deltaQUvDc, 0, 255)];
		frame->uvDequant[i][1] = ac[clamp_int(q + frame->deltaQUvAc, 0, 255)];
	}

	/* 8.8.1, as libvpx's vp9_loop_filter_frame_init(): the deltas'
	   scale follows the frame's level, not the segment's */
	memset(frame->filterLevels, 0, sizeof(frame->filterLevels));
	if (frame->filterLevel == 0)
		return;
	int scale = 1 << (frame->filterLevel >> 5);
	for (int i = 0; i < VP9_MAX_SEGMENTS; i++) {
		int level = frame->filterLevel;
		if (seg_feature_active(seg, i, VP9_SEG_ALT_LF)) {
			int data = seg->featureData[i][VP9_SEG_ALT_LF];
			level = clamp_int(seg->absDelta ? data : level + data, 0, 63);
		}
		if (!frame->modeRefDeltaEnabled) {
			memset(frame->filterLevels[i], level, sizeof(frame->filterLevels[i]));
			continue;
		}
		int intra = clamp_int(level + frame->refDeltas[VP9_INTRA] * scale, 0,
			63);
		frame->filterLevels[i][VP9_INTRA][0] = (uint8_t)intra;
		frame->filterLevels[i][VP9_INTRA][1] = (uint8_t)intra;
		for (int ref = VP9_LAST; ref <= VP9_ALTREF; ref++) {
			for (int mode = 0; mode < 2; mode++) {
				frame->filterLevels[i][ref][mode] = (uint8_t)clamp_int(level
					+ frame->refDeltas[ref] * scale
					+ frame->modeDeltas[mode] * scale, 0, 63);
			}
		}
	}
}


int
vp9_parse_frame(Vp9State *state, const uint8_t *data, size_t size,
	Vp9Frame *frame, char *error)
{
	memset(frame, 0, sizeof(*frame));
	BitReader br;
	br_init(&br, data, size);
	int result = parse_uncompressed(state, frame, &br, error);
	if (result != 0 || frame->showExistingFrame)
		return result;

	if (frame->profile == 1 || frame->profile == 3
		|| frame->subsamplingX != 1 || frame->subsamplingY != 1)
		FAIL(-2, "the engine decodes 4:2:0 only (profile %d)", frame->profile);
	if (frame->bitDepth > 10)
		FAIL(-2, "the engine decodes up to ten bits, this is %d",
			frame->bitDepth);

	if (frame->uncompressedHeaderSize + frame->compressedHeaderSize > size)
		FAIL(-1, "the frame is shorter than its headers");

	if ((state->contextsValid & (1 << frame->frameContextIdx)) == 0)
		FAIL(-1, "probability context %d was never set up",
			frame->frameContextIdx);
	frame->probs = state->contexts[frame->frameContextIdx];
	result = parse_compressed(frame, data + frame->uncompressedHeaderSize,
		frame->compressedHeaderSize, error);
	if (result != 0)
		return result;

	int intra = frame->frameType == VP9_KEY_FRAME || frame->intraOnly;
	frame->usePrevFrameMvs = !intra && state->haveLast
		&& !frame->errorResilient
		&& frame->width == state->lastWidth
		&& frame->height == state->lastHeight
		&& !state->lastIntraOnly && state->lastShowFrame
		&& state->lastFrameType != VP9_KEY_FRAME;
	frame->resetSegmentMap = intra || frame->errorResilient
		|| !state->haveLast || frame->width != state->lastWidth
		|| frame->height != state->lastHeight;
	frame->refreshProbs = !frame->errorResilient && !frame->frameParallel;
	frame->lastFrameWasKey = state->lastFrameType == VP9_KEY_FRAME;
	derive_segment_values(frame);
	return 0;
}


//	#pragma mark - backward adaptation (8.4)


static const int8_t kIntraModeTree[18] = {
	-0, 2, -9, 4, -1, 6, 8, 12, -2, 10, -4, -5, -3, 14, -8, 16, -6, -7
};
static const int8_t kInterModeTree[6] = { -2, 2, -0, 4, -1, -3 };
static const int8_t kPartitionTree[6] = { -0, 2, -1, 4, -2, -3 };
static const int8_t kInterpTree[4] = { -0, 2, -1, -2 };
static const int8_t kMvJointTree[6] = { -0, 2, -1, 4, -2, -3 };
static const int8_t kMvClassTree[20] = {
	-0, 2, -1, 4, 6, 8, -2, -3, 10, 12, -4, -5, -6, 14, 16, 18, -7, -8, -9,
	-10
};
static const int8_t kMvClass0Tree[2] = { -0, -1 };
static const int8_t kMvFpTree[6] = { -0, 2, -1, 4, -2, -3 };

static const uint8_t kCountToFactor[21] = {
	0, 6, 12, 19, 25, 32, 38, 44, 51, 57, 64, 70, 76, 83, 89, 96, 102, 108,
	115, 121, 128
};


static uint8_t
probability_of(uint32_t zeros, uint32_t total)
{
	int p = (int)(((uint64_t)zeros * 256 + (total >> 1)) / total);
	return (uint8_t)(p < 1 ? 1 : p > 255 ? 255 : p);
}


static uint8_t
merge_probability(uint8_t pre, uint32_t zeros, uint32_t ones, uint32_t limit,
	uint32_t maxFactor)
{
	uint32_t total = zeros + ones;
	uint8_t p = total == 0 ? 128 : probability_of(zeros, total);
	uint32_t count = total < limit ? total : limit;
	uint32_t factor = maxFactor * count / limit;
	return (uint8_t)((pre * (256 - factor) + p * factor + 128) >> 8);
}


/*!	merge_prob() with the mode and motion vector counts' saturation: 20,
	at most 128 of 256 for the counts. */
static uint8_t
merge_mode(uint8_t pre, uint32_t zeros, uint32_t ones)
{
	uint32_t total = zeros + ones;
	if (total == 0)
		return pre;
	uint32_t factor = kCountToFactor[total < 20 ? total : 20];
	uint8_t p = probability_of(zeros, total);
	return (uint8_t)((pre * (256 - factor) + p * factor + 128) >> 8);
}


static uint32_t
merge_tree(const int8_t *tree, int node, const uint8_t *pre,
	const uint32_t *counts, uint8_t *probs)
{
	int left = tree[node];
	uint32_t leftCount = left <= 0 ? counts[-left]
		: merge_tree(tree, left, pre, counts, probs);
	int right = tree[node + 1];
	uint32_t rightCount = right <= 0 ? counts[-right]
		: merge_tree(tree, right, pre, counts, probs);
	probs[node >> 1] = merge_mode(pre[node >> 1], leftCount, rightCount);
	return leftCount + rightCount;
}


void
vp9_adapt_probs(Vp9Probs *probs, const Vp9Probs *pre, const Vp9Counts *counts,
	const Vp9Frame *frame)
{
	int intra = frame->frameType == VP9_KEY_FRAME || frame->intraOnly;

	/* coefficients: faster right after a key frame */
	uint32_t factor = !intra && frame->lastFrameWasKey ? 128 : 112;
	for (int t = 0; t < 4; t++) {
		for (int i = 0; i < 2; i++) {
			for (int j = 0; j < 2; j++) {
				for (int k = 0; k < 6; k++) {
					for (int l = 0; l < (k == 0 ? 3 : 6); l++) {
						const uint32_t *c = counts->coef[t][i][j][k][l];
						uint32_t eob = counts->eobBranch[t][i][j][k][l];
						const uint8_t *before = pre->coef[t][i][j][k][l];
						uint8_t *after = probs->coef[t][i][j][k][l];
						after[0] = merge_probability(before[0], c[3],
							eob >= c[3] ? eob - c[3] : 0, 24, factor);
						after[1] = merge_probability(before[1], c[0],
							c[1] + c[2], 24, factor);
						after[2] = merge_probability(before[2], c[1], c[2], 24,
							factor);
					}
				}
			}
		}
	}
	if (intra)
		return;

	for (int i = 0; i < 4; i++) {
		probs->intraInter[i] = merge_mode(pre->intraInter[i],
			counts->intraInter[i][0], counts->intraInter[i][1]);
	}
	for (int i = 0; i < 5; i++) {
		probs->compInter[i] = merge_mode(pre->compInter[i],
			counts->compInter[i][0], counts->compInter[i][1]);
		probs->compRef[i] = merge_mode(pre->compRef[i],
			counts->compRef[i][0], counts->compRef[i][1]);
		for (int j = 0; j < 2; j++) {
			probs->singleRef[i][j] = merge_mode(pre->singleRef[i][j],
				counts->singleRef[i][j][0], counts->singleRef[i][j][1]);
		}
	}
	for (int i = 0; i < 7; i++) {
		merge_tree(kInterModeTree, 0, pre->interMode[i], counts->interMode[i],
			probs->interMode[i]);
	}
	for (int i = 0; i < 4; i++) {
		merge_tree(kIntraModeTree, 0, pre->yMode[i], counts->yMode[i],
			probs->yMode[i]);
	}
	for (int i = 0; i < 10; i++) {
		merge_tree(kIntraModeTree, 0, pre->uvMode[i], counts->uvMode[i],
			probs->uvMode[i]);
	}
	for (int i = 0; i < 16; i++) {
		merge_tree(kPartitionTree, 0, pre->partition[i], counts->partition[i],
			probs->partition[i]);
	}
	if (frame->interpFilter == VP9_SWITCHABLE) {
		for (int i = 0; i < 4; i++) {
			merge_tree(kInterpTree, 0, pre->interp[i], counts->interp[i],
				probs->interp[i]);
		}
	}
	if (frame->txMode == VP9_TX_MODE_SELECT) {
		for (int i = 0; i < 2; i++) {
			const uint32_t *c8 = counts->tx8[i];
			probs->tx8[i][0] = merge_mode(pre->tx8[i][0], c8[0], c8[1]);
			const uint32_t *c16 = counts->tx16[i];
			probs->tx16[i][0] = merge_mode(pre->tx16[i][0], c16[0],
				c16[1] + c16[2]);
			probs->tx16[i][1] = merge_mode(pre->tx16[i][1], c16[1], c16[2]);
			const uint32_t *c32 = counts->tx32[i];
			probs->tx32[i][0] = merge_mode(pre->tx32[i][0], c32[0],
				c32[1] + c32[2] + c32[3]);
			probs->tx32[i][1] = merge_mode(pre->tx32[i][1], c32[1],
				c32[2] + c32[3]);
			probs->tx32[i][2] = merge_mode(pre->tx32[i][2], c32[2], c32[3]);
		}
	}
	for (int i = 0; i < 3; i++) {
		probs->skip[i] = merge_mode(pre->skip[i], counts->skip[i][0],
			counts->skip[i][1]);
	}

	merge_tree(kMvJointTree, 0, pre->mvJoints, counts->mvJoints,
		probs->mvJoints);
	for (int i = 0; i < 2; i++) {
		const Vp9MvComponentProbs *b = &pre->mv[i];
		Vp9MvComponentProbs *a = &probs->mv[i];
		const Vp9MvComponentCounts *c = &counts->mv[i];
		a->sign = merge_mode(b->sign, c->sign[0], c->sign[1]);
		merge_tree(kMvClassTree, 0, b->classes, c->classes, a->classes);
		merge_tree(kMvClass0Tree, 0, b->class0, c->class0, a->class0);
		for (int j = 0; j < 10; j++)
			a->bits[j] = merge_mode(b->bits[j], c->bits[j][0], c->bits[j][1]);
		for (int j = 0; j < 2; j++) {
			merge_tree(kMvFpTree, 0, b->class0Fp[j], c->class0Fp[j],
				a->class0Fp[j]);
		}
		merge_tree(kMvFpTree, 0, b->fp, c->fp, a->fp);
		if (frame->allowHighPrecisionMv) {
			a->class0Hp = merge_mode(b->class0Hp, c->class0Hp[0],
				c->class0Hp[1]);
			a->hp = merge_mode(b->hp, c->hp[0], c->hp[1]);
		}
	}
}


void
vp9_finish_frame(Vp9State *state, Vp9Frame *frame, const Vp9Counts *counts)
{
	if (frame->showExistingFrame)
		return;

	if (counts != NULL) {
		if (frame->refreshProbs) {
			vp9_adapt_probs(&frame->probs,
				&state->contexts[frame->frameContextIdx], counts, frame);
		}
		if (frame->refreshFrameContext) {
			state->contexts[frame->frameContextIdx] = frame->probs;
			state->contextsValid |= 1 << frame->frameContextIdx;
		}
	}

	for (int i = 0; i < VP9_REF_SLOTS; i++) {
		if ((frame->refreshFrameFlags & (1 << i)) == 0)
			continue;
		Vp9Slot *slot = &state->slots[i];
		slot->valid = 1;
		slot->width = frame->width;
		slot->height = frame->height;
		slot->bitDepth = frame->bitDepth;
		slot->subsamplingX = frame->subsamplingX;
		slot->subsamplingY = frame->subsamplingY;
	}

	/* a frame that did not decode leaves no motion vectors to predict
	   from */
	state->haveLast = counts != NULL;
	state->lastWidth = frame->width;
	state->lastHeight = frame->height;
	state->lastShowFrame = frame->showFrame;
}
