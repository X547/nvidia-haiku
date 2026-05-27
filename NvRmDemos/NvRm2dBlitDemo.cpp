// NvRm2dBlitDemo
//
// FERMI_TWOD_A demo: load sample.bmp into VRAM, compose four blits onto a
// VRAM canvas — pixel-perfect SRCCOPY, bilinear downscale, bilinear
// upscale, and premultiplied-alpha BLEND — then download the canvas and
// save it as result.bmp.
//
// VRAM buffers are never CPU-mapped. All data movement between system
// memory and VRAM is staged through CPU-mapped GART (sysmem) buffers and
// driven by TURING_DMA_COPY_A (or newer) on the same graphics-engine
// GPFIFO channel that issues the 2D methods. The pipeline is broken into
// five submissions, each signaling a semsurf slot via a WFI release:
//
//   stage 1: CPU paints inputStage with a gradient background.
//            CE: inputStage   -> dstVram     (paint background canvas)
//   stage 2: CPU loads sample.bmp into inputStage.
//            CE: inputStage   -> srcOpaque   (opaque source for blits 1-3)
//   stage 3: CPU rewrites inputStage with the same image but with RGB
//            premultiplied by a radial alpha gradient.
//            CE: inputStage   -> srcPremul   (premultiplied source for blit 4)
//   stage 4: TWOD: srcOpaque  -> dstVram     (three blits, different scales)
//            TWOD: srcPremul  -> dstVram     (BLEND_PREMULT over existing)
//   stage 5: CE: dstVram      -> outputStage (readback)
//   CPU:      write result.bmp from outputStage.
//
// Source format is A8R8G8B8 (matches a 32-bit BGRA BMP exactly when read
// as a little-endian NvU32 — no per-pixel swizzling). Both VRAM surfaces
// use pitch layout for simplicity.

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
#include "class/cl902d.h"              // FERMI_TWOD_A + NV902D_* methods
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


// Subchannel layout follows the standard cla06fsubch.h assignments
// (CE on 4, 2D on 3). The two engines are routed independently via
// SET_OBJECT, so they can interleave methods on the same GPFIFO channel
// without explicit handoff between them.
static const NvU32 kCeSubch   = NVA06F_SUBCHANNEL_COPY_ENGINE;
static const NvU32 kTwodSubch = NVA06F_SUBCHANNEL_2D;

// Output canvas size. Picked so each of the four blits lands in a
// distinct region and at least one overlaps the others (the alpha blit),
// giving a single visual comparison of all four operations.
static const NvU32 kDstWidth  = 1600;
static const NvU32 kDstHeight = 1200;

static const NvU32 kBpp = 4; // A8R8G8B8


// ----- Pattern generators --------------------------------------------------

// Background canvas: a smooth two-axis gradient so we can tell where the
// blits actually landed. Top-left is dark blue, bottom-right is warm.
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

// Build a premultiplied-alpha copy of `srcRgba` into `dstRgba`, using
// the source's existing per-pixel alpha unchanged. BLEND_PREMULT's input
// requirement (dst = src + (1 - src.a) * dst) is that the source RGB
// already carries the alpha modulation, which is what we produce here.
// Alpha bytes are copied through verbatim — the BMP's own alpha channel
// determines the blend.
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


// ----- TWOD push helpers ---------------------------------------------------

// Set the constant blend factors to "fully saturated" so BLEND_PREMULT
// uses the per-pixel source alpha unmodified (otherwise BETA1/BETA4
// default to 0 and the blend collapses to dst = dst or dst = src). The
// 2D engine's blend pipeline is:
//   dst = src * Kdst' + dst * (1 - Kdst')      // BLEND_PREMULT
// where Kdst' folds in BETA1 (8-bit global alpha at bits 30:23 — see
// mesa's nv50_2d.xml.h NV50_2D_BETA1_BETA1__MASK = 0x7f800000) and
// BETA4 (per-channel RGBA constant). Setting both to max == identity
// scaling, so the per-pixel source alpha drives the blend on its own.
static void TwodSetupBlendConstants(NvCommandBuffer &cmdBuf)
{
	NvPushWriter wr = cmdBuf.Push(4);
	wr.IncMethod(kTwodSubch, NV902D_SET_BETA1, 1);
	wr.U32(0x7F800000);

	wr.IncMethod(kTwodSubch, NV902D_SET_BETA4, 1);
	wr.U32(0xFFFFFFFFu);
	cmdBuf.EndPush(wr);
}

// Configure the destination surface once. All blits in the demo target
// the same canvas so this only fires once per channel binding.
static void TwodSetupDst(NvCommandBuffer &cmdBuf, NvU64 dstGpuAddr, NvU32 dstPitch, NvU32 dstW, NvU32 dstH)
{
	NvPushWriter wr = cmdBuf.Push(11);
	wr.IncMethod(kTwodSubch, NV902D_SET_DST_FORMAT, 1);
	wr.U32(NV902D_SET_DST_FORMAT_V_A8R8G8B8);

	wr.IncMethod(kTwodSubch, NV902D_SET_DST_MEMORY_LAYOUT, 1);
	wr.U32(NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH);

	wr.IncMethod(kTwodSubch, NV902D_SET_DST_PITCH, 1);
	wr.U32(dstPitch);

	// SET_DST_WIDTH + HEIGHT + OFFSET_UPPER + OFFSET_LOWER (contiguous)
	wr.IncMethod(kTwodSubch, NV902D_SET_DST_WIDTH, 4);
	wr.U32(dstW);
	wr.U32(dstH);
	wr.U32((NvU32)(dstGpuAddr >> 32));
	wr.U32((NvU32)(dstGpuAddr & 0xFFFFFFFFu));
	cmdBuf.EndPush(wr);
}

// Rebind the source surface. Called between groups of blits whenever we
// switch which VRAM image is being read (e.g. opaque -> premultiplied).
static void TwodSetupSrc(NvCommandBuffer &cmdBuf, NvU64 srcGpuAddr, NvU32 srcPitch, NvU32 srcW, NvU32 srcH)
{
	NvPushWriter wr = cmdBuf.Push(11);
	wr.IncMethod(kTwodSubch, NV902D_SET_SRC_FORMAT, 1);
	wr.U32(NV902D_SET_SRC_FORMAT_V_A8R8G8B8);

	wr.IncMethod(kTwodSubch, NV902D_SET_SRC_MEMORY_LAYOUT, 1);
	wr.U32(NV902D_SET_SRC_MEMORY_LAYOUT_V_PITCH);

	wr.IncMethod(kTwodSubch, NV902D_SET_SRC_PITCH, 1);
	wr.U32(srcPitch);

	wr.IncMethod(kTwodSubch, NV902D_SET_SRC_WIDTH, 4);
	wr.U32(srcW);
	wr.U32(srcH);
	wr.U32((NvU32)(srcGpuAddr >> 32));
	wr.U32((NvU32)(srcGpuAddr & 0xFFFFFFFFu));
	cmdBuf.EndPush(wr);
}

// Emit a single blit. `op` is one of NV902D_SET_OPERATION_V_* and
// determines whether the blit overwrites (SRCCOPY) or alpha-blends
// (BLEND_PREMULT) the destination. duDx64 / dvDy64 are 32.32 fixed-point
// per-dst-pixel steps in source-pixel units; 1ull<<32 means "1.0 source
// pixel per 1 dst pixel" (1:1 copy).
//
// For a pixel-clean 1:1 SRCCOPY (step == 1.0) use point sampling;
// bilinear at 1:1 with ORIGIN_CORNER walks fractional neighbors and
// softens the image. Bilinear is meaningful for scaled blits only.
static void Twod2dBlit(NvCommandBuffer &cmdBuf,
	NvU32 op, bool bilinear,
	NvU32 dstX, NvU32 dstY, NvU32 dstW, NvU32 dstH,
	NvU64 duDx64, NvU64 dvDy64,
	NvU32 srcX, NvU32 srcY)
{
	NvPushWriter wr = cmdBuf.Push(17);
	wr.IncMethod(kTwodSubch, NV902D_SET_OPERATION, 1);
	wr.U32(op);

	// DRF_DEF doesn't compile under C++: GCC treats the `902D` class
	// token as a user-defined-literal pp-number (suffix `D`) before the
	// macro can paste it. Build the SAMPLE_MODE register value manually
	// from the already-resolved NV902D_* constants. Bit layout per
	// cl902d.h: ORIGIN at 0:0, FILTER at 4:4.
	// ORIGIN_CORNER: source coord (0,0) is the top-left corner of the
	// top-left source pixel — the convention image-blit users (incl.
	// Mesa headsurface for non-shifted scaling) expect.
#if 0
	wr.IncMethod(kTwodSubch, NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, 1);
	wr.U32(
		(bilinear
			? DRF_DEF(902D, _SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, _FILTER, _BILINEAR)
			: DRF_DEF(902D, _SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, _FILTER, _POINT)) |
		DRF_DEF(902D, _SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, _ORIGIN, _CORNER));
#endif
	const NvU32 sampleMode =
		(NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_ORIGIN_CORNER << 0) |
		((bilinear
			? NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_FILTER_BILINEAR
			: NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_FILTER_POINT) << 4);
	wr.IncMethod(kTwodSubch, NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, 1);
	wr.U32(sampleMode);

	// 12 contiguous methods starting at SET_PIXELS_FROM_MEMORY_DST_X0;
	// writing the final word (PIXELS_FROM_MEMORY_SRC_Y0_INT) triggers
	// the blit on the engine.
	wr.IncMethod(kTwodSubch, NV902D_SET_PIXELS_FROM_MEMORY_DST_X0, 12);
	wr.U32(dstX);
	wr.U32(dstY);
	wr.U32(dstW);
	wr.U32(dstH);
	wr.U32((NvU32)(duDx64 & 0xFFFFFFFFu)); // DU_DX_FRAC
	wr.U32((NvU32)(duDx64 >> 32));         // DU_DX_INT
	wr.U32((NvU32)(dvDy64 & 0xFFFFFFFFu)); // DV_DY_FRAC
	wr.U32((NvU32)(dvDy64 >> 32));         // DV_DY_INT
	wr.U32(0);                              // SRC_X0_FRAC
	wr.U32(srcX);                           // SRC_X0_INT
	wr.U32(0);                              // SRC_Y0_FRAC
	wr.U32(srcY);                           // SRC_Y0_INT
	cmdBuf.EndPush(wr);
}


// ----- Misc ----------------------------------------------------------------

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

// 32.32 fixed-point per-dst-pixel step in source-pixel units: src/dst.
// With ORIGIN_CORNER the engine samples src at srcStart + i * step for
// dst pixel i, so an exact ratio gives geometric correctness regardless
// of scale factor.
static NvU64 ScaleStep(NvU32 srcSize, NvU32 dstSize)
{
	return ((NvU64)srcSize << 32) / dstSize;
}


// Submit the methods already written to `cmdBuf`, blocking the caller via
// poll() until the trailing sem release lands.
static void SubmitAndWait(NvChannel &channel, NvCommandBuffer &cmdBuf,
	NvSemSurf &semSurf, NvOsEvent &osEvent, NvU64 slot, NvU64 wSeq)
{
	// RM only fires the OS-event fd when a registered waiter on a
	// specific (slot, value) matches the GPU's sem release. Without
	// this call the release still happens but no waiter is matched, so
	// poll() blocks until its own timeout — the GPU work itself runs
	// fine, which is misleading. RecordSubmission (called inside
	// EmitDoneRelease) only feeds the 32-bit-sem CPU-side tracking and
	// is unrelated to the wakeup path.
	semSurf.RegisterWaiter(slot, wSeq, 0, (NvU64)osEvent.Fd());

	cmdBuf.SubmitTo(channel);
	while (semSurf.ReadValue(slot) < wSeq) {
		// A bad address or method into either CE or TWOD shows up as a
		// channel kill; without this check the poll loop would hang on
		// a sem the GPU is never going to write.
		channel.CheckError();
		struct pollfd pfd { .fd = osEvent.Fd(), .events = POLLIN | POLLPRI };
		int pr = poll(&pfd, 1, 5000);
		if (pr < 0) {
			CheckErrno(pr);
		}
	}
}

// Append a sem release WFI for `slot` at value `wSeq`, accommodating the
// 32-bit-semaphore path on pre-Volta hardware via RecordSubmission.
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
		// Load source image first so a malformed input fails before we
		// touch any GPU state.
		std::vector<NvU8> srcCpuOpaque;
		NvU32 srcWidth = 0, srcHeight = 0;
		ReadImageA8R8G8B8(inputPath, srcCpuOpaque, srcWidth, srcHeight);
		std::vector<NvU8> srcCpuPremul(srcCpuOpaque.size());
		MakePremultiply(srcCpuOpaque.data(), srcCpuPremul.data(), srcWidth, srcHeight);

		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		// Channel binds to GRAPHICS so we can SET_OBJECT both FERMI_TWOD_A
		// (GR engine class) and a TURING_DMA_COPY_A-or-newer object on it.
		NvChannel channel(rmDev);
		NvCommandBuffer cmdBuf(channel);

		NvU32 copyClass = PickCopyClass(rmDev);
		RequireClass(rmDev, FERMI_TWOD_A, "FERMI_TWOD_A");

		NvRmObject ceObj   = channel.Channel().Alloc(copyClass,    nullptr, 0);
		NvRmObject twodObj = channel.Channel().Alloc(FERMI_TWOD_A, nullptr, 0);

		const NvU64 srcBytes = (NvU64)srcWidth  * srcHeight  * kBpp;
		const NvU64 dstBytes = (NvU64)kDstWidth * kDstHeight * kBpp;
		// inputStage has to be the bigger of the two so it can carry
		// either an entire upload payload (background = dstBytes) or
		// the source image (srcBytes).
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

		// Single-slot semsurf is enough because we serialize all phases
		// CPU-side (each SubmitAndWait blocks before the next queues).
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
		printf("inputStage:    %" PRIu64 " bytes (sysmem, cpu-mapped)\n", inputStage .Size());
		printf("outputStage:   %" PRIu64 " bytes (sysmem, cpu-mapped)\n", outputStage.Size());
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
			// CE object stays bound on kCeSubch across submissions on the
			// same channel; no need to re-SET_OBJECT.
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

		// ----- stage 4: 2D blits onto dstVram ------------------------------
		{
			// Bind FERMI_TWOD_A on its own subchannel and configure the
			// destination once — every blit writes to the same canvas.
			WriteSetObject(cmdBuf, kTwodSubch, FERMI_TWOD_A);
			TwodSetupBlendConstants(cmdBuf);
			TwodSetupDst(cmdBuf, dstVram.GpuAddr(), dstPitch, kDstWidth, kDstHeight);

			// --- blits 1..3 read from the opaque source ---
			TwodSetupSrc(cmdBuf, srcOpaque.GpuAddr(), srcPitch, srcWidth, srcHeight);

			// Blit 1: 1:1 SRCCOPY at top-left. Point sampling — bilinear
			// at 1:1 with ORIGIN_CORNER blurs every pixel.
			Twod2dBlit(cmdBuf,
				NV902D_SET_OPERATION_V_SRCCOPY, /*bilinear=*/false,
				/*dstXY*/ 0, 0, /*dstWH*/ srcWidth, srcHeight,
				/*du,dv*/ 1ull << 32, 1ull << 32,
				/*srcXY*/ 0, 0);

			// Blit 2: 0.5x bilinear downscale into a 400x300 region at
			// (820, 40). step = 2.0 src pixels per dst pixel.
			{
				const NvU32 outW = srcWidth  / 2;
				const NvU32 outH = srcHeight / 2;
				Twod2dBlit(cmdBuf,
					NV902D_SET_OPERATION_V_SRCCOPY, /*bilinear=*/true,
					/*dstXY*/ 820, 40, /*dstWH*/ outW, outH,
					/*du,dv*/ ScaleStep(srcWidth, outW), ScaleStep(srcHeight, outH),
					/*srcXY*/ 0, 0);
			}

			// Blit 3: 1.5x bilinear upscale into a 1200x900 region at
			// (200, 260). step = srcSize / (srcSize * 3/2) = 2/3.
			{
				const NvU32 outW = (srcWidth  * 3) / 2;
				const NvU32 outH = (srcHeight * 3) / 2;
				Twod2dBlit(cmdBuf,
					NV902D_SET_OPERATION_V_SRCCOPY, /*bilinear=*/true,
					/*dstXY*/ 200, 260, /*dstWH*/ outW, outH,
					/*du,dv*/ ScaleStep(srcWidth, outW), ScaleStep(srcHeight, outH),
					/*srcXY*/ 0, 0);
			}

			// --- blit 4 reads from the premultiplied source, alpha-blends ---
			TwodSetupSrc(cmdBuf, srcPremul.GpuAddr(), srcPitch, srcWidth, srcHeight);

			// Blit 4: 1:1 BLEND_PREMULT on top of whatever is already at
			// (700, 500). Combined with the radial alpha gradient this
			// produces a vignetted overlay that fades back to the canvas
			// at the corners — the visible proof that the alpha channel
			// participated in the operation.
			Twod2dBlit(cmdBuf,
				NV902D_SET_OPERATION_V_BLEND_PREMULT, /*bilinear=*/false,
				/*dstXY*/ 700, 500, /*dstWH*/ srcWidth, srcHeight,
				/*du,dv*/ 1ull << 32, 1ull << 32,
				/*srcXY*/ 0, 0);

			EmitDoneRelease(cmdBuf, semSurf, 0, ++wSeq);
			SubmitAndWait(channel, cmdBuf, semSurf, osEvent, 0, wSeq);
		}
		printf("stage 4: 4 blits issued (SRCCOPY, bilinear 0.5x, bilinear 1.5x, BLEND_PREMULT)\n");

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
		fprintf(stderr, "[!] NvRm2dBlitDemo: %s\n", ex.what());
		return 1;
	} catch (const std::exception &ex) {
		fprintf(stderr, "[!] NvRm2dBlitDemo: %s\n", ex.what());
		return 1;
	}
}
