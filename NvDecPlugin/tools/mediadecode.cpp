/* Decode a film through Haiku's media kit and say who did the work.
 *
 * This is the plain way any program plays video: open the file, find the
 * video track, ask for pixels. If the NVDEC add-on is installed, the card
 * does the decoding and this says so.
 *
 * Usage: mediadecode <file> [frames] [out.ppm]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Entry.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <OS.h>

int
main(int argc, char** argv)
{
	if (argc < 2) {
		printf("usage: %s <file> [frames] [out.ppm]\n", argv[0]);
		return 2;
	}
	int wanted = argc > 2 ? atoi(argv[2]) : 100;

	entry_ref ref;
	if (get_ref_for_path(argv[1], &ref) != B_OK) {
		printf("cannot find %s\n", argv[1]);
		return 1;
	}
	BMediaFile file(&ref);
	if (file.InitCheck() != B_OK) {
		printf("cannot open %s: %s\n", argv[1], strerror(file.InitCheck()));
		return 1;
	}
	BMediaTrack* video = NULL;
	for (int i = 0; i < file.CountTracks(); i++) {
		BMediaTrack* track = file.TrackAt(i);
		media_format format;
		if (track->EncodedFormat(&format) == B_OK
			&& format.type == B_MEDIA_ENCODED_VIDEO) {
			video = track;
			break;
		}
		file.ReleaseTrack(track);
	}
	if (video == NULL) {
		printf("no video in %s\n", argv[1]);
		return 1;
	}

	media_format format;
	memset(&format, 0, sizeof(format));
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video.display.format = B_RGB32;
	if (video->DecodedFormat(&format) != B_OK) {
		printf("nothing here can decode this film\n");
		return 1;
	}
	media_codec_info codec;
	if (video->GetCodecInfo(&codec) == B_OK)
		printf("decoder: %s\n", codec.pretty_name);

	int width = format.u.raw_video.display.line_width;
	int height = format.u.raw_video.display.line_count;
	size_t rowBytes = format.u.raw_video.display.bytes_per_row;
	printf("%dx%d, %zu bytes a line, %.2f pictures a second, %lld in all\n",
		width, height, rowBytes, format.u.raw_video.field_rate,
		video->CountFrames());

	uint8* buffer = (uint8*)malloc(rowBytes * height);
	if (buffer == NULL)
		return 1;

	int64 got = 0;
	bigtime_t began = system_time();
	while (got < wanted) {
		int64 count = 0;
		media_header header;
		status_t status = video->ReadFrames(buffer, &count, &header);
		if (status != B_OK) {
			printf("stopped after %lld: %s\n", got, strerror(status));
			break;
		}
		got += count > 0 ? count : 1;
	}
	bigtime_t took = system_time() - began;
	printf("%lld pictures in %.1f ms, %.2f ms each (%.1f a second)\n",
		got, took / 1000.0, took / 1000.0 / (got > 0 ? got : 1),
		got * 1000000.0 / (took > 0 ? took : 1));

	if (argc > 3) {
		FILE* out = fopen(argv[3], "wb");
		if (out != NULL) {
			fprintf(out, "P6\n%d %d\n255\n", width, height);
			for (int y = 0; y < height; y++) {
				uint8* line = buffer + (size_t)y * rowBytes;
				for (int x = 0; x < width; x++) {
					fputc(line[4 * x + 2], out);
					fputc(line[4 * x + 1], out);
					fputc(line[4 * x + 0], out);
				}
			}
			fclose(out);
			printf("wrote the last picture to %s\n", argv[3]);
		}
	}
	free(buffer);
	return 0;
}
