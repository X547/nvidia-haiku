/* Decode a whole H.264 stream on the card and write what comes out.
 *
 * Reads an Annex B file, hands the decoder one picture at a time, and writes
 * the pictures in the order they should be shown, as I420 - which is what
 * ffmpeg writes with -pix_fmt yuv420p, so the two can be compared byte for
 * byte. With NVDEC_NV12 set in the environment, as NV12 (-pix_fmt nv12)
 * through the media add-on's conversion.
 *
 * Usage: nvdecstream <stream.h264> [out.yuv] [frame limit]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include "nvdec_convert.h"
#include "nvdec_h264.h"

static uint8_t *gPlane;
static size_t gPlaneSize;

static void
writeFrame(FILE *out, const NvdecFrame *frame)
{
	if (out == NULL)
		return;
	if (getenv("NVDEC_NV12") != NULL) {
		/* What the media add-on gives a caller that asks for 'NV12': ffmpeg
		 * writes the same with -pix_fmt nv12. */
		size_t size = (size_t)frame->width * frame->height * 3 / 2;
		if (gPlaneSize < size) {
			gPlane = realloc(gPlane, size);
			gPlaneSize = size;
		}
		nvdecFrameToNV12Threaded(frame, gPlane, frame->width);
		fwrite(gPlane, 1, size, out);
		return;
	}
	size_t needed = (size_t)frame->codedWidth * frame->codedHeight;
	if (gPlaneSize < needed) {
		gPlane = realloc(gPlane, needed);
		gPlaneSize = needed;
	}
	/* Luma, cropped to what should be shown. */
	nvdecUntile(gPlane, frame->width, frame->luma + 0, frame->width, frame->height,
		frame->pitch);
	fwrite(gPlane, 1, (size_t)frame->width * frame->height, out);

	/* Chroma arrives with Cb and Cr interleaved; I420 wants them apart. */
	int chromaWidth = frame->width / 2;
	int chromaHeight = frame->height / 2;
	nvdecUntile(gPlane, frame->width, frame->chroma, frame->width, chromaHeight,
		frame->pitch);
	uint8_t *cb = malloc((size_t)chromaWidth * chromaHeight);
	uint8_t *cr = malloc((size_t)chromaWidth * chromaHeight);
	for (int y = 0; y < chromaHeight; y++) {
		const uint8_t *line = gPlane + (size_t)y * frame->width;
		for (int x = 0; x < chromaWidth; x++) {
			cb[(size_t)y * chromaWidth + x] = line[2 * x];
			cr[(size_t)y * chromaWidth + x] = line[2 * x + 1];
		}
	}
	fwrite(cb, 1, (size_t)chromaWidth * chromaHeight, out);
	fwrite(cr, 1, (size_t)chromaWidth * chromaHeight, out);
	free(cb);
	free(cr);
}

/* Cut a stream into access units: one picture, with whatever parameter sets
 * and other headers were sent immediately before it. */
typedef struct {
	size_t	offset;
	size_t	length;
	int	type;
	bool	startsPicture;
} Nal;

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
		nals[count].offset = i;
		nals[count].length = end - i;
		nals[count].type = data[payload] & 0x1f;
		/* first_mb_in_slice is the first exp-Golomb value of a slice
		 * header, so a leading set bit means it is zero. */
		nals[count].startsPicture = (nals[count].type == 1 || nals[count].type == 5)
			&& payload + 1 < size && (data[payload + 1] & 0x80) != 0;
		count++;
		i = (j + 3 <= size) ? j : size;
	}
	return count;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		printf("usage: %s <stream.h264> [out.yuv] [frame limit]\n", argv[0]);
		return 2;
	}
	int limit = (argc > 3) ? atoi(argv[3]) : 0;
	bool verbose = getenv("NVDEC_VERBOSE") != NULL;

	FILE *file = fopen(argv[1], "rb");
	if (file == NULL) { perror(argv[1]); return 1; }
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	uint8_t *data = malloc(size);
	if (data == NULL || fread(data, 1, size, file) != (size_t)size) {
		printf("cannot read %s\n", argv[1]);
		return 1;
	}
	fclose(file);

	FILE *out = NULL;
	if (argc > 2 && strcmp(argv[2], "-") != 0) {
		out = fopen(argv[2], "wb");
		if (out == NULL) { perror(argv[2]); return 1; }
	}

	size_t maxNals = size / 4 + 16;
	Nal *nals = malloc(maxNals * sizeof(Nal));
	size_t nalCount = findNals(data, size, nals, maxNals);

	char reason[256] = "";
	NvdecEngine *engine = nvdecOpen(reason, sizeof(reason));
	if (engine == NULL) { printf("opening the decoder: %s\n", reason); return 1; }
	NvdecH264 *decoder = nvdecH264Create(engine, reason, sizeof(reason));
	if (decoder == NULL) { printf("creating the decoder failed\n"); return 1; }

	int decoded = 0, written = 0;
	bigtime_t began = system_time();
	bigtime_t decodeTime = 0;
	size_t auStart = 0;
	size_t auEnd = 0;		/* just past the last picture unit seen */
	bool sawPicture = false;
	NvdecFrame frame;

	for (size_t i = 0; i <= nalCount; i++) {
		bool last = (i == nalCount);
		bool newPicture = !last && nals[i].startsPicture;
		if ((newPicture || last) && sawPicture) {
			bigtime_t at = system_time();
			if (verbose) {
				printf("picture %d: %zu bytes, units", decoded,
					auEnd - auStart);
				for (size_t j = 0; j < nalCount; j++) {
					if (nals[j].offset >= auStart && nals[j].offset < auEnd)
						printf(" %d", nals[j].type);
				}
				printf("\n");
			}
			if (!nvdecH264Decode(decoder, data + auStart, auEnd - auStart, decoded)) {
				printf("picture %d: %s\n", decoded, nvdecH264LastError(decoder));
				break;
			}
			decodeTime += system_time() - at;
			decoded++;
			while (nvdecH264NextFrame(decoder, &frame)) {
				writeFrame(out, &frame);
				nvdecH264ReleaseFrame(decoder, &frame);
				written++;
			}
			if (limit > 0 && decoded >= limit)
				break;
			auStart = nals[i].offset;
			sawPicture = false;
		}
		if (last)
			break;
		if (!sawPicture && (nals[i].type == 1 || nals[i].type == 5)) {
			sawPicture = true;
		}
		if (nals[i].type == 1 || nals[i].type == 5)
			auEnd = nals[i].offset + nals[i].length;
	}

	nvdecH264DrainAll(decoder);
	while (nvdecH264NextFrame(decoder, &frame)) {
		writeFrame(out, &frame);
		nvdecH264ReleaseFrame(decoder, &frame);
		written++;
	}

	bigtime_t total = system_time() - began;
	int width = 0, height = 0;
	nvdecH264HasFormat(decoder, &width, &height);
	printf("%d pictures decoded, %d written, %dx%d\n", decoded, written, width, height);
	if (decoded > 0 && decodeTime > 0) {
		printf("decoding took %.1f ms in all, %.2f ms a picture (%.1f a second)\n",
			decodeTime / 1000.0, decodeTime / 1000.0 / decoded,
			decoded * 1000000.0 / decodeTime);
		printf("everything, including writing the file, took %.1f ms\n", total / 1000.0);
	}
	if (out != NULL)
		fclose(out);
	nvdecH264Destroy(decoder);
	nvdecClose(engine);
	return 0;
}
