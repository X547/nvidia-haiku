/* H.264, H.265 and VP9 on the graphics card.
 *
 * Haiku picks one decoder for a format, by add-on directory, so this one takes
 * H.264 away from libavcodec wherever it is installed. It decodes what the
 * card's engine can - eight bit 4:2:0, progressive - and refuses the rest
 * rather than producing a wrong picture.
 *
 * H.265 and VP9 are not offered to the media kit, because a stream this
 * cannot decode would then not play at all: a program that wants them - one
 * that falls back to software on its own - instantiates the decoder and sets
 * it up with an H.265 or VP9 format. H.265 Main and Main 10 and VP9 profile 0
 * are decoded (VP9 profile 2 not on the GP104, see nvdec_vp9.h). Ten-bit
 * pictures can be had as they are, as NVDEC_COLOR_SPACE_P010. Any picture can
 * be had as NVDEC_COLOR_SPACE_NV12, without a conversion.
 *
 * NewDecoder() knows indexes 0 to 2 (H.264, H.265, VP9 - any of them takes
 * any of the codecs) and returns NULL for the rest, so that a program can
 * tell an add-on that decodes VP9 from one before it: index 2 is VP9's.
 */
#ifndef NVDEC_PLUGIN_H
#define NVDEC_PLUGIN_H

#include "DecoderPlugin.h"

extern "C" {
#include "nvdec_convert.h"
#include "nvdec_h264.h"
#include "nvdec_hevc.h"
#include "nvdec_vp9.h"
}

/* Not one of Haiku's: a plane of sixteen-bit luma samples with the value in
 * the top bits, then one of Cb and Cr in pairs with half as many lines, both
 * bytes_per_row bytes a line. Only given to a caller that asks for it. */
#define NVDEC_COLOR_SPACE_P010	((color_space)0x50303130)	/* 'P010' */

/* Not one of Haiku's either: NV12, a plane of eight-bit luma samples, then
 * one of Cb and Cr in pairs with half as many lines, both bytes_per_row bytes
 * a line - what a graphics card takes as a video texture. Every codec;
 * ten-bit pictures keep their top eight bits. */
#define NVDEC_COLOR_SPACE_NV12	((color_space)0x4e563132)	/* 'NV12' */


class NVDecDecoder : public Decoder {
public:
								NVDecDecoder();
	virtual						~NVDecDecoder();

	virtual	void				GetCodecInfo(media_codec_info* info);
	virtual	status_t			Setup(media_format* ioEncodedFormat,
									const void* infoBuffer, size_t infoSize);
	virtual	status_t			NegotiateOutputFormat(
									media_format* ioDecodedFormat);
	virtual	status_t			SeekedTo(int64 frame, bigtime_t time);
	virtual	status_t			Decode(void* buffer, int64* frameCount,
									media_header* mediaHeader,
									media_decode_info* info);

private:
			status_t			_ReadParameterSets(const uint8* data, size_t size);
			status_t			_AppendChunk(const uint8* data, size_t size);
			void				_Deliver(const NvdecFrame& frame, void* buffer,
									media_header* mediaHeader, bool convert = true);

			bool				_DecodeUnit(const uint8* data, size_t size,
									bigtime_t time);
			bool				_NextFrame(NvdecFrame* frame);
			void				_ReleaseFrame(const NvdecFrame& frame);
			void				_DrainAll();
			const char*			_LastError();

			NvdecEngine*		fEngine;
			bool				fHevc;
			bool				fVp9;
			NvdecH264*			fDecoder;
			NvdecHevc*			fHevcDecoder;
			NvdecVp9*			fVp9Decoder;
			char				fReason[256];

			/* A chunk from an MPEG-4 file is a run of length-prefixed units
			 * rather than one with start codes; this says how long the
			 * prefix is, or zero when the stream already has start codes. */
			int					fLengthSize;
			uint8*				fAccessUnit;
			size_t				fAccessUnitSize;
			size_t				fAccessUnitUsed;
			uint8*				fParameterSets;
			size_t				fParameterSetsSize;
			bool				fSentParameterSets;

			media_format		fInputFormat;
			int					fWidth;
			int					fHeight;
			uint32				fColorSpace;
			size_t				fRowBytes;
			bigtime_t			fFrameTime;
			bigtime_t			fLastTime;
			int64				fFrameNumber;
			NvdecColorRange		fRange;
			int					fBitDepth;
};


class NVDecPlugin : public DecoderPlugin {
public:
	virtual	Decoder*			NewDecoder(uint index);
	virtual	status_t			GetSupportedFormats(media_format** formats,
									size_t* count);
};

#endif	// NVDEC_PLUGIN_H
