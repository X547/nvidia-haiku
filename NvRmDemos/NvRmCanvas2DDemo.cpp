// NvRmCanvas2DDemo
//
// The FERMI_TWOD_A blit pipeline from NvRm2dBlitDemo, driven through the
// reusable NvCanvas2D::Canvas instead of open-coded TWOD methods. It loads
// sample.bmp into VRAM, composes a canvas, and saves result.bmp.
//
// Stage layout matches NvRm2dBlitDemo (CE staging stays here in the demo;
// Canvas only records 2D methods):
//   stage 1: CE: inputStage -> dstVram    (paint background canvas)
//   stage 2: CE: inputStage -> srcOpaque  (opaque source for the blits)
//   stage 3: CE: inputStage -> srcPremul  (premultiplied source for blend)
//   stage 4: TWOD via Canvas:
//              - FillRect opaque                (solid border block)
//              - DrawBitmap SRCCOPY 1:1
//              - DrawBitmap bilinear 0.5x downscale
//              - DrawBitmap bilinear 1.5x upscale
//              - DrawBitmap BLEND_PREMULT       (alpha-composited overlay)
//              - FillRect blend                 (alpha-composited solid)
//   stage 5: CE: dstVram -> outputStage   (readback)
//   CPU:      write result.bmp.
//
// Source format is A8R8G8B8; both VRAM surfaces use pitch layout.

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <system_error>
#include <vector>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
#include "class/cl2080_notification.h" // NV2080_NOTIFIERS_FIFO_EVENT_MTHD, NV2080_NOTIFIERS_RC_ERROR
#include "class/cl902d.h"              // FERMI_TWOD_A
#include "class/cla06fsubch.h"         // NVA06F_SUBCHANNEL_2D, NVA06F_SUBCHANNEL_COPY_ENGINE
#include "class/clc5b5.h"              // TURING_DMA_COPY_A
#include "class/clc6b5.h"              // AMPERE_DMA_COPY_A
#include "class/clc7b5.h"              // AMPERE_DMA_COPY_B
#include "class/clc8b5.h"              // HOPPER_DMA_COPY_A
#include "class/clc9b5.h"              // BLACKWELL_DMA_COPY_A
#include "class/clcab5.h"              // BLACKWELL_DMA_COPY_B
#include "ctrl/ctrl0080/ctrl0080gpu.h" // NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2
}

#include <ErrorUtils.h>
#include <FileDesc.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "ImageIo.h"
#include "NvOsEvent.h"
#include "NvGpuMemory.h"
#include "NvChannel.h"
#include "NvCommandBuffer.h"
#include "NvSemSurf.h"
#include "NvCanvas2D.h"


static const NvU32 kCeSubch   = NVA06F_SUBCHANNEL_COPY_ENGINE;
static const NvU32 kTwodSubch = NVA06F_SUBCHANNEL_2D;

static const NvU32 kDstWidth  = 1600;
static const NvU32 kDstHeight = 1200;

static const NvU32 kBpp = 4; // A8R8G8B8


static void FillBackground(NvU8 *pixels, NvU32 w, NvU32 h)
{
	for (NvU32 y = 0; y < h; y++) {
		for (NvU32 x = 0; x < w; x++) {
			NvU8 *p = pixels + (size_t)(y * w + x) * 4;
			p[0] = (NvU8)(80 + (x * 100) / w);              // B
			p[1] = (NvU8)(40 + (y * 120) / h);              // G
			p[2] = (NvU8)(60 + ((x + y) * 60) / (w + h));   // R
			p[3] = 0xFF;
		}
	}
}

// Premultiplied-alpha copy: source RGB scaled by a radial alpha gradient so
// the BLEND_PREMULT blit visibly fades to the canvas at the corners.
static void MakePremultiply(const NvU8 *srcRgba, NvU8 *dstRgba, NvU32 w, NvU32 h)
{
	for (NvU32 y = 0; y < h; y++) {
		for (NvU32 x = 0; x < w; x++) {
			const NvU8 *s = srcRgba + (size_t)(y * w + x) * 4;
			NvU8 *d       = dstRgba + (size_t)(y * w + x) * 4;
			NvU32 a = s[3];
			d[0] = (NvU8)((NvU32)s[0] * a / 255);
			d[1] = (NvU8)((NvU32)s[1] * a / 255);
			d[2] = (NvU8)((NvU32)s[2] * a / 255);
			d[3] = (NvU8)a;
		}
	}
}


static NvU32 PickCopyClass(NvRmDevice &rmDev)
{
	static const NvU32 kCandidates[] = {
		BLACKWELL_DMA_COPY_B,
		BLACKWELL_DMA_COPY_A,
		HOPPER_DMA_COPY_A,
		AMPERE_DMA_COPY_B,
		AMPERE_DMA_COPY_A,
		TURING_DMA_COPY_A,
	};
	NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS params {};
	rmDev.Device().Control(NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &params, sizeof(params));
	for (NvU32 cand : kCandidates) {
		for (NvU32 i = 0; i < params.numClasses; i++) {
			if (params.classList[i] == cand) {
				return cand;
			}
		}
	}
	throw std::system_error(ENOTSUP, std::generic_category(), "no supported DMA copy class");
}

static void RequireClass(NvRmDevice &rmDev, NvU32 wanted, const char *name)
{
	NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS params {};
	rmDev.Device().Control(NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &params, sizeof(params));
	for (NvU32 i = 0; i < params.numClasses; i++) {
		if (params.classList[i] == wanted) {
			return;
		}
	}
	throw std::system_error(ENOTSUP, std::generic_category(),
		std::string("GPU does not advertise ") + name);
}


static void SubmitAndWait(NvChannel &channel, NvCommandBuffer &cmdBuf,
	NvSemSurf &semSurf, NvOsEvent &osEvent, NvU64 slot, NvU64 wSeq)
{
	semSurf.RegisterWaiter(slot, wSeq, 0, (NvU64)osEvent.Fd());

	cmdBuf.SubmitTo(channel);
	while (semSurf.ReadValue(slot) < wSeq) {
		channel.CheckError();
		struct pollfd pfd { .fd = osEvent.Fd(), .events = POLLIN | POLLPRI };
		int pr = poll(&pfd, 1, 5000);
		if (pr < 0) {
			CheckErrno(pr);
		}
	}
}

static void EmitDoneRelease(NvCommandBuffer &cmdBuf, NvSemSurf &semSurf, NvU64 slot, NvU64 wSeq)
{
	semSurf.RecordSubmission(slot, wSeq);
	semSurf.WriteRelease(cmdBuf, slot, wSeq, /*waitForIdle=*/true);
	WriteNonStallInterrupt(cmdBuf);
}


// --------------------------------------------------------------------------

int main(int argc, char **argv)
{
	const char *inputPath  = (argc > 1) ? argv[1] : "sample.bmp";
	const char *outputPath = (argc > 2) ? argv[2] : "result.bmp";

	try {
		std::vector<NvU8> srcCpuOpaque;
		NvU32 srcWidth = 0, srcHeight = 0;
		ReadImageA8R8G8B8(inputPath, srcCpuOpaque, srcWidth, srcHeight);
		std::vector<NvU8> srcCpuPremul(srcCpuOpaque.size());
		MakePremultiply(srcCpuOpaque.data(), srcCpuPremul.data(), srcWidth, srcHeight);

		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		NvChannel channel(rmDev);
		NvCommandBuffer cmdBuf(channel);

		NvU32 copyClass = PickCopyClass(rmDev);
		RequireClass(rmDev, FERMI_TWOD_A, "FERMI_TWOD_A");

		NvRmObject ceObj   = channel.Channel().Alloc(copyClass,    nullptr, 0);
		NvRmObject twodObj = channel.Channel().Alloc(FERMI_TWOD_A, nullptr, 0);

		const NvU64 srcBytes = (NvU64)srcWidth  * srcHeight  * kBpp;
		const NvU64 dstBytes = (NvU64)kDstWidth * kDstHeight * kBpp;
		const NvU64 stageBytes = (dstBytes > srcBytes) ? dstBytes : srcBytes;

		NvGpuMemory inputStage;  // GART, CPU-writable: upload payload
		NvGpuMemory srcOpaque;   // VRAM: original sample image
		NvGpuMemory srcPremul;   // VRAM: premult-radial sample image
		NvGpuMemory dstVram;     // VRAM: blit target / canvas
		NvGpuMemory outputStage; // GART, CPU-readable: readback of canvas

		inputStage .AllocSystem(rmDev, channel.VaSpace(), stageBytes, 0x1000, /*mapCpu=*/true);
		srcOpaque  .AllocLocal (rmDev, channel.VaSpace(), srcBytes,   0x1000);
		srcPremul  .AllocLocal (rmDev, channel.VaSpace(), srcBytes,   0x1000);
		dstVram    .AllocLocal (rmDev, channel.VaSpace(), dstBytes,   0x1000);
		outputStage.AllocSystem(rmDev, channel.VaSpace(), dstBytes,   0x1000, /*mapCpu=*/true);

		NvSemSurf semSurf(rmDev, channel.VaSpace(), 1);
		semSurf.BindChannel(channel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});

		NvOsEvent osEvent(rm);
		NvRmObject hErrorEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_RC_ERROR, /*nonStallIntr=*/false);

		printf("channel:       %#" PRIx32 "\n", channel.Handle());
		printf("ce class:      %#" PRIx32 "\n", copyClass);
		printf("twod class:    %#x  (FERMI_TWOD_A)\n", FERMI_TWOD_A);
		printf("src:           %ux%u A8R8G8B8   gpu=%#" PRIx64 " (opaque)  %#" PRIx64 " (premul)\n",
			srcWidth, srcHeight, srcOpaque.GpuAddr(), srcPremul.GpuAddr());
		printf("dst canvas:    %ux%u A8R8G8B8   gpu=%#" PRIx64 "\n",
			kDstWidth, kDstHeight, dstVram.GpuAddr());
		puts("");

		const NvU32 srcPitch = srcWidth  * kBpp;
		const NvU32 dstPitch = kDstWidth * kBpp;

		NvU64 wSeq = 0;

		// ----- stage 1: paint background into dstVram via CE ----------------
		FillBackground((NvU8*)inputStage.CpuAddr(), kDstWidth, kDstHeight);
		{
			WriteSetObject(cmdBuf, kCeSubch, copyClass);
			WriteCopyDma(cmdBuf, kCeSubch, inputStage.GpuAddr(), dstVram.GpuAddr(), dstBytes);
			EmitDoneRelease(cmdBuf, semSurf, /*slot=*/0, ++wSeq);
			SubmitAndWait(channel, cmdBuf, semSurf, osEvent, 0, wSeq);
		}
		printf("stage 1: background painted (%" PRIu64 " bytes)\n", dstBytes);

		// ----- stage 2: upload opaque source into srcOpaque ----------------
		memcpy(inputStage.CpuAddr(), srcCpuOpaque.data(), srcBytes);
		{
			WriteCopyDma(cmdBuf, kCeSubch, inputStage.GpuAddr(), srcOpaque.GpuAddr(), srcBytes);
			EmitDoneRelease(cmdBuf, semSurf, 0, ++wSeq);
			SubmitAndWait(channel, cmdBuf, semSurf, osEvent, 0, wSeq);
		}
		printf("stage 2: opaque source uploaded\n");

		// ----- stage 3: upload premultiplied source into srcPremul ---------
		memcpy(inputStage.CpuAddr(), srcCpuPremul.data(), srcBytes);
		{
			WriteCopyDma(cmdBuf, kCeSubch, inputStage.GpuAddr(), srcPremul.GpuAddr(), srcBytes);
			EmitDoneRelease(cmdBuf, semSurf, 0, ++wSeq);
			SubmitAndWait(channel, cmdBuf, semSurf, osEvent, 0, wSeq);
		}
		printf("stage 3: premultiplied source uploaded\n");

		// ----- stage 4: 2D composition through NvCanvas2D ------------------
		{
			// The Canvas only records 2D methods; the object bind stays with
			// the caller (thin-emitter contract).
			WriteSetObject(cmdBuf, kTwodSubch, FERMI_TWOD_A);

			NvCanvas2D::Bitmap dstBmp;
			dstBmp.gpuAddr = dstVram.GpuAddr();
			dstBmp.width   = kDstWidth;
			dstBmp.height  = kDstHeight;
			dstBmp.stride  = dstPitch;
			dstBmp.format  = NvCanvas2D::Format::A8R8G8B8;
			dstBmp.layout  = NvCanvas2D::Layout::Pitch;

			NvCanvas2D::Bitmap srcOpaqueBmp;
			srcOpaqueBmp.gpuAddr = srcOpaque.GpuAddr();
			srcOpaqueBmp.width   = srcWidth;
			srcOpaqueBmp.height  = srcHeight;
			srcOpaqueBmp.stride  = srcPitch;

			NvCanvas2D::Bitmap srcPremulBmp = srcOpaqueBmp;
			srcPremulBmp.gpuAddr = srcPremul.GpuAddr();

			NvCanvas2D::Canvas canvas(cmdBuf, dstBmp, kTwodSubch);

			// Opaque solid fill: an orange band along the top of the canvas.
			canvas.FillRect({0, 0, (NvS32)kDstWidth, 24},
				NvCanvas2D::Color{0xFF, 0x80, 0x20, 0xFF});

			auto fullSrc = [&](const NvCanvas2D::Bitmap &b) {
				return NvCanvas2D::Rect{0.0f, 0.0f, (float)b.width, (float)b.height};
			};

			// Blit 1: 1:1 SRCCOPY at top-left, point sampling.
			canvas.DrawBitmap(srcOpaqueBmp,
				NvCanvas2D::Rect{0, 0, (float)srcWidth, (float)srcHeight},
				fullSrc(srcOpaqueBmp), NvCanvas2D::BitmapFilter::Point);

			// Blit 2: 0.5x bilinear downscale at (820, 40).
			canvas.DrawBitmap(srcOpaqueBmp,
				NvCanvas2D::Rect{820, 40, 820 + srcWidth / 2.0f, 40 + srcHeight / 2.0f},
				fullSrc(srcOpaqueBmp), NvCanvas2D::BitmapFilter::Bilinear);

			// Blit 3: 1.5x bilinear upscale at (200, 260).
			canvas.DrawBitmap(srcOpaqueBmp,
				NvCanvas2D::Rect{200, 260, 200 + srcWidth * 1.5f, 260 + srcHeight * 1.5f},
				fullSrc(srcOpaqueBmp), NvCanvas2D::BitmapFilter::Bilinear);

			// Blit 4: 1:1 BLEND_PREMULT overlay at (700, 500).
			canvas.DrawBitmap(srcPremulBmp,
				NvCanvas2D::Rect{700, 500, 700 + (float)srcWidth, 500 + (float)srcHeight},
				fullSrc(srcPremulBmp), NvCanvas2D::BitmapFilter::Point,
				NvCanvas2D::CompositeOp::BlendPremult);

			// Alpha-blended solid fill: a translucent cyan block over the
			// bottom-left, compositing against whatever the blits left there.
			canvas.FillRect({40, 900, 540, 1140},
				NvCanvas2D::Color{0x20, 0xC0, 0xE0, 0x80},
				NvCanvas2D::CompositeOp::BlendPremult);

			// --- solid-prim geometry showcase ---
			using NvCanvas2D::Point;
			const NvCanvas2D::Color white {0xFF, 0xFF, 0xFF, 0xFF};
			const NvCanvas2D::Color red   {0xFF, 0x30, 0x30, 0xFF};

			// Diagonal crosshair lines across the whole canvas.
			canvas.StrokeLine({0, 0}, {(NvS32)kDstWidth - 1, (NvS32)kDstHeight - 1}, white);
			canvas.StrokeLine({(NvS32)kDstWidth - 1, 0}, {0, (NvS32)kDstHeight - 1}, white);

			// Filled triangle pointer near the center.
			canvas.FillTriangle({760, 120}, {900, 120}, {830, 230}, red);

			// Closed zig-zag polyline (a diamond outline) bottom-right.
			const Point diamond[] = {
				{1200, 700}, {1320, 820}, {1200, 940}, {1080, 820},
			};
			canvas.StrokePolyline(diamond, 4, white, /*closed=*/true);

			// Clipped fill: only the intersection with the clip rect is drawn.
			canvas.SetClipRect({1000, 1000, 1400, 1150});
			canvas.FillRect({900, 950, 1500, 1180}, NvCanvas2D::Color{0xFF, 0xC0, 0x00, 0xFF});
			canvas.ClearClip();

			// ROP3 DSTINVERT (0x55) over a band — inverts whatever is under it.
			canvas.SetRop(0x55);
			canvas.FillRect({40, 40, 360, 120}, white, NvCanvas2D::CompositeOp::Rop);

			EmitDoneRelease(cmdBuf, semSurf, 0, ++wSeq);
			SubmitAndWait(channel, cmdBuf, semSurf, osEvent, 0, wSeq);
		}
		printf("stage 4: canvas composed (fills + blits + lines/triangle/polyline + clip + rop)\n");

		// ----- stage 5: download the canvas to outputStage -----------------
		{
			WriteCopyDma(cmdBuf, kCeSubch, dstVram.GpuAddr(), outputStage.GpuAddr(), dstBytes);
			EmitDoneRelease(cmdBuf, semSurf, 0, ++wSeq);
			SubmitAndWait(channel, cmdBuf, semSurf, osEvent, 0, wSeq);
		}
		printf("stage 5: canvas downloaded\n");

		WriteImageA8R8G8B8(outputPath, kDstWidth, kDstHeight, (const NvU8*)outputStage.CpuAddr());
		printf("\nwrote %s (%ux%u)\n", outputPath, kDstWidth, kDstHeight);

		semSurf.UnbindChannel(channel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});
		return 0;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmCanvas2DDemo: %s\n", ex.what());
		return 1;
	} catch (const std::exception &ex) {
		fprintf(stderr, "[!] NvRmCanvas2DDemo: %s\n", ex.what());
		return 1;
	}
}
