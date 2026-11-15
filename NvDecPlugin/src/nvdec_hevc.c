/* See nvdec_hevc.h. Section numbers are from ITU-T H.265 (09/2023).
 *
 * What the engine is given for a picture follows the two descriptions of it
 * there are outside NVIDIA: the H.265 decoder of Mesa's NVK (a draft, for
 * Turing and later) and averne's NVTEGRA hardware acceleration for FFmpeg
 * (for the Tegra X1's NVDEC). Where they disagree, or where this chip may
 * differ from both, the comment says which was followed.
 */

#include "nvdec_hevc.h"
#include "hevc_parse.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video/nvdec_drv.h"

#define MAX_SURFACES		16
#define MAX_SLICES		1024
#define BITSTREAM_SLACK		256

#define NVC2B0_HEVC_SET_SCALING_LIST_OFFSET	0x0580
#define NVC2B0_HEVC_SET_TILE_SIZES_OFFSET	0x0584
#define NVC2B0_HEVC_SET_FILTER_BUFFER_OFFSET	0x0588

/* Bytes of the filter buffer per line of the picture, for each of the three
 * things kept in it. The Tegra X1 uses 480, 3840 and 60, NVK's Turing traces
 * 624, 4864 and 152; the engine is told where each part starts, so spacing
 * them wider than this chip needs costs memory and nothing else. */
#define FILTER_PER_LINE		640
#define SAO_PER_LINE		5120
#define BSD_PER_LINE		256

/* The engine's table of tile sizes: a pair of column width and row height
 * for every tile, then from this offset the tiles' edges in sixteens of a
 * pixel - which the Tegra driver writes and NVK does not. */
#define TILE_EDGES_OFFSET	0x700

typedef struct {
	bool		inUse;
	int		surface;		/* also its place in the engine's
						   table of pictures */
	int		poc;
	bool		shortTerm;
	bool		longTerm;
	bool		neededForOutput;
	/* Chosen to be shown next by the bumping process (C.5.2.4), and
	 * when, so that the caller gets pictures in that order. */
	bool		bumped;
	uint32_t	bumpOrder;
	int		latency;		/* pictures decoded since */
	bool		heldByCaller;
	uint32_t	sequence;
	int64_t		time;
} Picture;

typedef struct {
	size_t	offset;		/* of the start code */
	size_t	length;		/* from the start code to the end */
	size_t	payload;	/* of the NAL unit header */
	int	type;
} Nal;

/* The layout nvdec_drv.h leaves out: the engine's scaling lists, from the
 * Tegra driver. Every list in raster order. */
typedef struct {
	uint8_t	dc16x16[6];
	uint8_t	dc32x32[2];
	uint8_t	reserved[8];
	uint8_t	list4x4[6][16];
	uint8_t	list8x8[6][64];
	uint8_t	list16x16[6][64];
	uint8_t	list32x32[2][64];
} EngineScalingList;

struct NvdecHevc {
	NvdecEngine	*engine;
	char		*reason;
	size_t		reasonSize;
	char		error[256];

	HevcParamSets	*sets;
	bool		haveFormat;
	int		width, height, cropLeft, cropTop;
	int		codedWidth, codedHeight;
	int		bitDepth;
	int		bytesPerSample;
	int		pitch;
	int		surfaceCount;
	int		dpbCapacity;
	int		reorderLimit;
	int		latencyLimit;		/* 0 for none */
	int		ctbSize;
	int		activeSpsId;
	int		lastSurface;

	NvdecBuffer	lumaPool, chromaPool;
	size_t		lumaSize, chromaSize;
	NvdecBuffer	colocated;
	uint32_t	colocatedSlot;
	NvdecBuffer	filter;
	uint32_t	saoOffset, bsdOffset, filterAboveOffset, saoAboveOffset,
			sliceEdgeOffset;
	NvdecBuffer	pictureSetup;
	NvdecBuffer	tileSizes;
	NvdecBuffer	scalingList;
	NvdecBuffer	status;
	NvdecBuffer	bitstream;
	bool		surfacesReady;

	Picture		dpb[MAX_SURFACES];

	/* Picture order, 8.3.1 */
	int		prevTid0Poc;
	/* The next IRAP picture starts a new coded video sequence, with no
	 * pictures before it to refer to: at the start, after a seek, after an
	 * end of sequence unit. */
	bool		firstPicture;
	/* RASL pictures refer to pictures from before the IRAP picture they
	 * follow; when that IRAP picture began the decoding, they are left
	 * out (8.1.3). */
	bool		skipRasl;
	uint32_t	sequence;
	uint32_t	bumpCounter;

	uint8_t		*rbsp;
	size_t		rbspSize;
	Nal		*nals;
	NvdecStatus	lastStatus;
	uint32_t	pictureCount;
	uint32_t	errorsReported;
};

static bool
setError(NvdecHevc *decoder, const char *format, ...)
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
nvdecHevcLastError(const NvdecHevc *decoder)
{
	return decoder->error;
}

void
nvdecHevcGetStatus(const NvdecHevc *decoder, NvdecStatus *status)
{
	*status = decoder->lastStatus;
}

NvdecHevc *
nvdecHevcCreate(NvdecEngine *engine, char *reason, size_t reasonSize)
{
	NvdecHevc *decoder = calloc(1, sizeof(NvdecHevc));
	if (decoder == NULL)
		return NULL;
	decoder->sets = calloc(1, sizeof(HevcParamSets));
	decoder->nals = calloc(MAX_SLICES, sizeof(Nal));
	if (decoder->sets == NULL || decoder->nals == NULL) {
		free(decoder->sets);
		free(decoder->nals);
		free(decoder);
		return NULL;
	}
	decoder->engine = engine;
	decoder->reason = reason;
	decoder->reasonSize = reasonSize;
	decoder->activeSpsId = -1;
	nvdecHevcReset(decoder);
	return decoder;
}

static void
freeSurfaces(NvdecHevc *decoder)
{
	nvdecFree(decoder->engine, &decoder->lumaPool);
	nvdecFree(decoder->engine, &decoder->chromaPool);
	nvdecFree(decoder->engine, &decoder->colocated);
	nvdecFree(decoder->engine, &decoder->filter);
	nvdecFree(decoder->engine, &decoder->pictureSetup);
	nvdecFree(decoder->engine, &decoder->tileSizes);
	nvdecFree(decoder->engine, &decoder->scalingList);
	nvdecFree(decoder->engine, &decoder->status);
	nvdecFree(decoder->engine, &decoder->bitstream);
	decoder->surfaceCount = 0;
	decoder->surfacesReady = false;
}

void
nvdecHevcDestroy(NvdecHevc *decoder)
{
	if (decoder == NULL)
		return;
	freeSurfaces(decoder);
	free(decoder->rbsp);
	free(decoder->nals);
	free(decoder->sets);
	free(decoder);
}

bool
nvdecHevcHasFormat(const NvdecHevc *decoder, int *width, int *height,
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

/* --------------------------------------------------------------- surfaces */

static uint32_t
alignUp(uint32_t value, uint32_t to)
{
	return (value + to - 1) / to * to;
}

static bool
prepareForSequence(NvdecHevc *decoder, const HevcSps *sps)
{
	int capacity = sps->maxDecPicBuffering;
	if (capacity < 1)
		capacity = 1;
	if (decoder->surfacesReady && decoder->activeSpsId == sps->id
		&& sps->width == decoder->codedWidth
		&& sps->height == decoder->codedHeight
		&& sps->bitDepthLuma == decoder->bitDepth
		&& capacity == decoder->dpbCapacity) {
		return true;
	}
	freeSurfaces(decoder);
	nvdecHevcReset(decoder);

	decoder->activeSpsId = sps->id;
	decoder->codedWidth = sps->width;
	decoder->codedHeight = sps->height;
	decoder->bitDepth = sps->bitDepthLuma;
	decoder->bytesPerSample = sps->bitDepthLuma > 8 ? 2 : 1;
	decoder->ctbSize = 1 << sps->log2CtbSize;
	decoder->cropLeft = sps->confLeft;
	decoder->cropTop = sps->confTop;
	decoder->width = sps->width - sps->confLeft - sps->confRight;
	decoder->height = sps->height - sps->confTop - sps->confBottom;
	decoder->dpbCapacity = capacity;
	decoder->reorderLimit = sps->maxNumReorderPics;
	decoder->latencyLimit = sps->maxLatencyIncreasePlus1 != 0
		? sps->maxNumReorderPics + sps->maxLatencyIncreasePlus1 - 1 : 0;
	/* Every picture the stream may hold, and the one being decoded. */
	decoder->surfaceCount = capacity + 1;
	if (decoder->surfaceCount > MAX_SURFACES)
		decoder->surfaceCount = MAX_SURFACES;

	/* Lines a whole number of 64 byte groups apart, as for H.264, and
	 * planes a whole number of coding tree blocks tall. */
	uint32_t alignedWidth = alignUp((uint32_t)sps->width, 64);
	uint32_t alignedHeight = alignUp((uint32_t)sps->height, 64);
	decoder->pitch = (int)alignUp((uint32_t)sps->width * decoder->bytesPerSample, 64);
	decoder->lumaSize = (size_t)decoder->pitch * alignedHeight;
	decoder->chromaSize = (size_t)decoder->pitch * (alignedHeight / 2);

	bool inSystemMemory = getenv("NVDEC_SURFACES_IN_VRAM") == NULL;
	char why[192];
	snprintf(why, sizeof(why), "%s", decoder->reason != NULL ? decoder->reason : "");
	/* One surface past the pictures stays blank, for the places in the
	 * engine's table that nothing should be read from. */
	if (!nvdecAlloc(decoder->engine, &decoder->lumaPool,
			(size_t)(decoder->surfaceCount + 1) * decoder->lumaSize,
			inSystemMemory)
		|| !nvdecAlloc(decoder->engine, &decoder->chromaPool,
			(size_t)(decoder->surfaceCount + 1) * decoder->chromaSize,
			inSystemMemory)) {
		snprintf(why, sizeof(why), "%s", decoder->reason != NULL ? decoder->reason : "");
		return setError(decoder, "no room for %d decoded pictures of %dx%d: %s",
			decoder->surfaceCount, sps->width, sps->height, why);
	}

	/* Colocated motion vectors: one slot a picture, sixteen bytes for every
	 * sixteen by sixteen block, found by the engine from the picture's index.
	 * Seventeen slots, as both other drivers have, whatever the stream. */
	decoder->colocatedSlot = alignUp(alignedWidth * alignedHeight / 16, 256);
	if (!nvdecAlloc(decoder->engine, &decoder->colocated,
			(size_t)17 * decoder->colocatedSlot, false)) {
		return setError(decoder, "no room for the motion data");
	}

	/* The filter buffer: three regions with a part for every line, then
	 * the two kept for the line above (offsets this chip may or may not
	 * read; nvdec_drv.h has them from the GP100 on), then the slice edge
	 * one of later engines. Units of 256 bytes. */
	uint32_t lines = alignedHeight;
	uint32_t at = FILTER_PER_LINE * lines;
	decoder->saoOffset = at;
	at += SAO_PER_LINE * lines;
	decoder->bsdOffset = at;
	at += BSD_PER_LINE * lines;
	uint32_t aboveSize = alignUp(alignedWidth * 64 + (1u << 20), 256);
	decoder->filterAboveOffset = at;
	at += aboveSize;
	decoder->saoAboveOffset = at;
	at += aboveSize;
	decoder->sliceEdgeOffset = at;
	at += aboveSize;
	if (!nvdecAlloc(decoder->engine, &decoder->filter, at, false))
		return setError(decoder, "no room for the filter buffer");

	if (!nvdecAlloc(decoder->engine, &decoder->pictureSetup,
			sizeof(nvdec_hevc_pic_s) + 0x100, true)
		|| !nvdecAlloc(decoder->engine, &decoder->tileSizes, 0x1000, true)
		|| !nvdecAlloc(decoder->engine, &decoder->scalingList, 0x1000, true)
		/* NVK finds a small status buffer makes HEVC pictures fail. */
		|| !nvdecAlloc(decoder->engine, &decoder->status, 0x10000, true)) {
		return setError(decoder, "no room for the decoder's working buffers");
	}
	decoder->haveFormat = true;
	decoder->surfacesReady = true;
	return true;
}

static bool
growBitstream(NvdecHevc *decoder, size_t needed)
{
	needed = (needed + 0xffff) & ~(size_t)0xffff;
	if (decoder->bitstream.size >= needed)
		return true;
	nvdecFree(decoder->engine, &decoder->bitstream);
	if (!nvdecAlloc(decoder->engine, &decoder->bitstream, needed, true))
		return setError(decoder, "no room for %zu bytes of bitstream", needed);
	return true;
}

static void
releasePicture(Picture *picture)
{
	memset(picture, 0, sizeof(*picture));
}

static void
dropUnneeded(NvdecHevc *decoder)
{
	for (int i = 0; i < MAX_SURFACES; i++) {
		Picture *picture = &decoder->dpb[i];
		if (picture->inUse && !picture->shortTerm && !picture->longTerm
			&& !picture->neededForOutput && !picture->heldByCaller) {
			releasePicture(picture);
		}
	}
}

/* A surface - and so a place in the engine's table - no picture has: the one
 * that has been free longest, as for H.264. */
static int
takeSurface(NvdecHevc *decoder)
{
	bool busy[MAX_SURFACES] = { false };
	for (int i = 0; i < MAX_SURFACES; i++) {
		if (decoder->dpb[i].inUse)
			busy[decoder->dpb[i].surface] = true;
	}
	for (int i = 0; i < decoder->surfaceCount; i++) {
		int at = (decoder->lastSurface + 1 + i) % decoder->surfaceCount;
		if (!busy[at]) {
			decoder->lastSurface = at;
			return at;
		}
	}
	return -1;
}

static Picture *
freeEntry(NvdecHevc *decoder)
{
	for (int i = 0; i < MAX_SURFACES; i++) {
		if (!decoder->dpb[i].inUse)
			return &decoder->dpb[i];
	}
	return NULL;
}

/* ------------------------------------------------------------ showing them */

/* C.5.2.4: the picture to be shown next is the one first in display order of
 * those waiting. It stays where it is until the caller takes it - before the
 * next access unit, which is what the decoder's surfaces are counted for. */
static bool
bumpOne(NvdecHevc *decoder)
{
	Picture *best = NULL;
	for (int i = 0; i < MAX_SURFACES; i++) {
		Picture *picture = &decoder->dpb[i];
		if (!picture->inUse || !picture->neededForOutput || picture->bumped)
			continue;
		if (best == NULL || picture->sequence < best->sequence
			|| (picture->sequence == best->sequence && picture->poc < best->poc)) {
			best = picture;
		}
	}
	if (best == NULL)
		return false;
	best->bumped = true;
	best->bumpOrder = decoder->bumpCounter++;
	return true;
}

/* How many pictures wait to be chosen, and how full the buffer is: chosen
 * pictures that are not references are as good as gone. */
static void
countPictures(NvdecHevc *decoder, int *waiting, int *fullness, bool *overdue)
{
	*waiting = 0;
	*fullness = 0;
	*overdue = false;
	for (int i = 0; i < MAX_SURFACES; i++) {
		Picture *picture = &decoder->dpb[i];
		if (!picture->inUse)
			continue;
		bool reference = picture->shortTerm || picture->longTerm;
		bool waits = picture->neededForOutput && !picture->bumped;
		if (waits || reference)
			(*fullness)++;
		if (waits) {
			(*waiting)++;
			if (decoder->latencyLimit > 0
				&& picture->latency >= decoder->latencyLimit) {
				*overdue = true;
			}
		}
	}
}

bool
nvdecHevcNextFrame(NvdecHevc *decoder, NvdecFrame *frame)
{
	Picture *picture = NULL;
	for (int i = 0; i < MAX_SURFACES; i++) {
		Picture *candidate = &decoder->dpb[i];
		if (!candidate->inUse || !candidate->neededForOutput || !candidate->bumped)
			continue;
		if (picture == NULL || candidate->bumpOrder < picture->bumpOrder)
			picture = candidate;
	}
	if (picture == NULL)
		return false;

	frame->width = decoder->width;
	frame->height = decoder->height;
	frame->codedWidth = decoder->codedWidth;
	frame->codedHeight = decoder->codedHeight;
	frame->cropLeft = decoder->cropLeft;
	frame->cropTop = decoder->cropTop;
	frame->pitch = decoder->pitch;
	frame->luma = (const uint8_t *)decoder->lumaPool.data
		+ (size_t)picture->surface * decoder->lumaSize;
	frame->chroma = (const uint8_t *)decoder->chromaPool.data
		+ (size_t)picture->surface * decoder->chromaSize;
	frame->bitDepth = decoder->bitDepth;
	frame->pictureOrder = picture->poc;
	frame->time = picture->time;
	frame->handle = (int)(picture - decoder->dpb);
	picture->neededForOutput = false;
	picture->heldByCaller = true;
	return true;
}

void
nvdecHevcReleaseFrame(NvdecHevc *decoder, const NvdecFrame *frame)
{
	if (frame->handle < 0 || frame->handle >= MAX_SURFACES)
		return;
	Picture *picture = &decoder->dpb[frame->handle];
	if (!picture->heldByCaller)
		return;
	picture->heldByCaller = false;
	dropUnneeded(decoder);
}

void
nvdecHevcDrainAll(NvdecHevc *decoder)
{
	for (int i = 0; i < MAX_SURFACES; i++) {
		decoder->dpb[i].shortTerm = false;
		decoder->dpb[i].longTerm = false;
	}
	while (bumpOne(decoder))
		;
}

void
nvdecHevcReset(NvdecHevc *decoder)
{
	for (int i = 0; i < MAX_SURFACES; i++)
		releasePicture(&decoder->dpb[i]);
	decoder->prevTid0Poc = 0;
	decoder->lastSurface = -1;
	decoder->firstPicture = true;
	decoder->skipRasl = false;
}

/* --------------------------------------------------------------- decoding */

static size_t
findNals(const uint8_t *data, size_t size, Nal *nals, size_t maxNals)
{
	size_t count = 0;
	size_t i = 0;
	while (i + 3 <= size && count < maxNals) {
		if (!(data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) {
			i++;
			continue;
		}
		size_t payload = i + 3;
		size_t j = payload;
		while (j + 3 <= size && !(data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1))
			j++;
		size_t end = (j + 3 <= size) ? j : size;
		while (end > payload && data[end - 1] == 0)
			end--;
		if (end >= payload + 2) {
			nals[count].offset = i;
			nals[count].length = end - i;
			nals[count].payload = payload;
			nals[count].type = (data[payload] >> 1) & 0x3f;
			count++;
		}
		i = (j + 3 <= size) ? j : size;
	}
	return count;
}

static bool
ensureRbsp(NvdecHevc *decoder, size_t size)
{
	if (decoder->rbspSize >= size)
		return true;
	uint8_t *grown = realloc(decoder->rbsp, size);
	if (grown == NULL)
		return false;
	decoder->rbsp = grown;
	decoder->rbspSize = size;
	return true;
}

/* 8.3.1 */
static int
pictureOrderCount(NvdecHevc *decoder, const HevcSps *sps, const HevcSlice *slice,
	bool noRaslOutput)
{
	int maxLsb = 1 << sps->log2MaxPocLsb;
	int msb;
	if (hevcIsIrap(slice->nalType) && noRaslOutput) {
		msb = 0;
	} else {
		int prevLsb = decoder->prevTid0Poc & (maxLsb - 1);
		int prevMsb = decoder->prevTid0Poc - prevLsb;
		if (slice->pocLsb < prevLsb && prevLsb - slice->pocLsb >= maxLsb / 2)
			msb = prevMsb + maxLsb;
		else if (slice->pocLsb > prevLsb && slice->pocLsb - prevLsb > maxLsb / 2)
			msb = prevMsb - maxLsb;
		else
			msb = prevMsb;
	}
	return msb + slice->pocLsb;
}

static Picture *
findByPoc(NvdecHevc *decoder, int poc, int mask, bool shortTermOnly)
{
	for (int i = 0; i < MAX_SURFACES; i++) {
		Picture *picture = &decoder->dpb[i];
		if (!picture->inUse || (!picture->shortTerm && !picture->longTerm))
			continue;
		if (shortTermOnly && !picture->shortTerm)
			continue;
		if ((picture->poc & mask) == poc)
			return picture;
	}
	return NULL;
}

typedef struct {
	Picture	*stCurrBefore[HEVC_MAX_DELTA_POCS];
	Picture	*stCurrAfter[HEVC_MAX_DELTA_POCS];
	Picture	*ltCurr[HEVC_MAX_LONG_TERM];
	int	numStCurrBefore, numStCurrAfter, numLtCurr;
} ReferenceSets;

/* 8.3.2: which pictures the current one refers to, and which of the rest
 * stop being references. */
static void
applyReferenceSets(NvdecHevc *decoder, const HevcSps *sps, const HevcSlice *slice,
	int poc, ReferenceSets *sets)
{
	memset(sets, 0, sizeof(*sets));
	bool keep[MAX_SURFACES] = { false };

	if (hevcIsIdr(slice->nalType)) {
		for (int i = 0; i < MAX_SURFACES; i++) {
			decoder->dpb[i].shortTerm = false;
			decoder->dpb[i].longTerm = false;
		}
		dropUnneeded(decoder);
		return;
	}

	int maxLsb = 1 << sps->log2MaxPocLsb;

	/* Long-term references first: they are found by their picture order,
	 * or just its low bits, and a short-term picture can become one. */
	for (int i = 0; i < slice->numLongTerm; i++) {
		int pocLt = slice->longTermPocLsb[i];
		int mask = maxLsb - 1;
		if (slice->longTermMsbPresent[i]) {
			pocLt += poc - slice->longTermMsbCycle[i] * maxLsb
				- (poc & (maxLsb - 1));
			mask = -1;
		}
		Picture *picture = findByPoc(decoder, pocLt, mask, false);
		if (picture != NULL) {
			keep[picture - decoder->dpb] = true;
			picture->longTerm = true;
			picture->shortTerm = false;
		}
		if (slice->longTermUsed[i] && sets->numLtCurr < HEVC_MAX_LONG_TERM)
			sets->ltCurr[sets->numLtCurr++] = picture;
	}

	const HevcShortTermSet *set = &slice->shortTerm;
	for (int i = 0; i < set->numNegative; i++) {
		Picture *picture = findByPoc(decoder, poc + set->deltaPoc[0][i], -1, true);
		if (picture != NULL)
			keep[picture - decoder->dpb] = true;
		if (set->used[0][i])
			sets->stCurrBefore[sets->numStCurrBefore++] = picture;
	}
	for (int i = 0; i < set->numPositive; i++) {
		Picture *picture = findByPoc(decoder, poc + set->deltaPoc[1][i], -1, true);
		if (picture != NULL)
			keep[picture - decoder->dpb] = true;
		if (set->used[1][i])
			sets->stCurrAfter[sets->numStCurrAfter++] = picture;
	}

	for (int i = 0; i < MAX_SURFACES; i++) {
		if (!keep[i]) {
			decoder->dpb[i].shortTerm = false;
			decoder->dpb[i].longTerm = false;
		}
	}
	dropUnneeded(decoder);
}

static void
fillScalingList(const HevcSps *sps, const HevcPps *pps, EngineScalingList *out)
{
	const HevcScalingList *lists = pps->scalingListPresent
		? &pps->scaling : &sps->scaling;
	memset(out, 0, sizeof(*out));
	memcpy(out->dc16x16, lists->dc16x16, 6);
	out->dc32x32[0] = lists->dc32x32[0];
	out->dc32x32[1] = lists->dc32x32[3];
	/* Column by column, as NVK sends them: the Tegra driver's raster order
	 * only works for lists that are symmetric, as the default ones are. */
	for (int m = 0; m < 6; m++) {
		for (int y = 0; y < 4; y++) {
			for (int x = 0; x < 4; x++)
				out->list4x4[m][x * 4 + y] = lists->list4x4[m][y * 4 + x];
		}
		for (int y = 0; y < 8; y++) {
			for (int x = 0; x < 8; x++) {
				out->list8x8[m][x * 8 + y] = lists->list8x8[m][y * 8 + x];
				out->list16x16[m][x * 8 + y] = lists->list16x16[m][y * 8 + x];
				if (m == 0 || m == 3) {
					out->list32x32[m / 3][x * 8 + y]
						= lists->list32x32[m][y * 8 + x];
				}
			}
		}
	}
}

static void
fillTileSizes(const HevcSps *sps, const HevcPps *pps, uint8_t *buffer)
{
	memset(buffer, 0, 0x1000);
	uint16_t *sizes = (uint16_t *)buffer;
	int columns[HEVC_MAX_TILE_COLUMNS], rows[HEVC_MAX_TILE_ROWS];
	hevcTileSizes(sps, pps, columns, rows);
	for (int r = 0; r < pps->numTileRows; r++) {
		for (int c = 0; c < pps->numTileColumns; c++) {
			*sizes++ = (uint16_t)columns[c];
			*sizes++ = (uint16_t)rows[r];
		}
	}
	if (!pps->tilesEnabled)
		return;
	uint16_t *edges = (uint16_t *)(buffer + TILE_EDGES_OFFSET);
	int shift = sps->log2CtbSize - 4;
	int sum = 0;
	for (int c = 0; c < pps->numTileColumns; c++) {
		sum += columns[c];
		*edges++ = (uint16_t)(sum << shift);
	}
	sum = 0;
	for (int r = 0; r < pps->numTileRows; r++) {
		sum += rows[r];
		*edges++ = (uint16_t)(sum << shift);
	}
}

static int8_t
clipDiff(int value)
{
	return (int8_t)(value < -128 ? -128 : (value > 127 ? 127 : value));
}

static void
traceSkipped(const HevcSlice *slice, const char *why)
{
	if (getenv("NVDEC_TRACE") == NULL)
		return;
	fprintf(stderr, "  -: nal %2d poc lsb %4d not decoded: %s\n", slice->nalType,
		slice->pocLsb, why);
}

/* Decodes the picture of one access unit. An end of sequence unit after the
 * picture's slices ends the sequence after it, so it is only reported in
 * *endOfSequence: the picture before it is still decoded and shown. */
static bool
decodeAccessUnit(NvdecHevc *decoder, const uint8_t *data, size_t size,
	int64_t time, bool *endOfSequence)
{
	Nal *nals = decoder->nals;
	size_t nalCount = findNals(data, size, nals, MAX_SLICES);
	if (nalCount == 0)
		return setError(decoder, "no NAL units in %zu bytes", size);
	if (!ensureRbsp(decoder, size + 16))
		return setError(decoder, "out of memory");

	HevcSlice slice;
	bool haveSlice = false;
	size_t firstSlice = 0, endSlice = nalCount;
	for (size_t i = 0; i < nalCount; i++) {
		int type = nals[i].type;
		size_t payloadLength = nals[i].offset + nals[i].length - nals[i].payload;
		if (type == HEVC_NAL_SPS || type == HEVC_NAL_PPS) {
			size_t length = h264ToRbsp(data + nals[i].payload, payloadLength,
				decoder->rbsp);
			if (type == HEVC_NAL_SPS) {
				HevcSps *sps = malloc(sizeof(HevcSps));
				if (sps != NULL && hevcParseSps(decoder->rbsp, length, sps))
					decoder->sets->sps[sps->id] = *sps;
				free(sps);
			} else {
				HevcPps pps;
				if (hevcParsePps(decoder->rbsp, length, decoder->sets, &pps))
					decoder->sets->pps[pps.id] = pps;
			}
		} else if (type == HEVC_NAL_EOS || type == HEVC_NAL_EOB) {
			/* After this, the next picture starts a coded video sequence
			 * (7.4.3.6): a CRA picture after it is handled as one that
			 * begins the decoding (8.1.3), its RASL pictures left out. */
			if (haveSlice)
				*endOfSequence = true;
			else
				decoder->firstPicture = true;
		} else if (hevcIsVcl(type) && type <= HEVC_NAL_CRA
			&& (type <= 9 || type >= 16)) {
			/* The first slice segment says what the picture is; a
			 * second picture in the same chunk is not decoded. */
			size_t length = h264ToRbsp(data + nals[i].payload,
				payloadLength < 128 ? payloadLength : 128, decoder->rbsp);
			bool first = (decoder->rbsp[2] & 0x80) != 0;
			if (haveSlice) {
				if (first) {
					endSlice = i;
					break;
				}
				continue;
			}
			if (!first)
				continue;
			size_t full = h264ToRbsp(data + nals[i].payload, payloadLength,
				decoder->rbsp);
			(void)length;
			if (!hevcParseSliceHeader(decoder->rbsp, full, decoder->sets, &slice))
				return setError(decoder, "cannot read this slice header");
			haveSlice = true;
			firstSlice = i;
		}
	}
	if (!haveSlice)
		return true;

	const HevcPps *pps = &decoder->sets->pps[slice.ppsId];
	const HevcSps *sps = &decoder->sets->sps[pps->spsId];
	if (sps->chromaFormatIdc != 1 || sps->separateColourPlane)
		return setError(decoder, "only 4:2:0 is supported");
	if (sps->bitDepthLuma != sps->bitDepthChroma
		|| (sps->bitDepthLuma != 8 && sps->bitDepthLuma != 10)) {
		return setError(decoder, "only eight and ten bits a sample are supported");
	}
	if (sps->width > 8192 || sps->height > 8192)
		return setError(decoder, "%dx%d is larger than the engine decodes",
			sps->width, sps->height);

	bool irap = hevcIsIrap(slice.nalType);
	if (decoder->firstPicture && !irap) {
		/* Nothing to refer to until the stream gives a starting point. */
		traceSkipped(&slice, "before the first random access point");
		return true;
	}
	if (hevcIsRasl(slice.nalType) && decoder->skipRasl) {
		/* A leading picture of the IRAP picture decoding began at (8.1.3):
		 * it refers to pictures from before that, which this decoder never
		 * had. Neither decoded nor shown, as libavcodec does it. */
		traceSkipped(&slice, "RASL of the starting picture");
		return true;
	}

	/* This starts over - the pictures gone, firstPicture set - when the
	 * parameters of the sequence change, so whether this picture begins
	 * the decoding is only known after it. */
	if (!prepareForSequence(decoder, sps))
		return false;
	if (decoder->firstPicture && !irap) {
		traceSkipped(&slice, "a new sequence without a random access point");
		return true;
	}
	/* NoRaslOutputFlag, 8.1.3: a CRA picture that begins the decoding, or
	 * follows an end of sequence, or begins a sequence with new parameters
	 * (HandleCraAsBlaFlag, as for a splice), is treated like a BLA picture.
	 * Its RASL pictures are left out until the next IRAP picture, after
	 * which those of a CRA picture in the middle of a sequence are shown. */
	bool noRaslOutput = irap && (hevcIsIdr(slice.nalType)
		|| hevcIsBla(slice.nalType) || decoder->firstPicture);
	if (irap)
		decoder->skipRasl = noRaslOutput;

	int poc = pictureOrderCount(decoder, sps, &slice, noRaslOutput);
	if (irap && noRaslOutput) {
		/* A new coded video sequence: what is still to be shown from the
		 * last one goes first, unless the stream says to drop it. A CRA
		 * picture here always does (C.5.2.2, and libavcodec): one after an
		 * end of sequence drops what was not yet chosen to be shown before
		 * it. After a seek or at the start there is nothing to drop. */
		bool dropPrior = slice.noOutputOfPriorPics
			|| slice.nalType == HEVC_NAL_CRA;
		decoder->sequence++;
		for (int i = 0; i < MAX_SURFACES; i++) {
			Picture *picture = &decoder->dpb[i];
			picture->shortTerm = false;
			picture->longTerm = false;
			if (dropPrior && !picture->bumped)
				picture->neededForOutput = false;
		}
		while (bumpOne(decoder))
			;
		dropUnneeded(decoder);
	}
	decoder->firstPicture = false;

	ReferenceSets references;
	applyReferenceSets(decoder, sps, &slice, poc, &references);

	/* C.5.2.2: with the references this picture keeps known, make room
	 * for it, showing pictures while too many wait or the buffer is full. */
	for (;;) {
		int waiting, fullness;
		bool overdue;
		countPictures(decoder, &waiting, &fullness, &overdue);
		if (waiting <= decoder->reorderLimit && !overdue
			&& fullness < decoder->dpbCapacity) {
			break;
		}
		if (!bumpOne(decoder))
			break;
	}

	int surface = takeSurface(decoder);
	Picture *entry = freeEntry(decoder);
	if (surface < 0 || entry == NULL)
		return setError(decoder, "every decoded picture is still in use");

	/* The bitstream: every slice segment with a four-byte start code. */
	size_t needed = size + (endSlice - firstSlice) * 4 + BITSTREAM_SLACK;
	if (!growBitstream(decoder, needed))
		return false;
	uint8_t *bits = decoder->bitstream.data;
	size_t streamLength = 0;
	for (size_t i = firstSlice; i < endSlice; i++) {
		int type = nals[i].type;
		if (!hevcIsVcl(type) || (type > 9 && type < 16) || type > HEVC_NAL_CRA)
			continue;
		size_t unit = nals[i].offset + nals[i].length - nals[i].payload;
		bits[streamLength++] = 0;
		bits[streamLength++] = 0;
		bits[streamLength++] = 0;
		bits[streamLength++] = 1;
		memcpy(bits + streamLength, data + nals[i].payload, unit);
		streamLength += unit;
	}
	memset(bits + streamLength, 0, BITSTREAM_SLACK);

	/* Which place in the engine's table each picture has: its surface. A
	 * place nothing occupies points at a real reference, so that a stray
	 * read finds a picture rather than garbage. */
	int fallbackSurface = surface;
	int fallbackDiff = 0;
	Picture *lists[3] = { NULL, NULL, NULL };
	if (references.numStCurrBefore > 0)
		lists[0] = references.stCurrBefore[0];
	if (references.numStCurrAfter > 0)
		lists[1] = references.stCurrAfter[0];
	if (references.numLtCurr > 0)
		lists[2] = references.ltCurr[0];
	for (int i = 0; i < 3; i++) {
		if (lists[i] != NULL) {
			fallbackSurface = lists[i]->surface;
			fallbackDiff = poc - lists[i]->poc;
			break;
		}
	}

	nvdec_hevc_pic_s *setup = decoder->pictureSetup.data;
	memset(setup, 0, sizeof(*setup));
	setup->stream_len = (unsigned)streamLength;
	setup->gptimer_timeout_value = 0;		/* the firmware's default */
	setup->tileformat = 1;				/* blocks, as for H.264 */
	setup->gob_height = 0;
	setup->sw_start_code_e = 1;
	setup->disp_output_mode = decoder->bitDepth > 8 ? 1 : 0;
	/* In samples rather than bytes when they are two bytes each. */
	setup->framestride[0] = (unsigned)decoder->pitch / decoder->bytesPerSample;
	setup->framestride[1] = (unsigned)decoder->pitch / decoder->bytesPerSample;
	setup->colMvBuffersize = decoder->colocatedSlot >> 8;
	setup->HevcSaoBufferOffset = decoder->saoOffset >> 8;
	setup->HevcBsdCtrlOffset = decoder->bsdOffset >> 8;

	setup->pic_width_in_luma_samples = (unsigned short)sps->width;
	setup->pic_height_in_luma_samples = (unsigned short)sps->height;
	setup->chroma_format_idc = 1;
	setup->bit_depth_luma = (unsigned)sps->bitDepthLuma;
	setup->bit_depth_chroma = (unsigned)sps->bitDepthChroma;
	setup->log2_min_luma_coding_block_size = (unsigned)sps->log2MinCbSize;
	setup->log2_max_luma_coding_block_size = (unsigned)sps->log2CtbSize;
	setup->log2_min_transform_block_size = (unsigned)sps->log2MinTbSize;
	setup->log2_max_transform_block_size = (unsigned)sps->log2MaxTbSize;
	setup->max_transform_hierarchy_depth_inter
		= (unsigned)sps->maxTransformHierarchyDepthInter;
	setup->max_transform_hierarchy_depth_intra
		= (unsigned)sps->maxTransformHierarchyDepthIntra;
	setup->scalingListEnable = (unsigned)sps->scalingListEnabled;
	setup->amp_enable_flag = (unsigned)sps->ampEnabled;
	setup->sample_adaptive_offset_enabled_flag = (unsigned)sps->saoEnabled;
	setup->pcm_enabled_flag = (unsigned)sps->pcmEnabled;
	if (sps->pcmEnabled) {
		setup->pcm_sample_bit_depth_luma = (unsigned)sps->pcmBitDepthLuma;
		setup->pcm_sample_bit_depth_chroma = (unsigned)sps->pcmBitDepthChroma;
		setup->log2_min_pcm_luma_coding_block_size = (unsigned)sps->log2MinPcmCbSize;
		setup->log2_max_pcm_luma_coding_block_size = (unsigned)sps->log2MaxPcmCbSize;
	}
	setup->pcm_loop_filter_disabled_flag = (unsigned)sps->pcmLoopFilterDisabled;
	setup->sps_temporal_mvp_enabled_flag = (unsigned)sps->temporalMvpEnabled;
	setup->strong_intra_smoothing_enabled_flag = (unsigned)sps->strongIntraSmoothing;

	setup->dependent_slice_segments_enabled_flag
		= (unsigned)pps->dependentSliceSegmentsEnabled;
	setup->output_flag_present_flag = (unsigned)pps->outputFlagPresent;
	setup->num_extra_slice_header_bits = (unsigned)pps->numExtraSliceHeaderBits;
	setup->sign_data_hiding_enabled_flag = (unsigned)pps->signDataHiding;
	setup->cabac_init_present_flag = (unsigned)pps->cabacInitPresent;
	setup->num_ref_idx_l0_default_active = (unsigned)pps->numRefIdxL0DefaultActive;
	setup->num_ref_idx_l1_default_active = (unsigned)pps->numRefIdxL1DefaultActive;
	setup->init_qp = (unsigned)(pps->initQpMinus26 + 26
		+ (sps->bitDepthLuma - 8) * 6);
	setup->constrained_intra_pred_flag = (unsigned)pps->constrainedIntraPred;
	setup->transform_skip_enabled_flag = (unsigned)pps->transformSkipEnabled;
	setup->cu_qp_delta_enabled_flag = (unsigned)pps->cuQpDeltaEnabled;
	setup->diff_cu_qp_delta_depth = (unsigned)pps->diffCuQpDeltaDepth;
	setup->pps_cb_qp_offset = (char)pps->cbQpOffset;
	setup->pps_cr_qp_offset = (char)pps->crQpOffset;
	setup->pps_beta_offset = (char)(pps->betaOffsetDiv2 * 2);
	setup->pps_tc_offset = (char)(pps->tcOffsetDiv2 * 2);
	setup->pps_slice_chroma_qp_offsets_present_flag
		= (unsigned)pps->sliceChromaQpOffsetsPresent;
	setup->weighted_pred_flag = (unsigned)pps->weightedPred;
	setup->weighted_bipred_flag = (unsigned)pps->weightedBipred;
	setup->transquant_bypass_enabled_flag = (unsigned)pps->transquantBypassEnabled;
	setup->tiles_enabled_flag = (unsigned)pps->tilesEnabled;
	setup->entropy_coding_sync_enabled_flag = (unsigned)pps->entropyCodingSync;
	if (pps->tilesEnabled) {
		setup->num_tile_columns = (unsigned)pps->numTileColumns;
		setup->num_tile_rows = (unsigned)pps->numTileRows;
		setup->loop_filter_across_tiles_enabled_flag
			= (unsigned)pps->loopFilterAcrossTiles;
	}
	setup->loop_filter_across_slices_enabled_flag = (unsigned)pps->loopFilterAcrossSlices;
	setup->deblocking_filter_control_present_flag
		= (unsigned)pps->deblockingControlPresent;
	setup->deblocking_filter_override_enabled_flag
		= (unsigned)pps->deblockingOverrideEnabled;
	setup->pps_deblocking_filter_disabled_flag = (unsigned)pps->deblockingDisabled;
	setup->lists_modification_present_flag = (unsigned)pps->listsModificationPresent;
	setup->log2_parallel_merge_level = (unsigned)pps->log2ParallelMergeLevel;
	setup->slice_segment_header_extension_present_flag
		= (unsigned)pps->sliceHeaderExtensionPresent;

	setup->num_ref_frames = (unsigned char)hevcNumPicTotalCurr(&slice);
	setup->IDR_picture_flag = hevcIsIdr(slice.nalType) ? 1 : 0;
	setup->RAP_picture_flag = irap ? 1 : 0;
	setup->curr_pic_idx = (unsigned char)surface;
	/* No dithering: both drivers send 2 for 8- and 10-bit output. */
	setup->pattern_id = 2;
	setup->sw_hdr_skip_length = (unsigned short)slice.skipBits;

	/* Picture order differences for every place in the table, and which
	 * places hold long-term pictures. */
	for (int i = 0; i < 16; i++)
		setup->RefDiffPicOrderCnts[i] = clipDiff(fallbackDiff);
	for (int i = 0; i < MAX_SURFACES; i++) {
		Picture *picture = &decoder->dpb[i];
		if (!picture->inUse || (!picture->shortTerm && !picture->longTerm))
			continue;
		setup->RefDiffPicOrderCnts[picture->surface] = clipDiff(poc - picture->poc);
		if (picture->longTerm)
			setup->longtermflag |= (unsigned short)(1u << (15 - picture->surface));
	}
	setup->RefDiffPicOrderCnts[surface] = 0;

	/* The initial reference lists, 8.3.4, each repeated to sixteen entries:
	 * the engine applies any modification the slices make itself. */
	int total = references.numStCurrBefore + references.numStCurrAfter
		+ references.numLtCurr;
	if (total > 0) {
		Picture *order0[3 * 16], *order1[3 * 16];
		int count = 0;
		for (int i = 0; i < references.numStCurrBefore; i++)
			order0[count++] = references.stCurrBefore[i];
		for (int i = 0; i < references.numStCurrAfter; i++)
			order0[count++] = references.stCurrAfter[i];
		for (int i = 0; i < references.numLtCurr; i++)
			order0[count++] = references.ltCurr[i];
		count = 0;
		for (int i = 0; i < references.numStCurrAfter; i++)
			order1[count++] = references.stCurrAfter[i];
		for (int i = 0; i < references.numStCurrBefore; i++)
			order1[count++] = references.stCurrBefore[i];
		for (int i = 0; i < references.numLtCurr; i++)
			order1[count++] = references.ltCurr[i];
		for (int i = 0; i < 16; i++) {
			Picture *a = order0[i % total], *b = order1[i % total];
			setup->initreflistidxl0[i] = (unsigned char)(a != NULL
				? a->surface : fallbackSurface);
			setup->initreflistidxl1[i] = (unsigned char)(b != NULL
				? b->surface : fallbackSurface);
		}
	}

	/* Range extension fields, read by engines from the GP100 on. The
	 * transform skip size is log2 itself, as NVK has it: nvdec_drv.h says
	 * log2 minus two, and every transform-skipped block then comes out
	 * wrong. */
	nvdec_hevc_main10_444_ext_s *ext = &setup->v1.hevc_main10_444_ext;
	ext->transformSkipRotationEnableFlag = (unsigned)sps->transformSkipRotation;
	ext->transformSkipContextEnableFlag = (unsigned)sps->transformSkipContext;
	ext->implicitRdpcmEnableFlag = (unsigned)sps->implicitRdpcm;
	ext->explicitRdpcmEnableFlag = (unsigned)sps->explicitRdpcm;
	ext->extendedPrecisionProcessingFlag = (unsigned)sps->extendedPrecision;
	ext->intraSmoothingDisabledFlag = (unsigned)sps->intraSmoothingDisabled;
	ext->highPrecisionOffsetsEnableFlag = (unsigned)sps->highPrecisionOffsets;
	ext->fastRiceAdaptationEnableFlag = (unsigned)sps->persistentRiceAdaptation;
	ext->cabacBypassAlignmentEnableFlag = (unsigned)sps->cabacBypassAlignment;
	ext->log2MaxTransformSkipSize = (unsigned)(pps->log2MaxTransformSkipSizeMinus2 + 2);
	ext->crossComponentPredictionEnableFlag = (unsigned)pps->crossComponentPrediction;
	ext->chromaQpAdjustmentEnableFlag = (unsigned)pps->chromaQpOffsetListEnabled;
	ext->diffCuChromaQpAdjustmentDepth = (unsigned)pps->diffCuChromaQpOffsetDepth;
	ext->chromaQpAdjustmentTableSize = (unsigned)pps->chromaQpOffsetListLen;
	ext->log2SaoOffsetScaleLuma = (unsigned)pps->log2SaoOffsetScaleLuma;
	ext->log2SaoOffsetScaleChroma = (unsigned)pps->log2SaoOffsetScaleChroma;
	for (int i = 0; i < 6; i++) {
		ext->cb_qp_adjustment[i] = (char)pps->cbQpOffsetList[i];
		ext->cr_qp_adjustment[i] = (char)pps->crQpOffsetList[i];
	}
	ext->HevcFltAboveOffset = decoder->filterAboveOffset >> 8;
	ext->HevcSaoAboveOffset = decoder->saoAboveOffset >> 8;
	setup->v3.HevcSliceEdgeOffset = decoder->sliceEdgeOffset >> 8;

	if (sps->scalingListEnabled) {
		fillScalingList(sps, pps, decoder->scalingList.data);
	} else {
		memset(decoder->scalingList.data, 0, sizeof(EngineScalingList));
	}
	fillTileSizes(sps, pps, decoder->tileSizes.data);
	memset(decoder->status.data, 0, sizeof(nvdec_status_s));

	NvdecEngine *engine = decoder->engine;
	nvdecBegin(engine);
	nvdecMethod(engine, NVC2B0_SET_APPLICATION_ID, NVDEC_CODEC_HEVC);
	nvdecMethod(engine, NVC2B0_SET_WATCHDOG_TIMER, 0);
	nvdecMethod(engine, NVC2B0_SET_CONTROL_PARAMS,
		NVDEC_CODEC_HEVC			/* CODEC_TYPE 3:0 */
		| (1u << 4)				/* GPTIMER_ON */
		| (1u << 6)				/* ERR_CONCEAL_ON */
		| (1u << 13));				/* MBTIMER_ON */
	nvdecMethod(engine, NVC2B0_SET_PICTURE_INDEX, decoder->pictureCount);
	nvdecAddress(engine, NVC2B0_SET_DRV_PIC_SETUP_OFFSET, decoder->pictureSetup.gpuAddress);
	nvdecAddress(engine, NVC2B0_SET_IN_BUF_BASE_OFFSET, decoder->bitstream.gpuAddress);
	nvdecAddress(engine, NVC2B0_SET_NVDEC_STATUS_OFFSET, decoder->status.gpuAddress);
	nvdecAddress(engine, NVC2B0_SET_COLOC_DATA_OFFSET, decoder->colocated.gpuAddress);
	nvdecAddress(engine, NVC2B0_HEVC_SET_FILTER_BUFFER_OFFSET, decoder->filter.gpuAddress);
	nvdecAddress(engine, NVC2B0_HEVC_SET_TILE_SIZES_OFFSET, decoder->tileSizes.gpuAddress);
	nvdecAddress(engine, NVC2B0_HEVC_SET_SCALING_LIST_OFFSET,
		decoder->scalingList.gpuAddress);
	for (int i = 0; i < 17; i++) {
		int from = i < decoder->surfaceCount ? i : decoder->surfaceCount;
		bool occupied = false;
		for (int j = 0; j < MAX_SURFACES; j++) {
			if (decoder->dpb[j].inUse && decoder->dpb[j].surface == i)
				occupied = true;
		}
		if (!occupied && i != surface)
			from = fallbackSurface;
		nvdecAddress(engine, NVC2B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i,
			decoder->lumaPool.gpuAddress + (size_t)from * decoder->lumaSize);
		nvdecAddress(engine, NVC2B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i,
			decoder->chromaPool.gpuAddress + (size_t)from * decoder->chromaSize);
	}
	nvdecMethod(engine, NVC2B0_EXECUTE, 1u << 0);

	if (!nvdecExecute(engine, 2000))
		return setError(decoder, "the decoder did not answer");

	const nvdec_status_s *status = decoder->status.data;
	decoder->lastStatus.macroblocksDecoded = status->mbs_correctly_decoded;
	decoder->lastStatus.macroblocksInError = status->mbs_in_error;
	decoder->lastStatus.errorStatus = status->error_status;
	decoder->lastStatus.sliceHeaderError = status->slice_header_error_code;
	decoder->lastStatus.cycles = status->cycle_count;
	bool failed = status->error_status != 0;
	if (failed && decoder->errorsReported < 20) {
		decoder->errorsReported++;
		fprintf(stderr, "nvdec hevc: picture %u (poc %d) reported %#x, slice "
			"header %#x, %u decoded\n", decoder->pictureCount, poc,
			status->error_status, status->slice_header_error_code,
			status->mbs_correctly_decoded);
	}

	/* Recorded even when the engine reported an error: later pictures refer
	 * to it, and a concealed picture is better than a hole in the order. */
	memset(entry, 0, sizeof(*entry));
	entry->inUse = true;
	entry->surface = surface;
	entry->poc = poc;
	entry->shortTerm = true;
	entry->neededForOutput = slice.picOutputFlag != 0;
	entry->sequence = decoder->sequence;
	entry->time = time;

	/* C.5.2.3: the pictures waiting have waited one more, and with this one
	 * among them, as many may now be shown as exceed the reordering the
	 * stream allows. */
	if (entry->neededForOutput) {
		for (int i = 0; i < MAX_SURFACES; i++) {
			Picture *picture = &decoder->dpb[i];
			if (picture->inUse && picture != entry && picture->neededForOutput
				&& !picture->bumped) {
				picture->latency++;
			}
		}
		for (;;) {
			int waiting, fullness;
			bool overdue;
			countPictures(decoder, &waiting, &fullness, &overdue);
			if (waiting <= decoder->reorderLimit && !overdue)
				break;
			if (!bumpOne(decoder))
				break;
		}
	}

	if (slice.temporalId == 0 && !hevcIsRasl(slice.nalType)
		&& !hevcIsRadl(slice.nalType)
		&& !hevcIsSubLayerNonReference(slice.nalType)) {
		decoder->prevTid0Poc = poc;
	}

	if (getenv("NVDEC_TRACE") != NULL) {
		static const char *kTypes[] = { "B", "P", "I" };
		fprintf(stderr, "%3u: %s nal %2d poc %4d surface %2d skip %3d refs %d/%d/%d "
			"status %#x ->", decoder->pictureCount, kTypes[slice.sliceType],
			slice.nalType, poc, surface, slice.skipBits,
			references.numStCurrBefore, references.numStCurrAfter,
			references.numLtCurr, status->error_status);
		for (int i = 0; i < MAX_SURFACES; i++) {
			Picture *other = &decoder->dpb[i];
			if (!other->inUse)
				continue;
			fprintf(stderr, " [%d:poc%d%s%s]", other->surface, other->poc,
				other->shortTerm ? ":short" : (other->longTerm ? ":long" : ""),
				other->neededForOutput ? ":out" : "");
		}
		fprintf(stderr, "\n");
	}
	decoder->pictureCount++;
	if (failed) {
		setError(decoder, "the decoder reported %#x on picture %u",
			status->error_status, decoder->pictureCount - 1);
	}
	return true;
}


bool
nvdecHevcDecode(NvdecHevc *decoder, const uint8_t *data, size_t size, int64_t time)
{
	bool endOfSequence = false;
	bool result = decodeAccessUnit(decoder, data, size, time, &endOfSequence);
	if (endOfSequence)
		decoder->firstPicture = true;
	return result;
}
