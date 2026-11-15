/* H.264, H.265 and VP9 on the graphics card.
 *
 * Haiku picks one decoder for a format, by add-on directory, so this one takes
 * the codecs it offers away from libavcodec wherever it is installed. It
 * decodes what the card's engine can and refuses the rest rather than
 * producing a wrong picture; a refused stream does not play, since nothing
 * falls back.
 *
 * All three codecs are offered to the media kit unless the environment
 * variable NVDEC_CODECS lists fewer (for example NVDEC_CODECS=h264). H.264 is
 * decoded in eight bit 4:2:0, progressive; H.265 Main and Main 10, 4:2:0; VP9
 * profile 0 (profile 2 only with NVDEC_VP9_TEN_BIT, see nvdec_vp9.h). Ten-bit
 * pictures can be had as they are, as NVDEC_COLOR_SPACE_P010. Any picture can
 * be had as NVDEC_COLOR_SPACE_NV12, without a conversion.
 *
 * NewDecoder() knows indexes 0 to 2 (H.264, H.265, VP9 - any of them takes
 * any of the codecs) and returns NULL for the rest, so that a program can
 * tell an add-on that decodes VP9 from one before it: index 2 is VP9's.
 */
#ifndef NVDEC_PLUGIN_H
#define NVDEC_PLUGIN_H

#include <OS.h>

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
			NvdecFrame			_Prepare(const NvdecFrame& frame, bool convert,
									void** pin);
			void				_Deliver(const NvdecFrame& frame, void* buffer,
									media_header* mediaHeader, bool convert,
									void* pin);
			status_t			_DeliverNext(void* buffer, int64* frameCount,
									media_header* mediaHeader, bigtime_t skipBefore,
									bool overlap);

			/* Decoding ahead: while a picture is converted on the caller's
			 * thread, the next access unit is decoded on this one. */
			void				_DecodeNextUnit();
			void				_StartDecodingAhead();
			void				_FinishDecodingAhead();
	static	status_t			_DecodeAheadThread(void* cookie);
			void				_StopDecodingAhead();

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

			/* What decoding the next unit found, for the next Decode(). */
			status_t			fChunkStatus;	/* not B_OK once the chunks end */
			status_t			fFatalStatus;	/* not B_OK: give up */
			bool				fRefused;		/* the engine will not decode
												   this stream */

			/* The threads converting pictures, made with the first one
			 * converted on the CPU. */
			NvdecConvertPool*	fConvertPool;

			bool				fOverlap;
			thread_id			fAheadThread;
			sem_id				fAheadStart;
			sem_id				fAheadDone;
			bool				fAheadQuit;
};


class NVDecPlugin : public DecoderPlugin {
public:
	virtual	Decoder*			NewDecoder(uint index);
	virtual	status_t			GetSupportedFormats(media_format** formats,
									size_t* count);
};

#endif	// NVDEC_PLUGIN_H
