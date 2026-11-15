/* See NVDecPlugin.h. */

#include "NVDecPlugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <MediaFormats.h>

/* libavcodec's number for H.264. The reader describes every encoded stream
 * with it, so this is how a format is recognised without linking to it. */
#define CODEC_ID_H264	27
#define CODEC_ID_HEVC	173
#define CODEC_ID_VP9	167

/* The decoders NewDecoder() has: H.264, H.265 and VP9 (see NVDecPlugin.h). */
#define DECODER_COUNT	3

#define ACCESS_UNIT_START	(1 << 20)

/* NVDEC_TIMING=1: say every sixty pictures where the time went. */
static bool sTiming = getenv("NVDEC_TIMING") != NULL;
static bigtime_t sEngineTime, sConvertTime, sChunkTime;
static int sTimedUnits, sTimedFrames;


NVDecDecoder::NVDecDecoder()
	:
	fEngine(NULL),
	fHevc(false),
	fVp9(false),
	fDecoder(NULL),
	fHevcDecoder(NULL),
	fVp9Decoder(NULL),
	fLengthSize(0),
	fAccessUnit(NULL),
	fAccessUnitSize(0),
	fAccessUnitUsed(0),
	fParameterSets(NULL),
	fParameterSetsSize(0),
	fSentParameterSets(false),
	fWidth(0),
	fHeight(0),
	fColorSpace(B_RGB32),
	fRowBytes(0),
	fFrameTime(0),
	fLastTime(0),
	fFrameNumber(0),
	fRange(NVDEC_RANGE_BT709),
	fBitDepth(8)
{
}


NVDecDecoder::~NVDecDecoder()
{
	if (fDecoder != NULL)
		nvdecH264Destroy(fDecoder);
	if (fHevcDecoder != NULL)
		nvdecHevcDestroy(fHevcDecoder);
	if (fVp9Decoder != NULL)
		nvdecVp9Destroy(fVp9Decoder);
	if (fEngine != NULL)
		nvdecClose(fEngine);
	free(fAccessUnit);
	free(fParameterSets);
}


void
NVDecDecoder::GetCodecInfo(media_codec_info* info)
{
	if (fVp9) {
		strlcpy(info->short_name, "nvdec vp9", sizeof(info->short_name));
		strlcpy(info->pretty_name, "VP9 on the graphics card (NVDEC)",
			sizeof(info->pretty_name));
		return;
	}
	if (fHevc) {
		strlcpy(info->short_name, "nvdec hevc", sizeof(info->short_name));
		strlcpy(info->pretty_name, "H.265 on the graphics card (NVDEC)",
			sizeof(info->pretty_name));
		return;
	}
	/* "2" since it holds sixteen reference frames: programs that sent such
	 * streams elsewhere, knowing the first version could not, tell the two
	 * apart by this name. */
	strlcpy(info->short_name, "nvdec h264 2", sizeof(info->short_name));
	strlcpy(info->pretty_name, "H.264 on the graphics card (NVDEC)",
		sizeof(info->pretty_name));
}


/* The parameter sets of an MPEG-4 file arrive as an AVCDecoderConfiguration
 * record rather than as start codes. Turn them into a piece of stream the
 * decoder can be handed. */
status_t
NVDecDecoder::_ReadParameterSets(const uint8* data, size_t size)
{
	/* A VP9 stream's codec private data, if any (a vpcC record), says
	 * nothing the frames do not. */
	if (size == 0 || fVp9)
		return B_OK;

	if (fHevc) {
		if (data[0] != 1 || size < 23) {
			fLengthSize = 0;
			fParameterSets = (uint8*)malloc(size);
			if (fParameterSets == NULL)
				return B_NO_MEMORY;
			memcpy(fParameterSets, data, size);
			fParameterSetsSize = size;
			return B_OK;
		}
		/* An HEVCDecoderConfigurationRecord: 22 bytes of header, then
		 * arrays of NAL units by type. */
		fBitDepth = (data[19] & 7) + 8;
		fLengthSize = (data[21] & 3) + 1;
		size_t capacity = size * 2 + 64;
		fParameterSets = (uint8*)malloc(capacity);
		if (fParameterSets == NULL)
			return B_NO_MEMORY;
		fParameterSetsSize = 0;
		int arrays = data[22];
		size_t at = 23;
		for (int a = 0; a < arrays && at + 3 <= size; a++) {
			int count = (data[at + 1] << 8) | data[at + 2];
			at += 3;
			for (int i = 0; i < count && at + 2 <= size; i++) {
				size_t length = ((size_t)data[at] << 8) | data[at + 1];
				at += 2;
				if (at + length > size
					|| fParameterSetsSize + length + 4 > capacity) {
					return B_OK;
				}
				fParameterSets[fParameterSetsSize++] = 0;
				fParameterSets[fParameterSetsSize++] = 0;
				fParameterSets[fParameterSetsSize++] = 0;
				fParameterSets[fParameterSetsSize++] = 1;
				memcpy(fParameterSets + fParameterSetsSize, data + at, length);
				fParameterSetsSize += length;
				at += length;
			}
		}
		return B_OK;
	}

	if (data[0] != 1 || size < 7) {
		/* Already a piece of stream with start codes. */
		fLengthSize = 0;
		fParameterSets = (uint8*)malloc(size);
		if (fParameterSets == NULL)
			return B_NO_MEMORY;
		memcpy(fParameterSets, data, size);
		fParameterSetsSize = size;
		return B_OK;
	}

	fLengthSize = (data[4] & 0x3) + 1;
	size_t capacity = size + 64;
	fParameterSets = (uint8*)malloc(capacity);
	if (fParameterSets == NULL)
		return B_NO_MEMORY;
	fParameterSetsSize = 0;

	size_t at = 5;
	for (int round = 0; round < 2 && at < size; round++) {
		int count = (round == 0) ? (data[at] & 0x1f) : data[at];
		at++;
		for (int i = 0; i < count && at + 2 <= size; i++) {
			size_t length = ((size_t)data[at] << 8) | data[at + 1];
			at += 2;
			if (at + length > size || fParameterSetsSize + length + 4 > capacity)
				break;
			fParameterSets[fParameterSetsSize++] = 0;
			fParameterSets[fParameterSetsSize++] = 0;
			fParameterSets[fParameterSetsSize++] = 0;
			fParameterSets[fParameterSetsSize++] = 1;
			memcpy(fParameterSets + fParameterSetsSize, data + at, length);
			fParameterSetsSize += length;
			at += length;
		}
	}
	return B_OK;
}


status_t
NVDecDecoder::Setup(media_format* ioEncodedFormat, const void* infoBuffer,
	size_t infoSize)
{
	if (ioEncodedFormat->type != B_MEDIA_ENCODED_VIDEO)
		return B_ERROR;

	media_format_description description;
	BMediaFormats formats;
	if (formats.GetCodeFor(*ioEncodedFormat, B_MISC_FORMAT_FAMILY,
			&description) != B_OK) {
		return B_ERROR;
	}
	if (description.u.misc.codec == CODEC_ID_HEVC)
		fHevc = true;
	else if (description.u.misc.codec == CODEC_ID_VP9)
		fVp9 = true;
	else if (description.u.misc.codec != CODEC_ID_H264)
		return B_ERROR;

	fReason[0] = '\0';
	fEngine = nvdecOpen(fReason, sizeof(fReason));
	if (fEngine == NULL) {
		fprintf(stderr, "nvdec: the card's decoder is not available: %s\n", fReason);
		return B_ERROR;
	}
	if (fVp9)
		fVp9Decoder = nvdecVp9Create(fEngine, fReason, sizeof(fReason));
	else if (fHevc)
		fHevcDecoder = nvdecHevcCreate(fEngine, fReason, sizeof(fReason));
	else
		fDecoder = nvdecH264Create(fEngine, fReason, sizeof(fReason));
	if (fDecoder == NULL && fHevcDecoder == NULL && fVp9Decoder == NULL) {
		nvdecClose(fEngine);
		fEngine = NULL;
		return B_NO_MEMORY;
	}

	if (_ReadParameterSets((const uint8*)infoBuffer, infoSize) != B_OK)
		return B_NO_MEMORY;

	fInputFormat = *ioEncodedFormat;
	const media_video_display_info& display
		= ioEncodedFormat->u.encoded_video.output.display;
	fWidth = display.line_width;
	fHeight = display.line_count;
	float rate = ioEncodedFormat->u.encoded_video.output.field_rate;
	fFrameTime = rate > 0 ? (bigtime_t)(1000000 / rate) : 0;
	fRange = nvdecRangeForHeight(fHeight > 0 ? fHeight : 720);
	return B_OK;
}


status_t
NVDecDecoder::NegotiateOutputFormat(media_format* ioDecodedFormat)
{
	media_format format;
	memset(&format, 0, sizeof(format));
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video = fInputFormat.u.encoded_video.output;
	format.u.raw_video.interlace = 1;
	format.u.raw_video.first_active = 0;
	format.u.raw_video.orientation = B_VIDEO_TOP_LEFT_RIGHT;
	format.u.raw_video.pixel_width_aspect
		= fInputFormat.u.encoded_video.output.pixel_width_aspect;
	format.u.raw_video.pixel_height_aspect
		= fInputFormat.u.encoded_video.output.pixel_height_aspect;

	/* Anything that asks for luma and chroma gets it without a conversion;
	 * everything else gets pixels. */
	uint32 wanted = ioDecodedFormat->u.raw_video.display.format;
	if (wanted == B_YCbCr422)
		fColorSpace = B_YCbCr422;
	else if (wanted == (uint32)NVDEC_COLOR_SPACE_P010 && (fHevc || fVp9))
		fColorSpace = NVDEC_COLOR_SPACE_P010;
	else if (wanted == (uint32)NVDEC_COLOR_SPACE_NV12)
		fColorSpace = NVDEC_COLOR_SPACE_NV12;
	else
		fColorSpace = B_RGB32;

	format.u.raw_video.display.format = (color_space)fColorSpace;
	format.u.raw_video.display.line_width = fWidth;
	format.u.raw_video.display.line_count = fHeight;
	format.u.raw_video.last_active = fHeight - 1;
	fRowBytes = (size_t)fWidth * (fColorSpace == B_RGB32 ? 4
		: fColorSpace == NVDEC_COLOR_SPACE_NV12 ? 1 : 2);
	format.u.raw_video.display.bytes_per_row = fRowBytes;

	*ioDecodedFormat = format;
	return B_OK;
}


status_t
NVDecDecoder::SeekedTo(int64 frame, bigtime_t time)
{
	if (fDecoder != NULL)
		nvdecH264Reset(fDecoder);
	if (fHevcDecoder != NULL)
		nvdecHevcReset(fHevcDecoder);
	if (fVp9Decoder != NULL)
		nvdecVp9Reset(fVp9Decoder);
	fAccessUnitUsed = 0;
	fSentParameterSets = false;
	fFrameNumber = frame;
	fLastTime = time;
	return B_OK;
}


/* Collect one chunk into the access unit being built, turning length prefixes
 * into start codes if the file uses them. */
status_t
NVDecDecoder::_AppendChunk(const uint8* data, size_t size)
{
	/* A start code is four bytes where a length prefix may be fewer, so the
	 * access unit can come out longer than the chunk went in. */
	size_t needed = fAccessUnitUsed + size * 2 + 64;
	if (needed > fAccessUnitSize) {
		size_t grown = needed * 2;
		uint8* buffer = (uint8*)realloc(fAccessUnit, grown);
		if (buffer == NULL)
			return B_NO_MEMORY;
		fAccessUnit = buffer;
		fAccessUnitSize = grown;
	}
	if (fLengthSize == 0) {
		memcpy(fAccessUnit + fAccessUnitUsed, data, size);
		fAccessUnitUsed += size;
		return B_OK;
	}
	size_t at = 0;
	while (at + (size_t)fLengthSize <= size) {
		size_t length = 0;
		for (int i = 0; i < fLengthSize; i++)
			length = (length << 8) | data[at + i];
		at += fLengthSize;
		if (length == 0 || at + length > size)
			break;
		uint8* to = fAccessUnit + fAccessUnitUsed;
		to[0] = 0; to[1] = 0; to[2] = 0; to[3] = 1;
		memcpy(to + 4, data + at, length);
		fAccessUnitUsed += 4 + length;
		at += length;
	}
	return B_OK;
}


void
NVDecDecoder::_Deliver(const NvdecFrame& frame, void* buffer,
	media_header* mediaHeader, bool convert)
{
	/* What the file said the picture is need not be what the stream says,
	 * and the buffer was made from what the file said. */
	NvdecFrame fitted = frame;
	if (fitted.width > fWidth)
		fitted.width = fWidth;
	if (fitted.height > fHeight)
		fitted.height = fHeight;
	const NvdecFrame& frameRef = fitted;
	bigtime_t convertStart = system_time();
	if (!convert)
		;
	else if (fColorSpace == B_YCbCr422)
		nvdecFrameToYCbCr422Threaded(&frameRef, (uint8*)buffer, fRowBytes);
	else if (fColorSpace == NVDEC_COLOR_SPACE_P010)
		nvdecFrameToP010Threaded(&frameRef, (uint8*)buffer, fRowBytes);
	else if (fColorSpace == NVDEC_COLOR_SPACE_NV12)
		nvdecFrameToNV12Threaded(&frameRef, (uint8*)buffer, fRowBytes);
	else
		nvdecFrameToRGB32Threaded(&frameRef, (uint8*)buffer, fRowBytes, fRange);

	sConvertTime += system_time() - convertStart;
	if (sTiming && ++sTimedFrames % 60 == 0) {
		fprintf(stderr, "nvdec: %d units %.1f ms each in the engine, %d "
			"pictures %.1f ms each converting, %.1f ms each waiting for "
			"chunks\n", sTimedUnits, sEngineTime / 1000.0 / sTimedUnits,
			sTimedFrames, sConvertTime / 1000.0 / sTimedFrames,
			sChunkTime / 1000.0 / sTimedUnits);
		sEngineTime = sConvertTime = sChunkTime = 0;
		sTimedUnits = sTimedFrames = 0;
	}
	mediaHeader->type = B_MEDIA_RAW_VIDEO;
	mediaHeader->start_time = frame.time != 0 ? frame.time : fLastTime;
	mediaHeader->file_pos = 0;
	mediaHeader->orig_size = 0;
	mediaHeader->size_used = fRowBytes * fHeight
		* (fColorSpace == NVDEC_COLOR_SPACE_P010
			|| fColorSpace == NVDEC_COLOR_SPACE_NV12 ? 3 : 2) / 2;
	mediaHeader->u.raw_video.display_line_width = fWidth;
	mediaHeader->u.raw_video.display_line_count = fHeight;
	mediaHeader->u.raw_video.bytes_per_row = fRowBytes;
	mediaHeader->u.raw_video.field_number = 0;
	mediaHeader->u.raw_video.pulldown_number = 0;
	mediaHeader->u.raw_video.first_active_line = 0;
	mediaHeader->u.raw_video.line_count = fHeight;
	fLastTime = mediaHeader->start_time + fFrameTime;
}


bool
NVDecDecoder::_DecodeUnit(const uint8* data, size_t size, bigtime_t time)
{
	bigtime_t at = system_time();
	bool result = fVp9 ? nvdecVp9Decode(fVp9Decoder, data, size, time)
		: fHevc ? nvdecHevcDecode(fHevcDecoder, data, size, time)
		: nvdecH264Decode(fDecoder, data, size, time);
	sEngineTime += system_time() - at;
	sTimedUnits++;
	return result;
}


bool
NVDecDecoder::_NextFrame(NvdecFrame* frame)
{
	if (fVp9)
		return nvdecVp9NextFrame(fVp9Decoder, frame);
	if (fHevc)
		return nvdecHevcNextFrame(fHevcDecoder, frame);
	return nvdecH264NextFrame(fDecoder, frame);
}


void
NVDecDecoder::_ReleaseFrame(const NvdecFrame& frame)
{
	if (fVp9)
		nvdecVp9ReleaseFrame(fVp9Decoder, &frame);
	else if (fHevc)
		nvdecHevcReleaseFrame(fHevcDecoder, &frame);
	else
		nvdecH264ReleaseFrame(fDecoder, &frame);
}


void
NVDecDecoder::_DrainAll()
{
	if (fVp9)
		nvdecVp9DrainAll(fVp9Decoder);
	else if (fHevc)
		nvdecHevcDrainAll(fHevcDecoder);
	else
		nvdecH264DrainAll(fDecoder);
}


const char*
NVDecDecoder::_LastError()
{
	if (fVp9)
		return nvdecVp9LastError(fVp9Decoder);
	if (fHevc)
		return nvdecHevcLastError(fHevcDecoder);
	return nvdecH264LastError(fDecoder);
}


status_t
NVDecDecoder::Decode(void* buffer, int64* frameCount, media_header* mediaHeader,
	media_decode_info* info)
{
	if (fDecoder == NULL && fHevcDecoder == NULL && fVp9Decoder == NULL)
		return B_NO_INIT;

	if (!fSentParameterSets && fParameterSetsSize > 0) {
		_DecodeUnit(fParameterSets, fParameterSetsSize, 0);
		fSentParameterSets = true;
	}

	/* A caller that will not show pictures earlier than some time (the ones
	 * decoded after a seek, from the keyframe up to the target) says so with
	 * a negative time_to_decode, and gets them without the pixel conversion. */
	bigtime_t skipBefore = info != NULL && info->time_to_decode < 0
		? -info->time_to_decode : 0;

	NvdecFrame frame;
	for (;;) {
		if (_NextFrame(&frame)) {
			_Deliver(frame, buffer, mediaHeader,
				skipBefore == 0 || frame.time >= skipBefore);
			_ReleaseFrame(frame);
			*frameCount = 1;
			fFrameNumber++;
			return B_OK;
		}

		const void* chunk = NULL;
		size_t chunkSize = 0;
		media_header chunkHeader;
		bigtime_t chunkStart = system_time();
		status_t status = GetNextChunk(&chunk, &chunkSize, &chunkHeader);
		sChunkTime += system_time() - chunkStart;
		if (status != B_OK) {
			/* The file has ended: let go of everything still held. */
			_DrainAll();
			if (_NextFrame(&frame)) {
				_Deliver(frame, buffer, mediaHeader,
					skipBefore == 0 || frame.time >= skipBefore);
				_ReleaseFrame(frame);
				*frameCount = 1;
				fFrameNumber++;
				return B_OK;
			}
			return status;
		}

		fAccessUnitUsed = 0;
		if (_AppendChunk((const uint8*)chunk, chunkSize) != B_OK)
			return B_NO_MEMORY;
		if (fAccessUnitUsed == 0)
			continue;
		if (!_DecodeUnit(fAccessUnit, fAccessUnitUsed, chunkHeader.start_time)) {
			fprintf(stderr, "nvdec: %s\n", _LastError());
			/* A VP9 stream the engine does not decode (profile 1 or 3,
			 * twelve bits, larger than it goes) will not get better: the
			 * caller should go elsewhere with it. */
			if (fVp9 && nvdecVp9Refused(fVp9Decoder))
				return B_NOT_SUPPORTED;
			/* One bad picture should not end the film. */
			continue;
		}
	}
}


Decoder*
NVDecPlugin::NewDecoder(uint index)
{
	/* Programs that load the add-on by name ask for the decoder of a codec
	 * by its index, and take a NULL as "this add-on has none". */
	if (index >= DECODER_COUNT)
		return NULL;
	return new(std::nothrow) NVDecDecoder();
}


status_t
NVDecPlugin::GetSupportedFormats(media_format** formats, size_t* count)
{
	static media_format sFormats[1];

	// Haiku picks one decoder for a format and does not fall back: without
	// NVIDIA's resource manager (no nvidia_rm, or no card it drives) claim
	// nothing, so H.264 stays with the other decoders.
	if (access("/dev/nvidiactl", F_OK) != 0)
		return B_NOT_SUPPORTED;

	media_format_description description;
	memset(&description, 0, sizeof(description));
	description.family = B_MISC_FORMAT_FAMILY;
	description.u.misc.file_format = 'ffmp';
	description.u.misc.codec = CODEC_ID_H264;

	media_format format;
	memset(&format, 0, sizeof(format));
	format.type = B_MEDIA_ENCODED_VIDEO;
	format.require_flags = 0;
	format.deny_flags = B_MEDIA_MAUI_UNDEFINED_FLAGS;

	BMediaFormats mediaFormats;
	if (mediaFormats.InitCheck() != B_OK)
		return B_ERROR;
	if (mediaFormats.MakeFormatFor(&description, 1, &format) != B_OK)
		return B_ERROR;

	sFormats[0] = format;
	*formats = sFormats;
	*count = 1;
	return B_OK;
}


MediaPlugin*
instantiate_plugin()
{
	return new(std::nothrow) NVDecPlugin();
}
