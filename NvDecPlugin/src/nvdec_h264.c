/* See nvdec_h264.h. Section numbers are from ITU-T H.264 (08/2021). */

#include "nvdec_h264.h"
#include "h264_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "video/nvdec_drv.h"

#define MAX_SURFACES		17
#define MAX_SLICES		4096
/* The engine reads past the last slice, and stops partway down the picture
 * with "out of data" if the length it is given ends exactly at the last byte.
 * It appends this end of stream marker itself, from the picture setup, as long
 * as the length it is given counts it. */
static const uint8_t kEndOfStream[16] = {
	0x00, 0x00, 0x01, 0x0b, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x01, 0x0b, 0x00, 0x00, 0x00, 0x00
};
#define BITSTREAM_SLACK		256

typedef struct {
	bool		inUse;
	int		surface;
	int		frameNum;
	int		frameNumWrap;
	int		picNum;
	int		longTermFrameIdx;
	int		longTermPicNum;
	bool		shortTerm;
	bool		longTerm;
	bool		neededForOutput;
	bool		heldByCaller;
	/* Where this picture sits in the engine's reference table. It stays
	 * put for as long as the picture is a reference, which is what the
	 * reference driver does; moving it about upsets temporal prediction. */
	int		tableSlot;
	int		topPoc, bottomPoc, poc;
	/* Picture order restarts at every IDR, so which coded sequence a
	 * picture belongs to comes before its order within one. */
	uint32_t	sequence;
	int64_t		time;
} FrameStore;

/* One NAL unit of the chunk being decoded: where it starts (at its start
 * code), how long it is, and where its payload starts after the header. */
typedef struct {
	size_t	offset;
	size_t	length;
	int	type;
	int	refIdc;
	size_t	payload;
} Nal;

struct NvdecH264 {
	NvdecEngine	*engine;
	char		*reason;
	size_t		reasonSize;
	char		error[256];

	H264ParamSets	sets;
	bool		haveFormat;
	int		activeSps, activePps;
	int		mbWidth, mbHeight;
	int		codedWidth, codedHeight;
	int		width, height, cropLeft, cropTop;
	int		pitch;
	int		surfaceCount;
	int		lastSurface;
	int		dpbSize;

	/* Every decoded picture lives in one array, because the engine works
	 * out where a picture with a given index is rather than reading the
	 * address it was given for it. */
	NvdecBuffer	lumaPool;
	NvdecBuffer	chromaPool;
	/* One buffer for every picture's colocated motion data: the engine is
	 * given its base and finds each picture's slot from the index in the
	 * picture setup, so the slots cannot be separate allocations. */
	NvdecBuffer	colocated;
	size_t		lumaSize, chromaSize;
	bool		surfaceBusy[MAX_SURFACES];

	NvdecBuffer	pictureSetup;
	NvdecBuffer	bitstream;
	NvdecBuffer	sliceOffsets;
	NvdecBuffer	history;
	NvdecBuffer	macroblockHistory;
	NvdecBuffer	scratch;

	FrameStore	dpb[MAX_SURFACES];
	int		dpbCount;

	/* Picture order state, 8.2.1 */
	int		prevPocMsb, prevPocLsb;
	int		prevFrameNum, prevFrameNumOffset;
	bool		prevHadMmco5;

	uint8_t		*rbsp;
	size_t		rbspSize;

	/* The NAL units of the chunk being decoded. Each decoder has its own:
	 * this was once one table for every decoder in the process, so two
	 * videos playing at once - two tracks, two threads - rewrote each
	 * other's units mid-decode, and the slice copy then read past a chunk
	 * with another chunk's offsets and crashed the process. */
	Nal		*nals;
	NvdecFrame	current;
	NvdecStatus	status;
	uint32_t	pictureCount;
	uint32_t	sequence;
	int		reorderLimit;
	uint32_t	historySize;
	uint32_t	macroblockHistorySize;
	bool		surfacesReady;
	bool		draining;
};

static bool
setError(NvdecH264 *decoder, const char *format, ...)
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
nvdecH264LastError(const NvdecH264 *decoder)
{
	return decoder->error;
}

void
nvdecH264GetStatus(const NvdecH264 *decoder, NvdecStatus *status)
{
	*status = decoder->status;
}

NvdecH264 *
nvdecH264Create(NvdecEngine *engine, char *reason, size_t reasonSize)
{
	NvdecH264 *decoder = calloc(1, sizeof(NvdecH264));
	if (decoder == NULL)
		return NULL;
	decoder->nals = calloc(MAX_SLICES, sizeof(Nal));
	if (decoder->nals == NULL) {
		free(decoder);
		return NULL;
	}
	decoder->engine = engine;
	decoder->reason = reason;
	decoder->reasonSize = reasonSize;
	return decoder;
}

static void
freeSurfaces(NvdecH264 *decoder)
{
	nvdecFree(decoder->engine, &decoder->lumaPool);
	nvdecFree(decoder->engine, &decoder->chromaPool);
	nvdecFree(decoder->engine, &decoder->colocated);
	decoder->surfaceCount = 0;
	decoder->surfacesReady = false;
	nvdecFree(decoder->engine, &decoder->pictureSetup);
	nvdecFree(decoder->engine, &decoder->bitstream);
	nvdecFree(decoder->engine, &decoder->sliceOffsets);
	nvdecFree(decoder->engine, &decoder->history);
	nvdecFree(decoder->engine, &decoder->macroblockHistory);
	nvdecFree(decoder->engine, &decoder->scratch);
}

void
nvdecH264Destroy(NvdecH264 *decoder)
{
	if (decoder == NULL)
		return;
	freeSurfaces(decoder);
	free(decoder->rbsp);
	free(decoder->nals);
	free(decoder);
}

bool
nvdecH264HasFormat(const NvdecH264 *decoder, int *width, int *height)
{
	if (!decoder->haveFormat)
		return false;
	if (width != NULL)
		*width = decoder->width;
	if (height != NULL)
		*height = decoder->height;
	return true;
}

/* --------------------------------------------------------------- surfaces */

static bool
prepareForSequence(NvdecH264 *decoder, const H264Sps *sps)
{
	int mbWidth = sps->picWidthInMbs;
	int mbHeight = sps->picHeightInMapUnits * (sps->frameMbsOnly ? 1 : 2);
	int dpbSize = h264MaxDpbFrames(sps);
	if (decoder->surfacesReady && mbWidth == decoder->mbWidth
		&& mbHeight == decoder->mbHeight && dpbSize == decoder->dpbSize) {
		return true;
	}
	freeSurfaces(decoder);
	nvdecH264Reset(decoder);

	decoder->mbWidth = mbWidth;
	decoder->mbHeight = mbHeight;
	decoder->codedWidth = mbWidth * 16;
	decoder->codedHeight = mbHeight * 16;
	decoder->dpbSize = dpbSize;
	/* One surface for the picture being decoded, and enough for everything
	 * the stream may still need: seventeen with sixteen reference frames,
	 * which is as many as the engine has places for. Sixteen here left
	 * such streams one short, and only their first pictures came out. */
	decoder->surfaceCount = dpbSize + 1;
	if (decoder->surfaceCount > MAX_SURFACES)
		decoder->surfaceCount = MAX_SURFACES;

	int subWidth = (sps->chromaFormatIdc == 3) ? 1 : 2;
	int subHeight = (sps->chromaFormatIdc >= 2) ? 1 : 2;
	(void)subWidth;
	decoder->cropLeft = sps->cropLeft * 2;
	decoder->cropTop = sps->cropTop * (sps->frameMbsOnly ? 2 : 4);
	decoder->width = decoder->codedWidth - (sps->cropLeft + sps->cropRight) * 2;
	decoder->height = decoder->codedHeight
		- (sps->cropTop + sps->cropBottom) * (sps->frameMbsOnly ? 2 : 4);
	if (decoder->width <= 0 || decoder->width > decoder->codedWidth)
		decoder->width = decoder->codedWidth;
	if (decoder->height <= 0 || decoder->height > decoder->codedHeight)
		decoder->height = decoder->codedHeight;

	/* Lines are a whole number of 64 byte groups apart, and a plane holds a
	 * whole number of sixteen-line blocks. */
	decoder->pitch = (decoder->codedWidth + 63) & ~63;
	int lumaLines = (decoder->codedHeight + 15) & ~15;
	int chromaLines = ((decoder->codedHeight / subHeight) + 15) & ~15;
	decoder->lumaSize = (size_t)decoder->pitch * lumaLines;
	decoder->chromaSize = (size_t)decoder->pitch * chromaLines;

	/* The sizes the engine expects for its working buffers. The colocated
	 * motion data is one buffer with a place for every picture, which the
	 * engine addresses itself from the index in the picture setup. */
	int mapUnits = sps->picHeightInMapUnits;
	size_t colocatedSlot = (((size_t)((mapUnits + 1) & ~1) * mbWidth * 64 - 63)
		+ 0xff) & ~(size_t)0xff;
	decoder->macroblockHistorySize = (uint32_t)(((size_t)mbWidth * 104 + 0xff) & ~(size_t)0xff);
	decoder->historySize = (uint32_t)(((size_t)mbWidth * 0x300 + 0x1ff) & ~(size_t)0x1ff);
	size_t mbCount = (size_t)mbWidth * mbHeight;
	(void)mbCount;
	/* One slot past the pictures stays blank, so that a place in the
	 * engine's table that nothing should be read from can point at it. */
	char why[192];
	snprintf(why, sizeof(why), "%s", decoder->reason != NULL ? decoder->reason : "");
	bool inSystemMemory = getenv("NVDEC_SURFACES_IN_VRAM") == NULL;
	if (!nvdecAlloc(decoder->engine, &decoder->lumaPool,
			(size_t)(decoder->surfaceCount + 1) * decoder->lumaSize, inSystemMemory)
		|| !nvdecAlloc(decoder->engine, &decoder->chromaPool,
			(size_t)(decoder->surfaceCount + 1) * decoder->chromaSize, inSystemMemory)) {
		snprintf(why, sizeof(why), "%s", decoder->reason != NULL ? decoder->reason : "");
		return setError(decoder, "no room for %d decoded pictures of %dx%d: %s",
			decoder->surfaceCount, decoder->codedWidth, decoder->codedHeight, why);
	}
	/* The engine's slot size is its own business - it knows the picture
	 * size - so this leaves several times what the motion data can need. */
	if (!nvdecAlloc(decoder->engine, &decoder->colocated,
			(size_t)decoder->surfaceCount * colocatedSlot, false)) {
		return setError(decoder, "no room for the motion data of %d pictures",
			decoder->surfaceCount);
	}
	if (!nvdecAlloc(decoder->engine, &decoder->pictureSetup,
			sizeof(nvdec_h264_pic_s) + 0x100, true)
		|| !nvdecAlloc(decoder->engine, &decoder->sliceOffsets, MAX_SLICES * 4, true)
		|| !nvdecAlloc(decoder->engine, &decoder->history, decoder->historySize, false)
		|| !nvdecAlloc(decoder->engine, &decoder->macroblockHistory,
			decoder->macroblockHistorySize, false)
		|| !nvdecAlloc(decoder->engine, &decoder->scratch, 0x10000, false)) {
		return setError(decoder, "no room for the decoder's working buffers");
	}
	decoder->reorderLimit = sps->hasReorderFrames ? sps->maxNumReorderFrames : dpbSize;
	if (decoder->reorderLimit > dpbSize)
		decoder->reorderLimit = dpbSize;
	decoder->haveFormat = true;
	decoder->surfacesReady = true;
	return true;
}

static bool
growBitstream(NvdecH264 *decoder, size_t needed)
{
	needed = (needed + 0xffff) & ~(size_t)0xffff;
	if (decoder->bitstream.size >= needed)
		return true;
	nvdecFree(decoder->engine, &decoder->bitstream);
	if (!nvdecAlloc(decoder->engine, &decoder->bitstream, needed, true))
		return setError(decoder, "no room for %zu bytes of bitstream", needed);
	return true;
}

/* Take the place that has been free longest, rather than the lowest one.
 * A picture's index is not only where its pixels are: the motion data the
 * engine keeps for temporal prediction is filed under it, and a picture
 * decoded later can still refer to it. Reusing an index as soon as it falls
 * free leaves that data describing the wrong picture. */
static int
takeSurface(NvdecH264 *decoder)
{
	for (int i = 0; i < decoder->surfaceCount; i++) {
		int at = (decoder->lastSurface + 1 + i) % decoder->surfaceCount;
		if (!decoder->surfaceBusy[at]) {
			decoder->lastSurface = at;
			return at;
		}
	}
	return -1;
}

static void
releaseStore(NvdecH264 *decoder, FrameStore *store)
{
	if (!store->inUse)
		return;
	decoder->surfaceBusy[store->surface] = false;
	memset(store, 0, sizeof(*store));
}

/* ----------------------------------------------------------- picture order */

static void
computePictureOrder(NvdecH264 *decoder, const H264Sps *sps, const H264Slice *slice,
	int *topPoc, int *bottomPoc)
{
	int maxFrameNum = 1 << (sps->log2MaxFrameNumMinus4 + 4);

	if (sps->picOrderCntType == 0) {
		int maxPocLsb = 1 << (sps->log2MaxPocLsbMinus4 + 4);
		int prevMsb = decoder->prevPocMsb, prevLsb = decoder->prevPocLsb;
		if (slice->idr) {
			prevMsb = 0;
			prevLsb = 0;
		}
		int msb;
		if (slice->pocLsb < prevLsb && prevLsb - slice->pocLsb >= maxPocLsb / 2)
			msb = prevMsb + maxPocLsb;
		else if (slice->pocLsb > prevLsb && slice->pocLsb - prevLsb > maxPocLsb / 2)
			msb = prevMsb - maxPocLsb;
		else
			msb = prevMsb;
		*topPoc = msb + slice->pocLsb;
		*bottomPoc = *topPoc + slice->deltaPocBottom;
		if (slice->nalRefIdc != 0) {
			decoder->prevPocMsb = msb;
			decoder->prevPocLsb = slice->pocLsb;
		}
		return;
	}

	int frameNumOffset;
	if (slice->idr)
		frameNumOffset = 0;
	else if (decoder->prevFrameNum > slice->frameNum)
		frameNumOffset = decoder->prevFrameNumOffset + maxFrameNum;
	else
		frameNumOffset = decoder->prevFrameNumOffset;

	if (sps->picOrderCntType == 1) {
		int absFrameNum = 0;
		if (sps->numRefFramesInPocCycle != 0)
			absFrameNum = frameNumOffset + slice->frameNum;
		if (slice->nalRefIdc == 0 && absFrameNum > 0)
			absFrameNum--;
		int expected = 0;
		if (absFrameNum > 0) {
			int cycle = (absFrameNum - 1) / sps->numRefFramesInPocCycle;
			int inCycle = (absFrameNum - 1) % sps->numRefFramesInPocCycle;
			int perCycle = 0;
			for (int i = 0; i < sps->numRefFramesInPocCycle; i++)
				perCycle += sps->offsetForRefFrame[i];
			expected = cycle * perCycle;
			for (int i = 0; i <= inCycle; i++)
				expected += sps->offsetForRefFrame[i];
		}
		if (slice->nalRefIdc == 0)
			expected += sps->offsetForNonRefPic;
		*topPoc = expected + slice->deltaPoc[0];
		*bottomPoc = *topPoc + sps->offsetForTopToBottomField + slice->deltaPoc[1];
	} else {
		int value = 2 * (frameNumOffset + slice->frameNum);
		if (slice->nalRefIdc == 0)
			value--;
		*topPoc = value;
		*bottomPoc = value;
	}
	decoder->prevFrameNumOffset = frameNumOffset;
	decoder->prevFrameNum = slice->frameNum;
}

/* ------------------------------------------------------ reference pictures */

static void
updatePicNums(NvdecH264 *decoder, const H264Sps *sps, int currentFrameNum)
{
	int maxFrameNum = 1 << (sps->log2MaxFrameNumMinus4 + 4);
	for (int i = 0; i < MAX_SURFACES; i++) {
		FrameStore *store = &decoder->dpb[i];
		if (!store->inUse)
			continue;
		if (store->shortTerm) {
			store->frameNumWrap = (store->frameNum > currentFrameNum)
				? store->frameNum - maxFrameNum : store->frameNum;
			store->picNum = store->frameNumWrap;
		} else if (store->longTerm) {
			store->longTermPicNum = store->longTermFrameIdx;
		}
	}
}

static FrameStore *
findShortTerm(NvdecH264 *decoder, int picNum)
{
	for (int i = 0; i < MAX_SURFACES; i++) {
		if (decoder->dpb[i].inUse && decoder->dpb[i].shortTerm
			&& decoder->dpb[i].picNum == picNum) {
			return &decoder->dpb[i];
		}
	}
	return NULL;
}

static FrameStore *
findLongTerm(NvdecH264 *decoder, int longTermPicNum)
{
	for (int i = 0; i < MAX_SURFACES; i++) {
		if (decoder->dpb[i].inUse && decoder->dpb[i].longTerm
			&& decoder->dpb[i].longTermPicNum == longTermPicNum) {
			return &decoder->dpb[i];
		}
	}
	return NULL;
}

static void
unmarkReference(FrameStore *store)
{
	store->shortTerm = false;
	store->longTerm = false;
	store->tableSlot = -1;
}

static int
takeTableSlot(NvdecH264 *decoder)
{
	bool used[16] = { false };
	for (int i = 0; i < MAX_SURFACES; i++) {
		FrameStore *store = &decoder->dpb[i];
		if (store->inUse && store->tableSlot >= 0 && store->tableSlot < 16)
			used[store->tableSlot] = true;
	}
	for (int i = 0; i < 16; i++) {
		if (!used[i])
			return i;
	}
	return -1;
}

static void
dropUnneeded(NvdecH264 *decoder)
{
	for (int i = 0; i < MAX_SURFACES; i++) {
		FrameStore *store = &decoder->dpb[i];
		if (store->inUse && !store->shortTerm && !store->longTerm
			&& !store->neededForOutput && !store->heldByCaller) {
			releaseStore(decoder, store);
			decoder->dpbCount--;
		}
	}
}

/* 8.2.5.3: make room by forgetting the oldest short-term reference. */
static void
slidingWindow(NvdecH264 *decoder, const H264Sps *sps)
{
	int shortTerm = 0, longTerm = 0;
	for (int i = 0; i < MAX_SURFACES; i++) {
		if (!decoder->dpb[i].inUse)
			continue;
		if (decoder->dpb[i].shortTerm)
			shortTerm++;
		else if (decoder->dpb[i].longTerm)
			longTerm++;
	}
	int limit = sps->maxNumRefFrames > 0 ? sps->maxNumRefFrames : 1;
	while (shortTerm + longTerm >= limit && shortTerm > 0) {
		FrameStore *oldest = NULL;
		for (int i = 0; i < MAX_SURFACES; i++) {
			FrameStore *store = &decoder->dpb[i];
			if (!store->inUse || !store->shortTerm)
				continue;
			if (oldest == NULL || store->frameNumWrap < oldest->frameNumWrap)
				oldest = store;
		}
		if (oldest == NULL)
			break;
		unmarkReference(oldest);
		shortTerm--;
	}
	dropUnneeded(decoder);
}

/* 8.2.5.4 */
static bool
applyMarking(NvdecH264 *decoder, const H264Sps *sps, const H264Slice *slice,
	int currentFrameNum, bool *currentIsLongTerm, int *currentLongTermIdx,
	bool *sawMmco5)
{
	*currentIsLongTerm = false;
	*currentLongTermIdx = 0;
	*sawMmco5 = false;
	int currPicNum = currentFrameNum;

	for (int i = 0; i < slice->mmcoCount; i++) {
		const H264Mmco *mmco = &slice->mmco[i];
		FrameStore *store;
		switch (mmco->op) {
		case 1:
			store = findShortTerm(decoder,
				currPicNum - (mmco->differenceOfPicNumsMinus1 + 1));
			if (store != NULL)
				unmarkReference(store);
			break;
		case 2:
			store = findLongTerm(decoder, mmco->longTermPicNum);
			if (store != NULL)
				unmarkReference(store);
			break;
		case 3:
			store = findShortTerm(decoder,
				currPicNum - (mmco->differenceOfPicNumsMinus1 + 1));
			if (store != NULL) {
				FrameStore *existing = findLongTerm(decoder, mmco->longTermFrameIdx);
				if (existing != NULL && existing != store)
					unmarkReference(existing);
				store->shortTerm = false;
				store->longTerm = true;
				store->longTermFrameIdx = mmco->longTermFrameIdx;
				store->longTermPicNum = mmco->longTermFrameIdx;
			}
			break;
		case 4:
			for (int j = 0; j < MAX_SURFACES; j++) {
				FrameStore *other = &decoder->dpb[j];
				if (other->inUse && other->longTerm
					&& other->longTermFrameIdx >= mmco->maxLongTermFrameIdxPlus1) {
					unmarkReference(other);
				}
			}
			break;
		case 5:
			for (int j = 0; j < MAX_SURFACES; j++) {
				if (decoder->dpb[j].inUse)
					unmarkReference(&decoder->dpb[j]);
			}
			*sawMmco5 = true;
			break;
		case 6:
		{
			FrameStore *existing = findLongTerm(decoder, mmco->longTermFrameIdx);
			if (existing != NULL)
				unmarkReference(existing);
			*currentIsLongTerm = true;
			*currentLongTermIdx = mmco->longTermFrameIdx;
			break;
		}
		default:
			break;
		}
	}
	dropUnneeded(decoder);
	(void)sps;
	return true;
}

/* ------------------------------------------------------------ showing them */

static FrameStore *
oldestWaiting(NvdecH264 *decoder)
{
	FrameStore *best = NULL;
	for (int i = 0; i < MAX_SURFACES; i++) {
		FrameStore *store = &decoder->dpb[i];
		if (!store->inUse || !store->neededForOutput)
			continue;
		if (best == NULL || store->sequence < best->sequence
			|| (store->sequence == best->sequence && store->poc < best->poc)) {
			best = store;
		}
	}
	return best;
}

static void
fillFrame(NvdecH264 *decoder, const FrameStore *store, NvdecFrame *frame)
{
	const uint8_t *luma = (const uint8_t *)decoder->lumaPool.data
		+ (size_t)store->surface * decoder->lumaSize;
	const uint8_t *chroma = (const uint8_t *)decoder->chromaPool.data
		+ (size_t)store->surface * decoder->chromaSize;
	frame->width = decoder->width;
	frame->height = decoder->height;
	frame->codedWidth = decoder->codedWidth;
	frame->codedHeight = decoder->codedHeight;
	frame->cropLeft = decoder->cropLeft;
	frame->cropTop = decoder->cropTop;
	frame->pitch = decoder->pitch;
	frame->luma = luma;
	frame->chroma = chroma;
	frame->bitDepth = 8;
	frame->pictureOrder = store->poc;
	frame->time = store->time;
}

bool
nvdecH264NextFrame(NvdecH264 *decoder, NvdecFrame *frame)
{
	FrameStore *store = oldestWaiting(decoder);
	if (store == NULL)
		return false;

	if (!decoder->draining) {
		/* Anything left over from before the last IDR has to be shown
		 * before anything after it, whatever the picture order says. */
		bool fromAnEarlierSequence = store->sequence < decoder->sequence;
		if (!fromAnEarlierSequence) {
			int waiting = 0, held = 0;
			for (int i = 0; i < MAX_SURFACES; i++) {
				if (!decoder->dpb[i].inUse)
					continue;
				held++;
				if (decoder->dpb[i].neededForOutput)
					waiting++;
			}
			/* Hold a picture back only as far as the stream may
			 * reorder them, and only while there is room. */
			if (waiting <= decoder->reorderLimit
				&& held < decoder->surfaceCount) {
				return false;
			}
		}
	}

	fillFrame(decoder, store, frame);
	frame->handle = (int)(store - decoder->dpb);
	store->neededForOutput = false;
	store->heldByCaller = true;
	return true;
}

void
nvdecH264ReleaseFrame(NvdecH264 *decoder, const NvdecFrame *frame)
{
	if (frame->handle < 0 || frame->handle >= MAX_SURFACES)
		return;
	FrameStore *store = &decoder->dpb[frame->handle];
	if (!store->heldByCaller)
		return;
	store->heldByCaller = false;
	dropUnneeded(decoder);
}

void
nvdecH264DrainAll(NvdecH264 *decoder)
{
	for (int i = 0; i < MAX_SURFACES; i++) {
		if (decoder->dpb[i].inUse)
			unmarkReference(&decoder->dpb[i]);
	}
	decoder->draining = true;
}

void
nvdecH264Reset(NvdecH264 *decoder)
{
	for (int i = 0; i < MAX_SURFACES; i++)
		memset(&decoder->dpb[i], 0, sizeof(FrameStore));
	memset(decoder->surfaceBusy, 0, sizeof(decoder->surfaceBusy));
	decoder->lastSurface = -1;
	decoder->dpbCount = 0;
	decoder->prevPocMsb = 0;
	decoder->prevPocLsb = 0;
	decoder->prevFrameNum = 0;
	decoder->prevFrameNumOffset = 0;
	decoder->prevHadMmco5 = false;
	decoder->draining = false;
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
		if (payload >= size)
			break;			/* a start code with nothing after it */
		size_t j = payload;
		while (j + 3 <= size && !(data[j] == 0 && data[j + 1] == 0 && data[j + 2] == 1))
			j++;
		size_t end = (j + 3 <= size) ? j : size;
		/* A start code may be preceded by a zero that belongs to it. */
		while (end > payload && data[end - 1] == 0)
			end--;
		if (end <= payload) {
			/* Nothing but zeros: no header byte, so no unit. Counting it
			 * would make its payload length negative. */
			i = (j + 3 <= size) ? j : size;
			continue;
		}
		nals[count].offset = i;
		nals[count].length = end - i;
		nals[count].type = data[payload] & 0x1f;
		nals[count].refIdc = (data[payload] >> 5) & 3;
		nals[count].payload = payload + 1;
		count++;
		i = (j + 3 <= size) ? j : size;
	}
	return count;
}

static bool
ensureRbsp(NvdecH264 *decoder, size_t size)
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

/* ------------------------------------------------- the picture table */

/* The engine finds a reference by the picture index written into its entry,
 * which is also the place in the table of addresses where that picture lives.
 * Where the entry itself sits in the table does not matter - but an entry
 * whose field markings are zero is not a reference at all, and the engine
 * quietly predicts from picture zero instead. */
static void
fillPictureTable(NvdecH264 *decoder, nvdec_h264_pic_s *setup)
{
	for (int i = 0; i < 16; i++) {
		setup->dpb[i].index = 0x7f;
		setup->dpb[i].col_idx = 0x1f;
		setup->dpb[i].state = 0;
		setup->dpb[i].not_existing = 1;
	}
	for (int i = 0; i < MAX_SURFACES; i++) {
		FrameStore *store = &decoder->dpb[i];
		if (!store->inUse || (!store->shortTerm && !store->longTerm))
			continue;
		if (store->tableSlot < 0 || store->tableSlot >= 16)
			continue;
		int marking = store->longTerm ? 2 : 1;
		nvdec_dpb_entry_s *entry = &setup->dpb[store->tableSlot];
		entry->index = store->surface;
		entry->col_idx = store->surface;
		entry->state = 3;			/* a whole frame */
		entry->is_long_term = store->longTerm ? 1 : 0;
		entry->not_existing = 0;
		entry->is_field = 0;
		entry->top_field_marking = marking;
		entry->bottom_field_marking = marking;
		entry->output_memory_layout = 0;
		entry->FieldOrderCnt[0] = store->topPoc;
		entry->FieldOrderCnt[1] = store->bottomPoc;
		entry->FrameIdx = store->longTerm ? store->longTermFrameIdx : store->frameNum;
	}
}

bool
nvdecH264Decode(NvdecH264 *decoder, const uint8_t *data, size_t size, int64_t time)
{
	Nal *nals = decoder->nals;
	size_t nalCount = findNals(data, size, nals, MAX_SLICES);
	if (nalCount == 0)
		return setError(decoder, "no NAL units in %zu bytes", size);
	if (!ensureRbsp(decoder, size + 16))
		return setError(decoder, "out of memory");

	H264Slice slice;
	bool haveSlice = false;
	size_t firstSlice = 0;

	for (size_t i = 0; i < nalCount; i++) {
		size_t payloadLength = nals[i].offset + nals[i].length - nals[i].payload;
		if (nals[i].type == 7 || nals[i].type == 8
			|| ((nals[i].type == 1 || nals[i].type == 5) && !haveSlice)) {
			size_t length = h264ToRbsp(data + nals[i].payload, payloadLength,
				decoder->rbsp);
			if (nals[i].type == 7) {
				H264Sps sps;
				if (h264ParseSps(decoder->rbsp, length, &sps))
					decoder->sets.sps[sps.id] = sps;
			} else if (nals[i].type == 8) {
				H264Pps pps;
				if (h264ParsePps(decoder->rbsp, length, &decoder->sets, &pps))
					decoder->sets.pps[pps.id] = pps;
			} else {
				if (!h264ParseSliceHeader(decoder->rbsp, length, &decoder->sets,
						nals[i].type, nals[i].refIdc, &slice)) {
					return setError(decoder, "cannot read this slice header");
				}
				haveSlice = true;
				firstSlice = i;
			}
		}
	}
	if (!haveSlice) {
		/* Parameter sets on their own are not an error. */
		return true;
	}

	const H264Pps *pps = &decoder->sets.pps[slice.ppsId];
	const H264Sps *sps = &decoder->sets.sps[pps->spsId];
	if (slice.fieldPic)
		return setError(decoder, "field pictures are not supported");
	if (sps->chromaFormatIdc != 1)
		return setError(decoder, "only 4:2:0 is supported");
	if (sps->bitDepthLuma != 8 || sps->bitDepthChroma != 8)
		return setError(decoder, "only eight bits a sample are supported");
	if (!prepareForSequence(decoder, sps))
		return false;

	if (slice.idr) {
		decoder->sequence++;
		decoder->draining = false;
		for (int i = 0; i < MAX_SURFACES; i++)
			unmarkReference(&decoder->dpb[i]);
		if (slice.noOutputOfPriorPics) {
			for (int i = 0; i < MAX_SURFACES; i++) {
				decoder->dpb[i].neededForOutput = false;
				releaseStore(decoder, &decoder->dpb[i]);
			}
			decoder->dpbCount = 0;
		}
		dropUnneeded(decoder);
		decoder->prevFrameNum = 0;
		decoder->prevFrameNumOffset = 0;
	}

	int topPoc = 0, bottomPoc = 0;
	computePictureOrder(decoder, sps, &slice, &topPoc, &bottomPoc);
	updatePicNums(decoder, sps, slice.frameNum);

	/* Make a surface free if every one is spoken for, by letting go of the
	 * oldest picture that is neither a reference nor still to be shown. */
	int surface = takeSurface(decoder);
	if (surface < 0) {
		FrameStore *oldest = NULL;
		for (int i = 0; i < MAX_SURFACES; i++) {
			FrameStore *store = &decoder->dpb[i];
			if (!store->inUse || store->shortTerm || store->longTerm
				|| store->neededForOutput || store->heldByCaller) {
				continue;
			}
			if (oldest == NULL || store->poc < oldest->poc)
				oldest = store;
		}
		if (oldest != NULL) {
			releaseStore(decoder, oldest);
			decoder->dpbCount--;
			surface = takeSurface(decoder);
		}
	}
	if (surface < 0)
		return setError(decoder, "every decoded picture is still in use");

	nvdec_h264_pic_s *setup = decoder->pictureSetup.data;
	memset(setup, 0, sizeof(*setup));

	/* Gather every slice of this picture, with start codes, and say where
	 * each one begins. */
	size_t needed = size + BITSTREAM_SLACK;
	if (!growBitstream(decoder, needed))
		return false;
	uint8_t *bits = decoder->bitstream.data;
	uint32_t *offsets = decoder->sliceOffsets.data;
	size_t streamLength = 0;
	uint32_t sliceCount = 0;
	for (size_t i = firstSlice; i < nalCount && sliceCount < MAX_SLICES; i++) {
		if (nals[i].type != 1 && nals[i].type != 5)
			continue;
		if (nals[i].offset > size || nals[i].length > size - nals[i].offset
			|| streamLength + nals[i].length > size)
			return setError(decoder, "slice %zu runs past the %zu byte chunk",
				i, size);
		offsets[sliceCount++] = (uint32_t)streamLength;
		memcpy(bits + streamLength, data + nals[i].offset, nals[i].length);
		streamLength += nals[i].length;
	}
	memset(bits + streamLength, 0, BITSTREAM_SLACK);
	/* The engine wants one offset more than there are slices: where the last
	 * one ends. */
	offsets[sliceCount] = (uint32_t)streamLength;

	memcpy(setup->eos, kEndOfStream, sizeof(kEndOfStream));
	setup->explicitEOSPresentFlag = 1;
	setup->stream_len = (unsigned)(streamLength + sizeof(kEndOfStream));
	setup->slice_count = sliceCount;
	setup->gptimer_timeout_value = 81000000;
	setup->mbhist_buffer_size = decoder->macroblockHistorySize;
	setup->log2_max_pic_order_cnt_lsb_minus4 = sps->log2MaxPocLsbMinus4;
	setup->delta_pic_order_always_zero_flag = sps->deltaPicOrderAlwaysZero;
	setup->frame_mbs_only_flag = sps->frameMbsOnly;
	setup->PicWidthInMbs = decoder->mbWidth;
	setup->FrameHeightInMbs = decoder->mbHeight;
	setup->tileFormat = 1;			/* blocks, not whole lines */
	setup->gob_height = 0;
	setup->entropy_coding_mode_flag = pps->entropyCodingMode;
	setup->pic_order_present_flag = pps->picOrderPresent;
	/* What the picture parameters say, not what a slice header overrides it
	 * to: the engine reads the slice headers itself. */
	setup->num_ref_idx_l0_active_minus1 = pps->numRefIdxL0Minus1;
	setup->num_ref_idx_l1_active_minus1 = pps->numRefIdxL1Minus1;
	setup->deblocking_filter_control_present_flag = pps->deblockingFilterControlPresent;
	setup->redundant_pic_cnt_present_flag = pps->redundantPicCntPresent;
	setup->transform_8x8_mode_flag = pps->transform8x8Mode;
	setup->pitch_luma = decoder->pitch;
	setup->pitch_chroma = decoder->pitch;
	setup->luma_bot_offset = (unsigned)decoder->mbWidth * 16;
	setup->chroma_bot_offset = (unsigned)decoder->pitch / 2;
	setup->HistBufferSize = decoder->historySize >> 8;
	setup->MbaffFrameFlag = sps->mbAdaptiveFrameField && !slice.fieldPic;
	setup->direct_8x8_inference_flag = sps->direct8x8Inference;
	setup->weighted_pred_flag = pps->weightedPred;
	setup->constrained_intra_pred_flag = pps->constrainedIntraPred;
	setup->ref_pic_flag = slice.nalRefIdc != 0;
	setup->field_pic_flag = 0;
	setup->bottom_field_flag = 0;
	setup->second_field = 0;
	setup->log2_max_frame_num_minus4 = sps->log2MaxFrameNumMinus4;
	setup->chroma_format_idc = sps->chromaFormatIdc;
	setup->pic_order_cnt_type = sps->picOrderCntType;
	setup->pic_init_qp_minus26 = pps->picInitQpMinus26;
	setup->chroma_qp_index_offset = pps->chromaQpIndexOffset;
	setup->second_chroma_qp_index_offset = pps->secondChromaQpIndexOffset;
	setup->weighted_bipred_idc = pps->weightedBipredIdc;
	fillPictureTable(decoder, setup);
	setup->CurrPicIdx = surface;
	setup->CurrColIdx = surface;
	setup->frame_num = slice.frameNum;
	setup->frame_surfaces = 0;
	setup->output_memory_layout = 0;	/* NV12 */
	setup->CurrFieldOrderCnt[0] = topPoc;
	setup->CurrFieldOrderCnt[1] = bottomPoc;
	setup->lossless_ipred8x8_filter_enable = 0;
	setup->qpprime_y_zero_transform_bypass_flag = sps->qpprimeYZeroTransformBypass;
	for (int i = 0; i < 6; i++)
		memcpy(&setup->WeightScale[i][0][0], pps->scaling4x4[i], 16);
	for (int i = 0; i < 2; i++)
		memcpy(&setup->WeightScale8x8[i][0][0], pps->scaling8x8[i], 64);
	NvdecEngine *engine = decoder->engine;
	nvdecBegin(engine);
	nvdecMethod(engine, NVC2B0_SET_APPLICATION_ID, NVDEC_CODEC_H264);
	nvdecMethod(engine, NVC2B0_SET_WATCHDOG_TIMER, 0);
	uint32_t errorFrame = sps->maxNumRefFrames > 0
		? decoder->pictureCount % (unsigned)sps->maxNumRefFrames : 0;
	nvdecMethod(engine, NVC2B0_SET_CONTROL_PARAMS,
		NVDEC_CODEC_H264			/* CODEC_TYPE 3:0 */
		| (1u << 4)				/* GPTIMER_ON */
		| (1u << 5)				/* RET_ERROR */
		| (1u << 6)				/* ERR_CONCEAL_ON */
		| ((errorFrame & 0x3f) << 7)		/* ERROR_FRM_IDX */
		| (1u << 13));				/* MBTIMER_ON */
	nvdecAddress(engine, NVC2B0_SET_DRV_PIC_SETUP_OFFSET, decoder->pictureSetup.gpuAddress);
	nvdecAddress(engine, NVC2B0_SET_IN_BUF_BASE_OFFSET, decoder->bitstream.gpuAddress);
	nvdecMethod(engine, NVC2B0_SET_PICTURE_INDEX, decoder->pictureCount);
	nvdecAddress(engine, NVC2B0_SET_SLICE_OFFSETS_BUF_OFFSET, decoder->sliceOffsets.gpuAddress);
	nvdecAddress(engine, NVC2B0_SET_COLOC_DATA_OFFSET, decoder->colocated.gpuAddress);
	nvdecAddress(engine, NVC2B0_SET_HISTORY_OFFSET, decoder->history.gpuAddress);
	nvdecAddress(engine, NVC2B0_SET_NVDEC_STATUS_OFFSET, nvdecStatusAddress(engine));
	nvdecAddress(engine, NVC2B0_SET_PIC_SCRATCH_BUF_OFFSET, decoder->scratch.gpuAddress);
	nvdecAddress(engine, NVC2B0_H264_SET_MBHIST_BUF_OFFSET,
		decoder->macroblockHistory.gpuAddress);
	for (int i = 0; i < 17; i++) {
		int from = i < decoder->surfaceCount ? i : decoder->surfaceCount;
		nvdecAddress(engine, NVC2B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i,
			decoder->lumaPool.gpuAddress + (size_t)from * decoder->lumaSize);
		nvdecAddress(engine, NVC2B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i,
			decoder->chromaPool.gpuAddress + (size_t)from * decoder->chromaSize);
	}
	nvdecMethod(engine, NVC2B0_EXECUTE, 1u << 0);

	if (!nvdecExecute(engine, 2000))
		return setError(decoder, "the decoder did not answer");
	nvdecGetStatus(engine, &decoder->status);
	if (decoder->status.errorStatus != 0) {
		return setError(decoder, "the decoder reported %#x on a picture of %u macroblocks",
			decoder->status.errorStatus, decoder->status.macroblocksDecoded);
	}

	/* The picture is decoded; record what it is and what it costs. */
	bool currentIsLongTerm = false, sawMmco5 = false;
	int currentLongTermIdx = 0;
	if (slice.nalRefIdc != 0 && !slice.idr) {
		if (slice.adaptiveRefPicMarking) {
			applyMarking(decoder, sps, &slice, slice.frameNum,
				&currentIsLongTerm, &currentLongTermIdx, &sawMmco5);
		} else {
			slidingWindow(decoder, sps);
		}
	} else if (slice.idr && slice.longTermReference) {
		currentIsLongTerm = true;
		currentLongTermIdx = 0;
	}

	if (sawMmco5) {
		topPoc -= (topPoc < bottomPoc) ? topPoc : bottomPoc;
		bottomPoc -= (topPoc < bottomPoc) ? topPoc : bottomPoc;
		decoder->prevFrameNum = 0;
		decoder->prevFrameNumOffset = 0;
		decoder->prevPocMsb = 0;
		decoder->prevPocLsb = topPoc;
	}

	FrameStore *store = NULL;
	for (int i = 0; i < MAX_SURFACES; i++) {
		if (!decoder->dpb[i].inUse) {
			store = &decoder->dpb[i];
			break;
		}
	}
	if (store == NULL)
		return setError(decoder, "no room to record the decoded picture");
	memset(store, 0, sizeof(*store));
	store->inUse = true;
	store->surface = surface;
	store->frameNum = slice.frameNum;
	store->frameNumWrap = slice.frameNum;
	store->picNum = slice.frameNum;
	store->topPoc = topPoc;
	store->bottomPoc = bottomPoc;
	store->poc = topPoc < bottomPoc ? topPoc : bottomPoc;
	store->neededForOutput = true;
	store->sequence = decoder->sequence;
	store->time = time;
	store->tableSlot = -1;
	if (slice.nalRefIdc != 0) {
		store->tableSlot = takeTableSlot(decoder);
		if (currentIsLongTerm) {
			store->longTerm = true;
			store->longTermFrameIdx = currentLongTermIdx;
			store->longTermPicNum = currentLongTermIdx;
		} else {
			store->shortTerm = true;
		}
	}
	decoder->surfaceBusy[surface] = true;
	decoder->dpbCount++;
	if (getenv("NVDEC_TRACE") != NULL) {
		static const char *kTypes[] = { "P", "B", "I", "SP", "SI" };
		fprintf(stderr, "%3u: %-2s fn %2d poc %3d %s surface %2d ->",
			decoder->pictureCount, kTypes[slice.sliceType], slice.frameNum,
			store->poc, slice.nalRefIdc != 0 ? "ref    " : "not ref",
			surface);
		for (int i = 0; i < MAX_SURFACES; i++) {
			FrameStore *other = &decoder->dpb[i];
			if (!other->inUse)
				continue;
			fprintf(stderr, " [%d:fn%d:poc%d%s%s]", other->surface,
				other->frameNum, other->poc,
				other->shortTerm ? ":short" : (other->longTerm ? ":long" : ""),
				other->neededForOutput ? ":out" : "");
		}
		fprintf(stderr, "\n");
	}
	decoder->prevHadMmco5 = false;
	decoder->pictureCount++;
	return true;
}
