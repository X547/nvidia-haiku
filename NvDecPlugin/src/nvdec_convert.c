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
static inline void
gatherLine(const NvdecFrame *frame, const uint8_t *plane, int x, int y,
	int count, uint8_t *out)
{
	if (frame->bitDepth <= 8) {
		for (int i = 0; i < count; i += 16) {
			int run = (count - i) < 16 ? (count - i) : 16;
			memcpy(out + i, plane + nvdecTileOffset(x + i, y, frame->pitch), run);
		}
		return;
	}
	for (int i = 0; i < count; i += 8) {
		int run = (count - i) < 8 ? (count - i) : 8;
		const uint8_t *from = plane
			+ nvdecTileOffset((x + i) * 2, y, frame->pitch);
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
		gatherLine(frame, frame->chroma, left, (top + y) / 2, width, chroma);
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
				memcpy(line + x, frame->luma + nvdecTileOffset((left + x) * 2,
					top + y, frame->pitch), (size_t)run * 2);
				if (chromaLine != NULL) {
					memcpy(chromaLine + x, frame->chroma + nvdecTileOffset(
						(left + x) * 2, (top + y) / 2, frame->pitch),
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
	int			kind;		/* 0 YCbCr422, 1 RGB32, 2 P010, 3 NV12 */
	int			bands;
	int32			next;
	/* The helpers and the caller each hold a reference; the last one out
	 * frees it. It is not on the caller's stack: a caller that stops
	 * waiting early - interrupted, or killed as its team exits - must not
	 * leave the helpers reading a stack that is gone. */
	int32			references;
	NvdecFrame		frameCopy;
} Work;

static void
releaseWork(Work *work)
{
	if (atomic_add(&work->references, -1) == 1)
		free(work);
}

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
		} else {
			nvdecFrameToYCbCr422(work->frame, work->out, work->outPitch, from, to);
		}
	}
}

static int32
convertHelper(void *data)
{
	Work *work = data;
	convertBands(work);
	releaseWork(work);
	return 0;
}

static void
convertThreaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	NvdecColorRange range, int kind)
{
	system_info info;
	get_system_info(&info);
	int threads = getenv("NVDEC_CONVERT_SINGLE") != NULL ? 1 : (int)info.cpu_count;
	if (threads > 8)
		threads = 8;
	if (threads < 1)
		threads = 1;

	Work *work = malloc(sizeof(Work));
	if (work == NULL) {
		/* No room even for this: convert on this thread alone. */
		Work local = { .frame = frame, .out = out, .outPitch = outPitch,
			.range = range, .kind = kind, .bands = 1, .next = 0 };
		convertBands(&local);
		return;
	}
	work->frameCopy = *frame;
	work->frame = &work->frameCopy;
	work->out = out;
	work->outPitch = outPitch;
	work->range = range;
	work->kind = kind;
	work->bands = threads * 2;
	work->next = 0;
	work->references = 1;

	thread_id helpers[8];
	int started = 0;
	for (int i = 0; i < threads - 1 && i < 8; i++) {
		atomic_add(&work->references, 1);
		helpers[started] = spawn_thread(convertHelper, "nvdec convert",
			B_DISPLAY_PRIORITY, work);
		if (helpers[started] < 0) {
			atomic_add(&work->references, -1);
			break;
		}
		resume_thread(helpers[started]);
		started++;
	}
	convertBands(work);
	for (int i = 0; i < started; i++) {
		status_t ignored;
		while (wait_for_thread(helpers[i], &ignored) == B_INTERRUPTED)
			;
	}
	releaseWork(work);
}

void
nvdecFrameToRGB32Threaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch,
	NvdecColorRange range)
{
	convertThreaded(frame, out, outPitch, range, 1);
}

void
nvdecFrameToYCbCr422Threaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch)
{
	convertThreaded(frame, out, outPitch, NVDEC_RANGE_BT601, 0);
}

void
nvdecFrameToP010Threaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch)
{
	convertThreaded(frame, out, outPitch, NVDEC_RANGE_BT601, 2);
}

void
nvdecFrameToNV12Threaded(const NvdecFrame *frame, uint8_t *out, size_t outPitch)
{
	convertThreaded(frame, out, outPitch, NVDEC_RANGE_BT601, 3);
}
