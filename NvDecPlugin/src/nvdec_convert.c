/* See nvdec_convert.h. */

#include "nvdec_convert.h"

#include <stdlib.h>
#include <string.h>

#include <OS.h>

/* Y is scaled from 16..235 and the differences from 16..240, so both are
 * stretched; the three coefficients are the ones the standard names. */
typedef struct {
	int	yMul;
	int	rV, gU, gV, bU;
} Coefficients;

static const Coefficients kBT601 = { 298, 409, -100, -208, 516 };
static const Coefficients kBT709 = { 298, 459, -55, -136, 541 };

static inline uint8_t
clamp(int value)
{
	return (uint8_t)(value < 0 ? 0 : (value > 255 ? 255 : value));
}

/* One line of a plane, `count` samples from column `x`, as eight bit samples.
 * Sixteen bytes are consecutive in the card's arrangement, so a line is
 * gathered in runs of that many: sixteen samples, or eight of the sixteen-bit
 * ones, of which the top byte is kept. */
/* Where byte `x` of line `y` of a plane is: in plain lines once the copy
 * engine has untiled the picture, in the card's arrangement otherwise. */
static inline size_t
planeOffset(const NvdecFrame *frame, int x, int y)
{
	if (frame->linear)
		return (size_t)y * frame->pitch + x;
	return nvdecTileOffset(x, y, frame->pitch);
}

static inline void
gatherLine(const NvdecFrame *frame, const uint8_t *plane, int x, int y,
	int count, uint8_t *out)
{
	if (frame->bitDepth <= 8) {
		if (frame->linear) {
			memcpy(out, plane + planeOffset(frame, x, y), count);
			return;
		}
		for (int i = 0; i < count; i += 16) {
			int run = (count - i) < 16 ? (count - i) : 16;
			memcpy(out + i, plane + planeOffset(frame, x + i, y), run);
		}
		return;
	}
	for (int i = 0; i < count; i += 8) {
		int run = (count - i) < 8 ? (count - i) : 8;
		const uint8_t *from = plane
			+ planeOffset(frame, (x + i) * 2, y);
		for (int k = 0; k < run; k++)
			out[i + k] = from[2 * k + 1];
	}
}

void
nvdecFrameToRGB32(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	int fromLine, int toLine, NvdecColorRange range)
{
	const Coefficients *c = (range == NVDEC_RANGE_BT709) ? &kBT709 : &kBT601;
	const int width = frame->width;
	const int left = frame->cropLeft;
	const int top = frame->cropTop;

	/* Gathering a line at a time, on the stack: this runs on several threads
	 * at once and thirty times a second, so it allocates nothing. */
	uint8_t luma[NVDEC_MAX_WIDTH + 32];
	uint8_t chroma[NVDEC_MAX_WIDTH + 32];
	if (width > NVDEC_MAX_WIDTH)
		return;
	for (int y = fromLine; y < toLine; y++) {
		gatherLine(frame, frame->luma, left, top + y, width, luma);
		/* An odd width still needs the last pixel's Cr, the byte after it. */
		gatherLine(frame, frame->chroma, left, (top + y) / 2, (width + 1) & ~1,
			chroma);
		uint8_t *line = out + (size_t)y * outPitch;
		for (int x = 0; x < width; x++) {
			int yy = (luma[x] - 16) * c->yMul;
			int cb = chroma[(x & ~1)] - 128;
			int cr = chroma[(x & ~1) + 1] - 128;
			int r = (yy + c->rV * cr + 128) >> 8;
			int g = (yy + c->gU * cb + c->gV * cr + 128) >> 8;
			int b = (yy + c->bU * cb + 128) >> 8;
			line[4 * x + 0] = clamp(b);
			line[4 * x + 1] = clamp(g);
			line[4 * x + 2] = clamp(r);
			line[4 * x + 3] = 255;
		}
	}
}

void
nvdecFrameToYCbCr422(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	int fromLine, int toLine)
{
	const int width = frame->width;
	const int left = frame->cropLeft;
	const int top = frame->cropTop;
	uint8_t luma[NVDEC_MAX_WIDTH + 32];
	uint8_t chroma[NVDEC_MAX_WIDTH + 32];
	if (width > NVDEC_MAX_WIDTH)
		return;
	for (int y = fromLine; y < toLine; y++) {
		gatherLine(frame, frame->luma, left, top + y, width, luma);
		gatherLine(frame, frame->chroma, left, (top + y) / 2, width, chroma);
		uint8_t *line = out + (size_t)y * outPitch;
		for (int x = 0; x + 1 < width; x += 2) {
			line[2 * x + 0] = chroma[x];		/* Cb */
			line[2 * x + 1] = luma[x];
			line[2 * x + 2] = chroma[x + 1];	/* Cr */
			line[2 * x + 3] = luma[x + 1];
		}
	}
}

void
nvdecFrameToP010(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	int fromLine, int toLine)
{
	const int width = frame->width;
	const int left = frame->cropLeft;
	const int top = frame->cropTop;
	uint8_t *chromaOut = out + outPitch * (size_t)frame->height;
	for (int y = fromLine; y < toLine; y++) {
		uint16_t *line = (uint16_t *)(out + (size_t)y * outPitch);
		uint16_t *chromaLine = (y & 1) == 0
			? (uint16_t *)(chromaOut + (size_t)(y / 2) * outPitch) : NULL;
		if (frame->bitDepth > 8) {
			/* Already sixteen bits a sample: runs of eight samples are
			 * consecutive, so this is a copy in the card's order. */
			for (int x = 0; x < width; x += 8) {
				int run = (width - x) < 8 ? (width - x) : 8;
				memcpy(line + x, frame->luma + planeOffset(frame,
					(left + x) * 2, top + y), (size_t)run * 2);
				if (chromaLine != NULL) {
					memcpy(chromaLine + x, frame->chroma + planeOffset(frame,
						(left + x) * 2, (top + y) / 2),
						(size_t)run * 2);
				}
			}
			continue;
		}
		uint8_t luma[NVDEC_MAX_WIDTH + 32], chroma[NVDEC_MAX_WIDTH + 32];
		if (width > NVDEC_MAX_WIDTH)
			return;
		gatherLine(frame, frame->luma, left, top + y, width, luma);
		for (int x = 0; x < width; x++)
			line[x] = (uint16_t)(luma[x] << 8);
		if (chromaLine != NULL) {
			gatherLine(frame, frame->chroma, left, (top + y) / 2, width, chroma);
			for (int x = 0; x < width; x++)
				chromaLine[x] = (uint16_t)(chroma[x] << 8);
		}
	}
}

void
nvdecFrameToNV12(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	int fromLine, int toLine)
{
	const int width = frame->width;
	const int left = frame->cropLeft;
	const int top = frame->cropTop;
	uint8_t *chromaOut = out + outPitch * (size_t)frame->height;
	for (int y = fromLine; y < toLine; y++) {
		gatherLine(frame, frame->luma, left, top + y, width,
			out + (size_t)y * outPitch);
		if ((y & 1) == 0) {
			gatherLine(frame, frame->chroma, left, (top + y) / 2, width,
				chromaOut + (size_t)(y / 2) * outPitch);
		}
	}
}

/* ------------------------------------------------------------- in bands */

typedef struct {
	const NvdecFrame	*frame;
	uint8_t			*out;
	size_t			outPitch;
	NvdecColorRange		range;
	int			kind;		/* 0 YCbCr422, 1 RGB32, 2 P010, 3 NV12,
					   4 copying converted lines */
	int			bands;
	int32			next;
	NvdecFrame		frameCopy;
	const uint8_t		*source;	/* the lines to copy, kind 4 */
	size_t			sourcePitch;
} Work;

#define MAX_HELPERS	7

/* The picture being converted is described in the pool, not on the caller's
 * stack: a caller that stops waiting early - interrupted, or killed as its
 * team exits - must not leave the helpers reading a stack that is gone. The
 * pool outlives every picture it converts. */
struct NvdecConvertPool {
	int		helpers;
	thread_id	helper[MAX_HELPERS];
	sem_id		start;		/* one count a helper, for each picture */
	sem_id		done;		/* one count a helper, when it is through */
	int32		quit;
	Work		work;
};

static void
convertBands(Work *work)
{
	for (;;) {
		int32 band = atomic_add(&work->next, 1);
		if (band >= work->bands)
			break;
		int height = work->frame->height;
		int from = (int)((int64)height * band / work->bands) & ~1;
		int to = (int)((int64)height * (band + 1) / work->bands) & ~1;
		if (band == work->bands - 1)
			to = height;
		if (work->kind == 1) {
			nvdecFrameToRGB32(work->frame, work->out, work->outPitch,
				from, to, work->range);
		} else if (work->kind == 2) {
			nvdecFrameToP010(work->frame, work->out, work->outPitch, from, to);
		} else if (work->kind == 3) {
			nvdecFrameToNV12(work->frame, work->out, work->outPitch, from, to);
		} else if (work->kind == 4) {
			size_t lineSize = (size_t)work->frame->width * 4;
			for (int y = from; y < to; y++) {
				memcpy(work->out + (size_t)y * work->outPitch,
					work->source + (size_t)y * work->sourcePitch, lineSize);
			}
		} else {
			nvdecFrameToYCbCr422(work->frame, work->out, work->outPitch, from, to);
		}
	}
}

static void
acquireCounts(sem_id sem, int32 count)
{
	while (count > 0) {
		if (acquire_sem(sem) == B_OK)
			count--;
	}
}

static int32
convertHelper(void *data)
{
	NvdecConvertPool *pool = data;
	for (;;) {
		acquireCounts(pool->start, 1);
		if (atomic_get(&pool->quit) != 0)
			break;
		convertBands(&pool->work);
		release_sem_etc(pool->done, 1, B_DO_NOT_RESCHEDULE);
	}
	return 0;
}

NvdecConvertPool *
nvdecConvertPoolCreate(void)
{
	NvdecConvertPool *pool = calloc(1, sizeof(NvdecConvertPool));
	if (pool == NULL)
		return NULL;

	system_info info;
	get_system_info(&info);
	int wanted = getenv("NVDEC_CONVERT_SINGLE") != NULL ? 0 : (int)info.cpu_count - 1;
	if (wanted > MAX_HELPERS)
		wanted = MAX_HELPERS;

	pool->start = create_sem(0, "nvdec convert start");
	pool->done = create_sem(0, "nvdec convert done");
	if (pool->start < 0 || pool->done < 0) {
		wanted = 0;
	}
	for (int i = 0; i < wanted; i++) {
		thread_id helper = spawn_thread(convertHelper, "nvdec convert",
			B_DISPLAY_PRIORITY, pool);
		if (helper < 0)
			break;
		pool->helper[pool->helpers++] = helper;
		resume_thread(helper);
	}
	return pool;
}

void
nvdecConvertPoolDestroy(NvdecConvertPool *pool)
{
	if (pool == NULL)
		return;
	atomic_set(&pool->quit, 1);
	if (pool->helpers > 0)
		release_sem_etc(pool->start, pool->helpers, 0);
	for (int i = 0; i < pool->helpers; i++) {
		status_t ignored;
		while (wait_for_thread(pool->helper[i], &ignored) == B_INTERRUPTED)
			;
	}
	if (pool->start >= 0)
		delete_sem(pool->start);
	if (pool->done >= 0)
		delete_sem(pool->done);
	free(pool);
}

static void
convertThreaded(NvdecConvertPool *pool, const NvdecFrame *frame, uint8_t *out,
	size_t outPitch, NvdecColorRange range, int kind, const uint8_t *source,
	size_t sourcePitch)
{
	if (pool == NULL || pool->helpers == 0) {
		Work local = { .frame = frame, .out = out, .outPitch = outPitch,
			.range = range, .kind = kind, .bands = 1, .next = 0,
			.source = source, .sourcePitch = sourcePitch };
		convertBands(&local);
		return;
	}

	Work *work = &pool->work;
	work->frameCopy = *frame;
	work->frame = &work->frameCopy;
	work->out = out;
	work->outPitch = outPitch;
	work->range = range;
	work->kind = kind;
	work->source = source;
	work->sourcePitch = sourcePitch;
	work->bands = (pool->helpers + 1) * 2;
	work->next = 0;

	/* A helper that finds no band left just reports done, so every count
	 * given out comes back before the next picture is described. */
	release_sem_etc(pool->start, pool->helpers, 0);
	convertBands(work);
	acquireCounts(pool->done, pool->helpers);
}

void
nvdecFrameToRGB32Threaded(NvdecConvertPool *pool, const NvdecFrame *frame,
	uint8_t *out, size_t outPitch, NvdecColorRange range)
{
	convertThreaded(pool, frame, out, outPitch, range, 1, NULL, 0);
}

bool
nvdecFrameToRGB32OnGpu(NvdecEngine *engine, NvdecConvertPool *pool,
	void *pin, const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	NvdecColorRange range)
{
	if (pin == NULL || !frame->linear || frame->bitDepth > 8)
		return false;
	const Coefficients *c = (range == NVDEC_RANGE_BT709) ? &kBT709 : &kBT601;
	const int coefficients[5] = { c->yMul, c->rV, c->gU, c->gV, c->bU };
	size_t sourcePitch;
	const uint8_t *source = nvdecConvertRGB32(engine, pin, frame->cropLeft,
		frame->cropTop, frame->width, frame->height, frame->pitch, coefficients,
		&sourcePitch);
	if (source == NULL)
		return false;
	convertThreaded(pool, frame, out, outPitch, range, 4, source, sourcePitch);
	return true;
}

void
nvdecFrameToYCbCr422Threaded(NvdecConvertPool *pool, const NvdecFrame *frame,
	uint8_t *out, size_t outPitch)
{
	convertThreaded(pool, frame, out, outPitch, NVDEC_RANGE_BT601, 0, NULL, 0);
}

void
nvdecFrameToP010Threaded(NvdecConvertPool *pool, const NvdecFrame *frame,
	uint8_t *out, size_t outPitch)
{
	convertThreaded(pool, frame, out, outPitch, NVDEC_RANGE_BT601, 2, NULL, 0);
}

void
nvdecFrameToNV12Threaded(NvdecConvertPool *pool, const NvdecFrame *frame,
	uint8_t *out, size_t outPitch)
{
	convertThreaded(pool, frame, out, outPitch, NVDEC_RANGE_BT601, 3, NULL, 0);
}
