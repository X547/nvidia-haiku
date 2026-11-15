/* See nvdec_vp9.h.
 *
 * What the engine is given for a frame follows the one description of it
 * outside NVIDIA there is: averne's NVTEGRA hardware acceleration for FFmpeg
 * (for the Tegra X1's NVDEC), which drives the same methods and reads the
 * same nvdec_vp9_pic_s, nvdec_vp9EntropyProbs_t and nvdec_vp9EntropyCounts_t
 * of nvdec_drv.h. Where this chip needed something else, the comment says.
 */

#include "nvdec_vp9.h"
#include "nvdec_vp9_tables.h"
#include "vp9_parse.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video/nvdec_drv.h"

/* Eight reference slots, the frame being decoded, and one shown frame the
 * caller has not taken yet - two, since a show_existing_frame can follow a
 * frame no slot keeps. */
#define MAX_SURFACES		11
#define BITSTREAM_SLACK		256

/* What this generation decodes: 4096x2304 (8192x8192 from Turing). A frame
 * one superblock (64 pixels) wide or tall the GP104 does not: it reports
 * error 3 and decodes nothing, and the inter frames of one 8 or 10 pixels
 * wide or tall it never finishes. */
#define MAX_WIDTH		4096
#define MAX_HEIGHT		2304
#define MIN_SIZE		65

#define NVC2B0_VP9_SET_PROB_TAB_BUF_OFFSET	0x05c0
#define NVC2B0_VP9_SET_CTX_COUNTER_BUF_OFFSET	0x05c4
#define NVC2B0_VP9_SET_SEGMENT_READ_BUF_OFFSET	0x05c8
#define NVC2B0_VP9_SET_SEGMENT_WRITE_BUF_OFFSET	0x05cc
#define NVC2B0_VP9_SET_TILE_SIZE_BUF_OFFSET	0x05d0
#define NVC2B0_VP9_SET_COL_MVWRITE_BUF_OFFSET	0x05d4
#define NVC2B0_VP9_SET_COL_MVREAD_BUF_OFFSET	0x05d8
#define NVC2B0_VP9_SET_FILTER_BUFFER_OFFSET	0x05dc

/* Bytes of the filter buffer per line of the picture: the Tegra X1 puts the
 * BSD control part 912 bytes a line in and ends it at 988. Spacing them
 * wider than this chip needs costs memory and nothing else, as for H.265. */
#define FILTER_PER_LINE		2048
#define BSD_PER_LINE		256

/* Bytes per 64x64 superblock: the segment map (nvdec_drv.h's
 * GIP_ASIC_VP9_CTB_SEG_SIZE is 32) and the motion vectors the next frame
 * predicts from (the Tegra driver's 1024), doubled for safety. */
#define SEGMENT_PER_SB		64
#define MOTION_PER_SB		2048

/* The engine's table of tile sizes, and two values the Tegra driver puts in
 * it once, for reasons it does not give. */
#define TILE_SIZES_BYTES	0x700

typedef struct {
	NvdecBuffer	buffer;		/* luma, then chroma */
	size_t		chromaOffset;
	int		width, height;	/* of the frame in it */
	int		pitch;
	int		bitDepth;
	int		slots;		/* reference slots holding it */
	int		queued;		/* times it waits to be shown */
	bool		heldByCaller;
	bool		current;
} Surface;

typedef struct {
	int		surface;
	int64_t		time;
} Output;

struct NvdecVp9 {
	NvdecEngine	*engine;
	char		*reason;
	size_t		reasonSize;
	char		error[256];
	char		parseError[160];
	bool		refused;

	Vp9State	*state;
	Vp9Frame	*frame;
	Vp9Counts	*counts;
	int		slotSurface[VP9_REF_SLOTS];
	Surface		surfaces[MAX_SURFACES];
	Output		output[MAX_SURFACES];
	int		outputCount;
	bool		awaitKeyFrame;

	/* The working buffers, for frames up to this size. */
	int		bufferWidth, bufferHeight;
	NvdecBuffer	pictureSetup, probabilities, counters, tileSizes;
	NvdecBuffer	segment[2], motion[2], filter, status, bitstream;
	int		segmentRead;	/* which segment map the last frame left */
	int		motionWrite;	/* which motion buffer this frame fills */
	uint32_t	bsdOffset, filterAboveOffset;

	bool		haveFormat;
	int		width, height, bitDepth;
	NvdecStatus	lastStatus;
	uint32_t	pictureCount;
	uint32_t	errorsReported;
};

static bool
setError(NvdecVp9 *decoder, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(decoder->error, sizeof(decoder->error), format, args);
	va_end(args);
	if (decoder->reason != NULL && decoder->reasonSize > 0)
		snprintf(decoder->reason, decoder->reasonSize, "%s", decoder->error);
	return false;
}

const char *
nvdecVp9LastError(const NvdecVp9 *decoder)
{
	return decoder->error;
}

void
nvdecVp9GetStatus(const NvdecVp9 *decoder, NvdecStatus *status)
{
	*status = decoder->lastStatus;
}

bool
nvdecVp9Refused(const NvdecVp9 *decoder)
{
	return decoder->refused;
}

bool
nvdecVp9HasFormat(const NvdecVp9 *decoder, int *width, int *height,
	int *bitDepth)
{
	if (!decoder->haveFormat)
		return false;
	if (width != NULL)
		*width = decoder->width;
	if (height != NULL)
		*height = decoder->height;
	if (bitDepth != NULL)
		*bitDepth = decoder->bitDepth;
	return true;
}

static uint32_t
alignUp(uint32_t value, uint32_t to)
{
	return (value + to - 1) / to * to;
}

/* --------------------------------------------------------------- buffers */

static void
freeWorkingBuffers(NvdecVp9 *decoder)
{
	NvdecEngine *engine = decoder->engine;
	nvdecFree(engine, &decoder->pictureSetup);
	nvdecFree(engine, &decoder->probabilities);
	nvdecFree(engine, &decoder->counters);
	nvdecFree(engine, &decoder->tileSizes);
	nvdecFree(engine, &decoder->segment[0]);
	nvdecFree(engine, &decoder->segment[1]);
	nvdecFree(engine, &decoder->motion[0]);
	nvdecFree(engine, &decoder->motion[1]);
	nvdecFree(engine, &decoder->filter);
	nvdecFree(engine, &decoder->status);
	nvdecFree(engine, &decoder->bitstream);
	decoder->bufferWidth = decoder->bufferHeight = 0;
}

/* The buffers whose size goes with the frame's, made larger when a frame
 * needs it. What they held is lost, which a frame of a new size allows: it
 * predicts neither its segment map nor its motion vectors from the last. */
static bool
prepareWorkingBuffers(NvdecVp9 *decoder, int width, int height)
{
	if (width <= decoder->bufferWidth && height <= decoder->bufferHeight)
		return true;
	if (width < decoder->bufferWidth)
		width = decoder->bufferWidth;
	if (height < decoder->bufferHeight)
		height = decoder->bufferHeight;
	freeWorkingBuffers(decoder);

	NvdecEngine *engine = decoder->engine;
	uint32_t alignedWidth = alignUp((uint32_t)width, 64);
	uint32_t alignedHeight = alignUp((uint32_t)height, 64);
	uint32_t superblocks = (alignedWidth / 64) * (alignedHeight / 64);

	/* The filter buffer: the filter's part and the BSD control part, a
	 * stretch for every line, then the line above (nvdec_drv.h's
	 * VP9_DBLK_TOP_BUF_SIZE, generously). Units of 256 bytes. */
	uint32_t at = FILTER_PER_LINE * alignedHeight;
	decoder->bsdOffset = at;
	at += BSD_PER_LINE * alignedHeight;
	decoder->filterAboveOffset = at;
	at += alignUp((alignedWidth / 64 + 2) * 2048 + (1u << 20), 256);

	if (!nvdecAlloc(engine, &decoder->pictureSetup, sizeof(nvdec_vp9_pic_s) + 0x100, true)
		|| !nvdecAlloc(engine, &decoder->probabilities,
			sizeof(nvdec_vp9EntropyProbs_t), true)
		|| !nvdecAlloc(engine, &decoder->counters,
			sizeof(nvdec_vp9EntropyCounts_t), true)
		|| !nvdecAlloc(engine, &decoder->tileSizes, TILE_SIZES_BYTES, true)
		/* In system memory, to be cleared from here. */
		|| !nvdecAlloc(engine, &decoder->segment[0], superblocks * SEGMENT_PER_SB, true)
		|| !nvdecAlloc(engine, &decoder->segment[1], superblocks * SEGMENT_PER_SB, true)
		|| !nvdecAlloc(engine, &decoder->motion[0], superblocks * MOTION_PER_SB, false)
		|| !nvdecAlloc(engine, &decoder->motion[1], superblocks * MOTION_PER_SB, false)
		|| !nvdecAlloc(engine, &decoder->filter, at, false)
		/* NVK finds a small status buffer makes HEVC pictures fail. */
		|| !nvdecAlloc(engine, &decoder->status, 0x10000, true)) {
		char why[192];
		snprintf(why, sizeof(why), "%s", decoder->reason != NULL ? decoder->reason : "");
		freeWorkingBuffers(decoder);
		return setError(decoder, "no room for the decoder's working buffers "
			"for %dx%d: %s", width, height, why);
	}
	uint16_t *sizes = decoder->tileSizes.data;
	sizes[0x37a] = 9;
	sizes[0x37b] = 1;
	decoder->bufferWidth = width;
	decoder->bufferHeight = height;
	decoder->segmentRead = 0;
	decoder->motionWrite = 0;
	return true;
}

static bool
growBitstream(NvdecVp9 *decoder, size_t needed)
{
	needed = (needed + 0xffff) & ~(size_t)0xffff;
	if (decoder->bitstream.size >= needed)
		return true;
	nvdecFree(decoder->engine, &decoder->bitstream);
	if (!nvdecAlloc(decoder->engine, &decoder->bitstream, needed, true))
		return setError(decoder, "no room for %zu bytes of bitstream", needed);
	return true;
}

static bool
surfaceInUse(const Surface *surface)
{
	return surface->slots > 0 || surface->queued > 0 || surface->heldByCaller
		|| surface->current;
}

/* A surface for a frame of this size: one nothing uses, made larger if it
 * has to be. Lines a whole number of 64 byte groups apart and planes a whole
 * number of superblocks tall, as for H.265. */
static int
takeSurface(NvdecVp9 *decoder, int width, int height, int bitDepth)
{
	int bytes = bitDepth > 8 ? 2 : 1;
	int pitch = (int)alignUp((uint32_t)width * bytes, 64);
	size_t lumaSize = (size_t)pitch * alignUp((uint32_t)height, 64);
	size_t size = lumaSize + lumaSize / 2;

	int chosen = -1;
	/* One large enough first, then any. */
	for (int pass = 0; pass < 2 && chosen < 0; pass++) {
		for (int i = 0; i < MAX_SURFACES; i++) {
			Surface *surface = &decoder->surfaces[i];
			if (surfaceInUse(surface))
				continue;
			if (pass == 0 && surface->buffer.size < size)
				continue;
			chosen = i;
			break;
		}
	}
	if (chosen < 0) {
		setError(decoder, "every surface is still in use");
		return -1;
	}
	Surface *surface = &decoder->surfaces[chosen];
	if (surface->buffer.size < size) {
		nvdecFree(decoder->engine, &surface->buffer);
		bool inSystemMemory = getenv("NVDEC_SURFACES_IN_VRAM") == NULL;
		if (!nvdecAlloc(decoder->engine, &surface->buffer, size, inSystemMemory)) {
			char why[192];
			snprintf(why, sizeof(why), "%s",
				decoder->reason != NULL ? decoder->reason : "");
			setError(decoder, "no room for a %dx%d frame: %s", width, height, why);
			return -1;
		}
	}
	surface->chromaOffset = lumaSize;
	surface->width = width;
	surface->height = height;
	surface->pitch = pitch;
	surface->bitDepth = bitDepth;
	return chosen;
}

/* ------------------------------------------------------------- lifetime */

NvdecVp9 *
nvdecVp9Create(NvdecEngine *engine, char *reason, size_t reasonSize)
{
	NvdecVp9 *decoder = calloc(1, sizeof(NvdecVp9));
	if (decoder == NULL)
		return NULL;
	decoder->state = malloc(sizeof(Vp9State));
	decoder->frame = malloc(sizeof(Vp9Frame));
	decoder->counts = malloc(sizeof(Vp9Counts));
	if (decoder->state == NULL || decoder->frame == NULL || decoder->counts == NULL) {
		free(decoder->state);
		free(decoder->frame);
		free(decoder->counts);
		free(decoder);
		return NULL;
	}
	decoder->engine = engine;
	decoder->reason = reason;
	decoder->reasonSize = reasonSize;
	vp9_init(decoder->state);
	for (int i = 0; i < VP9_REF_SLOTS; i++)
		decoder->slotSurface[i] = -1;
	decoder->awaitKeyFrame = true;
	return decoder;
}

void
nvdecVp9Destroy(NvdecVp9 *decoder)
{
	if (decoder == NULL)
		return;
	freeWorkingBuffers(decoder);
	for (int i = 0; i < MAX_SURFACES; i++)
		nvdecFree(decoder->engine, &decoder->surfaces[i].buffer);
	free(decoder->state);
	free(decoder->frame);
	free(decoder->counts);
	free(decoder);
}

void
nvdecVp9Reset(NvdecVp9 *decoder)
{
	vp9_reset(decoder->state);
	for (int i = 0; i < VP9_REF_SLOTS; i++)
		decoder->slotSurface[i] = -1;
	for (int i = 0; i < MAX_SURFACES; i++) {
		decoder->surfaces[i].slots = 0;
		decoder->surfaces[i].queued = 0;
		decoder->surfaces[i].current = false;
	}
	decoder->outputCount = 0;
	decoder->awaitKeyFrame = true;
}

/* ------------------------------------------------------------ showing them */

static void
queueOutput(NvdecVp9 *decoder, int surface, int64_t time)
{
	if (decoder->outputCount >= MAX_SURFACES)
		return;
	decoder->output[decoder->outputCount].surface = surface;
	decoder->output[decoder->outputCount].time = time;
	decoder->outputCount++;
	decoder->surfaces[surface].queued++;
}

bool
nvdecVp9NextFrame(NvdecVp9 *decoder, NvdecFrame *frame)
{
	if (decoder->outputCount == 0)
		return false;
	Output next = decoder->output[0];
	memmove(decoder->output, decoder->output + 1,
		(size_t)(decoder->outputCount - 1) * sizeof(Output));
	decoder->outputCount--;

	Surface *surface = &decoder->surfaces[next.surface];
	surface->queued--;
	surface->heldByCaller = true;

	memset(frame, 0, sizeof(*frame));
	frame->width = surface->width;
	frame->height = surface->height;
	frame->codedWidth = surface->width;
	frame->codedHeight = surface->height;
	frame->pitch = surface->pitch;
	frame->luma = surface->buffer.data;
	frame->chroma = (const uint8_t *)surface->buffer.data + surface->chromaOffset;
	frame->bitDepth = surface->bitDepth;
	frame->pictureOrder = (int)decoder->pictureCount;
	frame->time = next.time;
	frame->handle = next.surface;
	return true;
}

void
nvdecVp9ReleaseFrame(NvdecVp9 *decoder, const NvdecFrame *frame)
{
	if (frame->handle < 0 || frame->handle >= MAX_SURFACES)
		return;
	decoder->surfaces[frame->handle].heldByCaller = false;
}

void
nvdecVp9DrainAll(NvdecVp9 *decoder)
{
	/* Nothing is held back for reordering. */
	(void)decoder;
}

/* --------------------------------------------------------------- decoding */

/* The probabilities the engine decodes the frame with, in its layout: the
 * key frame tables, the segmentation's, and the frame's context. The order
 * of the intra modes is libvpx's, which the parser keeps. */
static void
fillProbabilities(nvdec_vp9EntropyProbs_t *out, const Vp9Frame *frame)
{
	const Vp9Probs *p = &frame->probs;
	memset(out, 0, sizeof(*out));

	for (int i = 0; i < 10; i++) {
		for (int j = 0; j < 10; j++) {
			memcpy(out->kf_bmode_prob[i][j], kNvdecVp9KfYModeProbs[i][j], 8);
			out->kf_bmode_probB[i][j][0] = kNvdecVp9KfYModeProbs[i][j][8];
		}
		memcpy(out->kf_uv_mode_prob[i], kNvdecVp9KfUvModeProbs[i], 8);
		out->kf_uv_mode_probB[i][0] = kNvdecVp9KfUvModeProbs[i][8];
	}
	memcpy(out->mb_segment_tree_probs, frame->seg.treeProbs, 7);
	if (frame->seg.temporalUpdate)
		memcpy(out->segment_pred_probs, frame->seg.predProbs, 3);
	else
		memset(out->segment_pred_probs, 0xff, 3);

	nvdec_vp9AdaptiveEntropyProbs_t *a = &out->a;
	for (int i = 0; i < 7; i++)
		memcpy(a->inter_mode_prob[i], p->interMode[i], 3);
	memcpy(a->intra_inter_prob, p->intraInter, 4);
	for (int i = 0; i < 10; i++) {
		memcpy(a->uv_mode_prob[i], p->uvMode[i], 8);
		a->uv_mode_probB[i][0] = p->uvMode[i][8];
	}
	for (int i = 0; i < 2; i++) {
		a->tx8x8_prob[i][0] = p->tx8[i][0];
		memcpy(a->tx16x16_prob[i], p->tx16[i], 2);
		memcpy(a->tx32x32_prob[i], p->tx32[i], 3);
	}
	for (int i = 0; i < 4; i++) {
		memcpy(a->sb_ymode_prob[i], p->yMode[i], 8);
		a->sb_ymode_probB[i][0] = p->yMode[i][8];
	}
	/* Partition contexts in libvpx's order, 8x8 blocks first: the key
	 * frame ones, then the frame's. */
	for (int i = 0; i < 16; i++) {
		memcpy(a->partition_prob[0][i], kNvdecVp9KfPartitionProbs[i], 3);
		memcpy(a->partition_prob[1][i], p->partition[i], 3);
	}
	memcpy(a->switchable_interp_prob, p->interp, sizeof(a->switchable_interp_prob));
	memcpy(a->comp_inter_prob, p->compInter, 5);
	memcpy(a->mbskip_probs, p->skip, 3);

	memcpy(a->nmvc.joints, p->mvJoints, 3);
	for (int i = 0; i < 2; i++) {
		const Vp9MvComponentProbs *mv = &p->mv[i];
		a->nmvc.sign[i] = mv->sign;
		a->nmvc.class0[i][0] = mv->class0[0];
		memcpy(a->nmvc.fp[i], mv->fp, 3);
		a->nmvc.class0_hp[i] = mv->class0Hp;
		a->nmvc.hp[i] = mv->hp;
		memcpy(a->nmvc.classes[i], mv->classes, 10);
		memcpy(a->nmvc.class0_fp[i], mv->class0Fp, sizeof(a->nmvc.class0_fp[i]));
		memcpy(a->nmvc.bits[i], mv->bits, 10);
	}
	memcpy(a->single_ref_prob, p->singleRef, sizeof(a->single_ref_prob));
	memcpy(a->comp_ref_prob, p->compRef, 5);

	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 2; j++) {
			for (int k = 0; k < 6; k++) {
				for (int l = 0; l < 6; l++) {
					memcpy(a->probCoeffs[i][j][k][l], p->coef[0][i][j][k][l], 3);
					memcpy(a->probCoeffs8x8[i][j][k][l], p->coef[1][i][j][k][l], 3);
					memcpy(a->probCoeffs16x16[i][j][k][l], p->coef[2][i][j][k][l], 3);
					memcpy(a->probCoeffs32x32[i][j][k][l], p->coef[3][i][j][k][l], 3);
				}
			}
		}
	}
}

/* The engine's symbol counts, in libvpx's FRAME_COUNTS the parser adapts
 * from. The inter modes are counted per node of their tree: zero against the
 * rest, nearest against the rest, near against new. */
static void
readCounts(Vp9Counts *out, const nvdec_vp9EntropyCounts_t *in)
{
	memset(out, 0, sizeof(*out));
	memcpy(out->yMode, in->sb_ymode_counts, sizeof(out->yMode));
	memcpy(out->uvMode, in->uv_mode_counts, sizeof(out->uvMode));
	memcpy(out->partition, in->partition_counts, sizeof(out->partition));
	for (int i = 0; i < 2; i++) {
		for (int j = 0; j < 2; j++) {
			for (int k = 0; k < 6; k++) {
				for (int l = 0; l < 6; l++) {
					memcpy(out->coef[0][i][j][k][l], in->countCoeffs[i][j][k][l], 16);
					memcpy(out->coef[1][i][j][k][l], in->countCoeffs8x8[i][j][k][l], 16);
					memcpy(out->coef[2][i][j][k][l], in->countCoeffs16x16[i][j][k][l], 16);
					memcpy(out->coef[3][i][j][k][l], in->countCoeffs32x32[i][j][k][l], 16);
				}
			}
		}
	}
	memcpy(out->eobBranch, in->countEobs, sizeof(out->eobBranch));
	memcpy(out->interp, in->switchable_interp_counts, sizeof(out->interp));
	for (int i = 0; i < 7; i++) {
		out->interMode[i][0] = in->inter_mode_counts[i][1][0];	/* nearest */
		out->interMode[i][1] = in->inter_mode_counts[i][2][0];	/* near */
		out->interMode[i][2] = in->inter_mode_counts[i][0][0];	/* zero */
		out->interMode[i][3] = in->inter_mode_counts[i][2][1];	/* new */
	}
	memcpy(out->intraInter, in->intra_inter_count, sizeof(out->intraInter));
	memcpy(out->compInter, in->comp_inter_count, sizeof(out->compInter));
	memcpy(out->singleRef, in->single_ref_count, sizeof(out->singleRef));
	memcpy(out->compRef, in->comp_ref_count, sizeof(out->compRef));
	memcpy(out->tx32, in->tx32x32_count, sizeof(out->tx32));
	memcpy(out->tx16, in->tx16x16_count, sizeof(out->tx16));
	memcpy(out->tx8, in->tx8x8_count, sizeof(out->tx8));
	memcpy(out->skip, in->mbskip_count, sizeof(out->skip));
	memcpy(out->mvJoints, in->nmvcount.joints, sizeof(out->mvJoints));
	for (int i = 0; i < 2; i++) {
		Vp9MvComponentCounts *mv = &out->mv[i];
		memcpy(mv->sign, in->nmvcount.sign[i], sizeof(mv->sign));
		memcpy(mv->classes, in->nmvcount.classes[i], sizeof(mv->classes));
		memcpy(mv->class0, in->nmvcount.class0[i], sizeof(mv->class0));
		memcpy(mv->bits, in->nmvcount.bits[i], sizeof(mv->bits));
		memcpy(mv->class0Fp, in->nmvcount.class0_fp[i], sizeof(mv->class0Fp));
		memcpy(mv->fp, in->nmvcount.fp[i], sizeof(mv->fp));
		memcpy(mv->class0Hp, in->nmvcount.class0_hp[i], sizeof(mv->class0Hp));
		memcpy(mv->hp, in->nmvcount.hp[i], sizeof(mv->hp));
	}
}

/* Columns and rows of each tile, in superblocks, row by row. */
static void
fillTileSizes(uint16_t *sizes, const Vp9Frame *frame)
{
	int sbCols = (frame->miCols + 7) >> 3;
	int sbRows = (frame->miRows + 7) >> 3;
	int columns = 1 << frame->log2TileCols, rows = 1 << frame->log2TileRows;
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < columns; c++) {
			*sizes++ = (uint16_t)(((sbCols * (c + 1)) >> frame->log2TileCols)
				- ((sbCols * c) >> frame->log2TileCols));
			*sizes++ = (uint16_t)(((sbRows * (r + 1)) >> frame->log2TileRows)
				- ((sbRows * r) >> frame->log2TileRows));
		}
	}
}

static void
setSlot(NvdecVp9 *decoder, int slot, int surface)
{
	int old = decoder->slotSurface[slot];
	if (old >= 0)
		decoder->surfaces[old].slots--;
	decoder->slotSurface[slot] = surface;
	if (surface >= 0)
		decoder->surfaces[surface].slots++;
}

/* One frame of a chunk. Returns false only for what ends the stream (a
 * refusal) or the engine not answering; a frame that cannot be decoded
 * otherwise waits for the next key frame, as libvpx does. */
static bool
decodeFrame(NvdecVp9 *decoder, const uint8_t *data, size_t size, int64_t time)
{
	Vp9State *state = decoder->state;
	Vp9Frame *frame = decoder->frame;

	/* What the frame before this one was, for the engine's choice of
	 * whether to predict motion vectors from it. */
	bool lastWasKey = state->haveLast && state->frameType == VP9_KEY_FRAME;
	int lastWidth = state->lastWidth, lastHeight = state->lastHeight;
	bool lastShown = state->haveLast && state->lastShowFrame;

	int result = vp9_parse_frame(state, data, size, frame, decoder->parseError);
	if (result == -2) {
		decoder->refused = true;
		return setError(decoder, "the engine does not decode this stream: %s",
			decoder->parseError);
	}
	if (result != 0) {
		if (!decoder->awaitKeyFrame) {
			fprintf(stderr, "nvdec vp9: frame %u: %s\n", decoder->pictureCount,
				decoder->parseError);
		}
		decoder->awaitKeyFrame = true;
		return true;
	}

	if (frame->showExistingFrame) {
		int surface = decoder->slotSurface[frame->frameToShow];
		if (!decoder->awaitKeyFrame && surface >= 0)
			queueOutput(decoder, surface, time);
		return true;
	}

	bool intra = frame->frameType == VP9_KEY_FRAME || frame->intraOnly;
	if (decoder->awaitKeyFrame && !intra)
		return true;

	if (frame->width > MAX_WIDTH || frame->height > MAX_HEIGHT) {
		decoder->refused = true;
		return setError(decoder, "the engine decodes up to %dx%d, this is %dx%d",
			MAX_WIDTH, MAX_HEIGHT, frame->width, frame->height);
	}
	if (frame->width < MIN_SIZE || frame->height < MIN_SIZE) {
		decoder->refused = true;
		return setError(decoder, "the engine decodes from %dx%d, this is %dx%d",
			MIN_SIZE, MIN_SIZE, frame->width, frame->height);
	}
	/* The GP104 - the GTX 1070 and 1080 - decodes VP9 in eight bits only:
	 * given a ten-bit frame (v1.bit_depth 10) it starts its predictions
	 * from 512 but keeps eight bits of every sample and dequantises as for
	 * eight, and the picture is wrong from the first frame. Other chips of
	 * the generation (GP102, GP107) and Turing are said to decode ten bits,
	 * which NVDEC_VP9_TEN_BIT=1 tries; it has not been measured. */
	if (frame->bitDepth > 8 && getenv("NVDEC_VP9_TEN_BIT") == NULL) {
		decoder->refused = true;
		return setError(decoder, "the engine decodes eight-bit VP9 only, this "
			"is %d bits (profile %d)", frame->bitDepth, frame->profile);
	}

	int references[3] = { -1, -1, -1 };
	if (!intra) {
		for (int i = 0; i < 3; i++) {
			references[i] = decoder->slotSurface[frame->refSlot[i]];
			if (references[i] < 0) {
				decoder->awaitKeyFrame = true;
				return true;
			}
		}
	}
	decoder->awaitKeyFrame = false;

	if (!prepareWorkingBuffers(decoder, frame->width, frame->height))
		return false;
	int current = takeSurface(decoder, frame->width, frame->height, frame->bitDepth);
	if (current < 0)
		return false;
	Surface *target = &decoder->surfaces[current];
	for (int i = 0; i < 3; i++) {
		if (references[i] < 0)
			references[i] = current;
	}

	size_t headers = frame->uncompressedHeaderSize + frame->compressedHeaderSize;
	size_t streamLength = size - headers;
	if (!growBitstream(decoder, streamLength + BITSTREAM_SLACK))
		return false;
	memcpy(decoder->bitstream.data, data + headers, streamLength);
	memset((uint8_t *)decoder->bitstream.data + streamLength, 0, BITSTREAM_SLACK);

	/* A frame that starts afresh, or one of another size, has no segment
	 * map to inherit. */
	if (frame->resetSegmentMap) {
		memset(decoder->segment[0].data, 0, decoder->segment[0].size);
		memset(decoder->segment[1].data, 0, decoder->segment[1].size);
	}

	int bytes = frame->bitDepth > 8 ? 2 : 1;
	nvdec_vp9_pic_s *setup = decoder->pictureSetup.data;
	memset(setup, 0, sizeof(*setup));
	setup->stream_len = (unsigned)streamLength;
	setup->gptimer_timeout_value = 0;		/* the firmware's default */
	setup->tileformat = 1;				/* blocks, as for H.264 */
	setup->gob_height = 0;
	setup->Vp9BsdCtrlOffset = decoder->bsdOffset >> 8;

	/* Sizes and strides of the references, strides in samples. */
	const Surface *ref[3];
	for (int i = 0; i < 3; i++)
		ref[i] = &decoder->surfaces[references[i]];
	setup->ref0_width = (unsigned short)ref[0]->width;
	setup->ref0_height = (unsigned short)ref[0]->height;
	setup->ref0_stride[0] = setup->ref0_stride[1]
		= (unsigned short)(ref[0]->pitch / bytes);
	setup->ref1_width = (unsigned short)ref[1]->width;
	setup->ref1_height = (unsigned short)ref[1]->height;
	setup->ref1_stride[0] = setup->ref1_stride[1]
		= (unsigned short)(ref[1]->pitch / bytes);
	setup->ref2_width = (unsigned short)ref[2]->width;
	setup->ref2_height = (unsigned short)ref[2]->height;
	setup->ref2_stride[0] = setup->ref2_stride[1]
		= (unsigned short)(ref[2]->pitch / bytes);
	setup->width = (unsigned short)frame->width;
	setup->height = (unsigned short)frame->height;
	setup->framestride[0] = setup->framestride[1]
		= (unsigned short)(target->pitch / bytes);

	setup->keyFrame = frame->frameType == VP9_KEY_FRAME;
	setup->prevIsKeyFrame = lastWasKey;
	setup->resolutionChange = frame->width != lastWidth || frame->height != lastHeight;
	setup->errorResilient = (unsigned)frame->errorResilient;
	/* The engine predicts motion vectors from the last frame's as libvpx
	 * does: only when that frame was shown, is of this size and was not
	 * intra only; the last also counts as not shown when there was none. */
	setup->prevShowFrame = lastShown && frame->usePrevFrameMvs;
	setup->intraOnly = (unsigned)frame->intraOnly;
	for (int i = 0; i < 4; i++)
		setup->refFrameSignBias[i] = (unsigned char)frame->signBias[i];
	setup->loopFilterLevel = (char)frame->filterLevel;
	setup->loopFilterSharpness = (char)frame->sharpness;

	setup->qpYAc = (unsigned char)frame->baseQIdx;
	setup->qpYDc = (char)frame->deltaQYDc;
	setup->qpChDc = (char)frame->deltaQUvDc;
	setup->qpChAc = (char)frame->deltaQUvAc;

	setup->lossless = (char)frame->lossless;
	setup->transform_mode = (char)frame->txMode;
	setup->allow_high_precision_mv = intra ? 0 : (char)frame->allowHighPrecisionMv;
	/* The filter as the header codes it - smooth 0, regular 1, sharp 2,
	 * bilinear 3 - rather than libvpx's numbering, which swaps the first
	 * two: with libvpx's, every frame with a fixed regular or smooth filter
	 * came out wrong. 4 is switchable in both. */
	setup->mcomp_filter_type = (char)(frame->interpFilter <= VP9_EIGHTTAP_SMOOTH
		? frame->interpFilter ^ 1 : frame->interpFilter);
	setup->comp_pred_mode = (char)frame->referenceMode;
	if (frame->referenceMode != VP9_SINGLE_REFERENCE) {
		setup->comp_fixed_ref = (char)frame->compFixedRef;
		setup->comp_var_ref[0] = (char)frame->compVarRef[0];
		setup->comp_var_ref[1] = (char)frame->compVarRef[1];
	}
	setup->log2_tile_columns = (char)frame->log2TileCols;
	setup->log2_tile_rows = (char)frame->log2TileRows;

	setup->segmentEnabled = (unsigned char)frame->seg.enabled;
	setup->segmentMapUpdate = (unsigned char)frame->seg.updateMap;
	setup->segmentMapTemporalUpdate = (unsigned char)frame->seg.temporalUpdate;
	setup->segmentFeatureMode = (unsigned char)frame->seg.absDelta;
	for (int i = 0; i < VP9_MAX_SEGMENTS; i++) {
		for (int j = 0; j < 4; j++) {
			setup->segmentFeatureEnable[i][j]
				= (frame->seg.featureEnabled[i] >> j) & 1;
			setup->segmentFeatureData[i][j] = j < 3 ? frame->seg.featureData[i][j] : 0;
		}
	}
	setup->modeRefLfEnabled = (char)frame->modeRefDeltaEnabled;
	for (int i = 0; i < 4; i++)
		setup->mbRefLfDelta[i] = (char)frame->refDeltas[i];
	for (int i = 0; i < 2; i++)
		setup->mbModeLfDelta[i] = (char)frame->modeDeltas[i];

	/* Read by engines from the GP100 on. The bit depth is read: it changes
	 * what the GP104 writes, eight-bit samples or sixteen-bit ones. */
	setup->v1.Vp9FltAboveOffset = decoder->filterAboveOffset >> 8;
	setup->v1.bit_depth = (unsigned)frame->bitDepth;

	fillProbabilities(decoder->probabilities.data, frame);
	memset(decoder->tileSizes.data, 0, 0x6f4);
	fillTileSizes(decoder->tileSizes.data, frame);
	memset(decoder->counters.data, 0, sizeof(nvdec_vp9EntropyCounts_t));
	memset(decoder->status.data, 0, sizeof(nvdec_status_s));

	int segmentWrite = frame->seg.updateMap ? 1 - decoder->segmentRead
		: decoder->segmentRead;

	NvdecEngine *engine = decoder->engine;
	nvdecBegin(engine);
	nvdecMethod(engine, NVC2B0_SET_APPLICATION_ID, NVDEC_CODEC_VP9);
	nvdecMethod(engine, NVC2B0_SET_WATCHDOG_TIMER, 0);
	nvdecMethod(engine, NVC2B0_SET_CONTROL_PARAMS,
		NVDEC_CODEC_VP9				/* CODEC_TYPE 3:0 */
		| (1u << 4)				/* GPTIMER_ON */
		| (1u << 6)				/* ERR_CONCEAL_ON */
		| (1u << 13));				/* MBTIMER_ON */
	nvdecMethod(engine, NVC2B0_SET_PICTURE_INDEX, decoder->pictureCount);
	nvdecAddress(engine, NVC2B0_SET_DRV_PIC_SETUP_OFFSET, decoder->pictureSetup.gpuAddress);
	nvdecAddress(engine, NVC2B0_SET_IN_BUF_BASE_OFFSET, decoder->bitstream.gpuAddress);
	nvdecAddress(engine, NVC2B0_SET_NVDEC_STATUS_OFFSET, decoder->status.gpuAddress);
	nvdecAddress(engine, NVC2B0_VP9_SET_PROB_TAB_BUF_OFFSET,
		decoder->probabilities.gpuAddress);
	nvdecAddress(engine, NVC2B0_VP9_SET_CTX_COUNTER_BUF_OFFSET,
		decoder->counters.gpuAddress);
	nvdecAddress(engine, NVC2B0_VP9_SET_TILE_SIZE_BUF_OFFSET, decoder->tileSizes.gpuAddress);
	nvdecAddress(engine, NVC2B0_VP9_SET_COL_MVWRITE_BUF_OFFSET,
		decoder->motion[decoder->motionWrite].gpuAddress);
	nvdecAddress(engine, NVC2B0_VP9_SET_COL_MVREAD_BUF_OFFSET,
		decoder->motion[1 - decoder->motionWrite].gpuAddress);
	nvdecAddress(engine, NVC2B0_VP9_SET_SEGMENT_READ_BUF_OFFSET,
		decoder->segment[decoder->segmentRead].gpuAddress);
	nvdecAddress(engine, NVC2B0_VP9_SET_SEGMENT_WRITE_BUF_OFFSET,
		decoder->segment[segmentWrite].gpuAddress);
	nvdecAddress(engine, NVC2B0_VP9_SET_FILTER_BUFFER_OFFSET, decoder->filter.gpuAddress);
	/* The three references, then the frame; the rest of the table points at
	 * the frame, so that a stray read finds a picture. */
	for (int i = 0; i < 17; i++) {
		const Surface *surface = i < 3 ? ref[i] : target;
		nvdecAddress(engine, NVC2B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i,
			surface->buffer.gpuAddress);
		nvdecAddress(engine, NVC2B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i,
			surface->buffer.gpuAddress + surface->chromaOffset);
	}
	nvdecMethod(engine, NVC2B0_EXECUTE, 1u << 0);

	target->current = true;
	if (!nvdecExecute(engine, 2000)) {
		target->current = false;
		decoder->awaitKeyFrame = true;
		return setError(decoder, "the decoder did not answer");
	}
	target->current = false;

	const nvdec_status_s *status = decoder->status.data;
	decoder->lastStatus.macroblocksDecoded = status->mbs_correctly_decoded;
	decoder->lastStatus.macroblocksInError = status->mbs_in_error;
	decoder->lastStatus.errorStatus = status->error_status;
	decoder->lastStatus.sliceHeaderError = status->slice_header_error_code;
	decoder->lastStatus.cycles = status->cycle_count;
	bool failed = status->error_status != 0;
	if (failed && decoder->errorsReported < 20) {
		decoder->errorsReported++;
		fprintf(stderr, "nvdec vp9: frame %u reported %#x, %u decoded, %u in "
			"error\n", decoder->pictureCount, status->error_status,
			status->mbs_correctly_decoded, status->mbs_in_error);
	}

	/* The backward adaptation, the saved context and the slots. A frame the
	 * engine reported an error in still is a reference - a concealed
	 * picture is better than a hole - but its counts are not trusted. */
	if (!failed)
		readCounts(decoder->counts, decoder->counters.data);
	vp9_finish_frame(state, frame, failed ? NULL : decoder->counts);
	if (frame->seg.updateMap)
		decoder->segmentRead = segmentWrite;
	decoder->motionWrite = 1 - decoder->motionWrite;
	for (int i = 0; i < VP9_REF_SLOTS; i++) {
		if ((frame->refreshFrameFlags & (1 << i)) != 0)
			setSlot(decoder, i, current);
	}
	if (frame->showFrame)
		queueOutput(decoder, current, time);

	decoder->haveFormat = true;
	decoder->width = frame->width;
	decoder->height = frame->height;
	decoder->bitDepth = frame->bitDepth;

	if (getenv("NVDEC_TRACE") != NULL) {
		fprintf(stderr, "%3u: %s%s %dx%d q %d lf %d tx %d interp %d refs %d/%d/%d "
			"-> surface %d refresh %#x ctx %d%s%s seg %d/%d/%d status %#x\n",
			decoder->pictureCount, frame->frameType == VP9_KEY_FRAME ? "key"
				: frame->intraOnly ? "intra" : "inter",
			frame->showFrame ? "" : " hidden", frame->width, frame->height,
			frame->baseQIdx, frame->filterLevel, frame->txMode,
			frame->interpFilter, references[0], references[1], references[2],
			current, frame->refreshFrameFlags, frame->frameContextIdx,
			frame->refreshProbs ? " adapt" : "",
			frame->usePrevFrameMvs ? " prevmvs" : "", frame->seg.enabled,
			frame->seg.updateMap, frame->seg.temporalUpdate,
			status->error_status);
	}
	decoder->pictureCount++;
	if (failed) {
		setError(decoder, "the decoder reported %#x on frame %u",
			status->error_status, decoder->pictureCount - 1);
	}
	return true;
}

bool
nvdecVp9Decode(NvdecVp9 *decoder, const uint8_t *data, size_t size, int64_t time)
{
	if (decoder->refused)
		return false;
	size_t offsets[VP9_MAX_FRAMES], sizes[VP9_MAX_FRAMES];
	int count = vp9_split_superframe(data, size, offsets, sizes);
	if (count == 0)
		return setError(decoder, "an empty chunk");

	/* As libvpx: a chunk shows one frame, the last it decoded to be shown
	 * (a spatial SVC superframe has one of every layer). */
	int queued = decoder->outputCount;
	bool result = true;
	for (int i = 0; i < count && result; i++)
		result = decodeFrame(decoder, data + offsets[i], sizes[i], time);
	while (decoder->outputCount > queued + 1) {
		decoder->surfaces[decoder->output[queued].surface].queued--;
		memmove(decoder->output + queued, decoder->output + queued + 1,
			(size_t)(decoder->outputCount - queued - 1) * sizeof(Output));
		decoder->outputCount--;
	}
	return result;
}
