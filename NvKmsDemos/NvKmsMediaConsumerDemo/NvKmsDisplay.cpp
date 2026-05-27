#include "NvKmsDisplay.h"

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <system_error>

extern "C" {
#include "nvkms-ioctl.h"
#include "nvkms-api.h"
#include "nvkms-sync.h"
#include "nv-haiku.h"

#include "nvos.h"
#include "class/cl50a0.h" // NV50_MEMORY_VIRTUAL
#include "class/cl2080_notification.h" // NV2080_NOTIFIERS_FIFO_EVENT_MTHD, NV2080_NOTIFIERS_RC_ERROR
#include "class/cl902d.h" // FERMI_TWOD_A
#include "class/cla06fsubch.h" // NVA06F_SUBCHANNEL_2D
#include "ctrl/ctrl0080/ctrl0080gpu.h" // NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2
}

#include "ErrorUtils.h"

#include "NvUtils.h"


static const NvKmsNIsoFormat kNotifierFormat = NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY;
static const NvU64 kNotifierMemSize = 4096;
static const NvU32 kNumBuffers = 2;
static const NvU32 kTwodSubch = NVA06F_SUBCHANNEL_2D;


static inline size_t AlignUp(size_t val, size_t align)
{
	return (val + (align - 1)) / align * align;
}


static NvU32 NotifierSlotOffsetInWords(NvU32 slot)
{
	return slot * (nvKmsSizeOfNotifier(kNotifierFormat, false) / (NvU32)sizeof(NvU32));
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
// FERMI_TWOD_A samples src at srcStart + i * step for dst pixel i with
// ORIGIN_CORNER, so an exact ratio gives geometric correctness regardless
// of scale factor.
static NvU64 ScaleStep(NvU32 srcSize, NvU32 dstSize)
{
	return ((NvU64)srcSize << 32) / dstSize;
}


NvKmsDisplay::NvKmsDisplay():
	fDevFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC)),
	fRmDev(fRm, 0),
	fKmsDev(fKms, 0),
	fDeviceHandle(0),
	fDispHandle(0),
	fHead(0),
	fWidth(0),
	fHeight(0),
	fRefreshRateMilliHz(0),
	// Default engine is GRAPHICS — required to SET_OBJECT FERMI_TWOD_A.
	fChannel(fRmDev),
	fCmdBuf(fChannel),
	fBlitEvent(fRm),
	fBlitSeq(0),
	fFramebufferGpuAddr{0, 0},
	fQueuedBuffer(0),
	fFlipCounter(0),
	fOwnershipHeld(false)
{
	CheckErrno(fDevFd.Get());

	if (fKmsDev.Info().numDisps == 0) {
		RaiseErrno(ENODEV);
	}
	fDeviceHandle = fKmsDev.Info().deviceHandle;
	fDispHandle = fKmsDev.Info().dispHandles[0];

	{
		NvKmsGrabOwnershipParams params {};
		params.request.deviceHandle = fDeviceHandle;
		fKms.Control(NVKMS_IOCTL_GRAB_OWNERSHIP, &params, sizeof(params));
		fOwnershipHeld = true;
	}
	{
		NvKmsDeclareEventInterestParams params {};
		params.request.interestMask = 1U << NVKMS_EVENT_TYPE_FLIP_OCCURRED;
		fKms.Control(NVKMS_IOCTL_DECLARE_EVENT_INTEREST, &params, sizeof(params));
	}

	RequireClass(fRmDev, FERMI_TWOD_A, "FERMI_TWOD_A");
	fTwodObj = fChannel.Channel().Alloc(FERMI_TWOD_A, nullptr, 0);

	fBlitSem = NvSemSurf(fRmDev, fChannel.VaSpace(), 1);
	fBlitSem.BindChannel(fChannel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});

	// RC_ERROR wake-up: if the channel faults the blit semaphore will
	// never advance, so we want poll() to return promptly with an error
	// rather than waiting on the per-poll timeout indefinitely.
	fBlitErrorEvent = AllocSubdeviceOsEvent(fRmDev, fBlitEvent,
		NV2080_NOTIFIERS_RC_ERROR, /*nonStallIntr=*/false);
}


NvKmsDisplay::~NvKmsDisplay()
{
	// Tear down the per-VRAM-buffer GPU virtual mappings before the
	// fFramebuffer destructors free the physical VRAM. UnmapMemoryDma needs
	// both the virt handle and the phys handle live.
	NvRmApi &rm = *fRmDev.Device().Rm();
	for (NvU32 i = 0; i < kNumBuffers; i++) {
		if (fFramebufferGpuAddr[i] != 0 && fFramebufferVirt[i].Get() != 0) {
			try {
				rm.UnmapMemoryDma(fRmDev.Device().Get(),
					fFramebufferVirt[i].Get(),
					fFramebuffer[i].Memory().Get(),
					0, fFramebufferGpuAddr[i]);
			} catch (...) {
				// Best-effort.
			}
			fFramebufferGpuAddr[i] = 0;
		}
	}
	if (fOwnershipHeld) {
		try {
			NvKmsReleaseOwnershipParams params {};
			params.request.deviceHandle = fDeviceHandle;
			fKms.Control(NVKMS_IOCTL_RELEASE_OWNERSHIP, &params, sizeof(params));
		} catch (...) {
			// Best-effort cleanup; the kernel will reclaim on close anyway.
		}
		fOwnershipHeld = false;
	}
}


bool NvKmsDisplay::QueryConnected(NVDpyId dpyId)
{
	NvKmsQueryDpyDynamicDataParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.dispHandle = fDispHandle;
	params.request.dpyId = dpyId;
	fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params));
	return params.reply.connected;
}


bool NvKmsDisplay::QueryPreferredMode(NVDpyId dpyId, NvKmsMode &outMode)
{
	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = fDeviceHandle;
		params.request.dispHandle = fDispHandle;
		params.request.dpyId = dpyId;
		params.request.modeIndex = i;
		fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params));
		if (params.reply.end) {
			return false;
		}
		if (params.reply.preferredMode) {
			outMode = params.reply.mode;
			return true;
		}
	}
}


bool NvKmsDisplay::PickConnectedDpy(NVDpyId &outDpyId, NvKmsMode &outMode)
{
	NVDpyIdList validDpys;
	{
		NvKmsQueryDispParams params {};
		params.request.deviceHandle = fDeviceHandle;
		params.request.dispHandle = fDispHandle;
		fKms.Control(NVKMS_IOCTL_QUERY_DISP, &params, sizeof(params));
		validDpys = params.reply.validDpys;
	}

	NVDpyId cur = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(), validDpys);
	while (!nvDpyIdIsInvalid(cur)) {
		if (QueryConnected(cur) && QueryPreferredMode(cur, outMode)) {
			outDpyId = cur;
			return true;
		}
		cur = nvNextDpyIdInDpyIdListUnsorted(cur, validDpys);
	}
	return false;
}


void NvKmsDisplay::AllocateNotifier()
{
	NvU8 compressible = 0;
	nvKmsKapiAllocateSystemMemory(fRmDev, fKmsDev, fNotifierMem,
		NvKmsSurfaceMemoryLayoutPitch, kNotifierMemSize,
		NVKMS_KAPI_ALLOCATION_TYPE_NOTIFIER, &compressible);

	fNotifierMap = fRmDev.MapMemory(fNotifierMem.Get(), true, 0, kNotifierMemSize, 0);

	FileDesc memoryFd = fRmDev.ExportObjectToFd(fRmDev.Device().Get(), fNotifierMem.Get());

	NvKmsRegisterSurfaceParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.useFd = true;
	params.request.planes[0].u.fd = memoryFd.Get();
	params.request.planes[0].pitch = kNotifierMemSize;
	params.request.planes[0].rmObjectSizeInBytes = kNotifierMemSize;
	params.request.widthInPixels = kNotifierMemSize;
	params.request.heightInPixels = 1;
	params.request.layout = NvKmsSurfaceMemoryLayoutPitch;
	params.request.format = NvKmsSurfaceMemoryFormatI8;
	params.request.isoType = NVKMS_MEMORY_NISO;
	fKms.Control(NVKMS_IOCTL_REGISTER_SURFACE, &params, sizeof(params));
	fNotifierSurface = NvKmsSurface(fKmsDev, params.reply.surfaceHandle);

	for (NvU32 i = 0; i < kNumBuffers; i++) {
		nvKmsResetNotifier(kNotifierFormat, false, i, fNotifierMap.Address());
	}
}


void NvKmsDisplay::FillFlipCompletionNotifier(NvKmsCompletionNotifierDescription &cn, NvU32 slot)
{
	cn.surface.surfaceHandle = fNotifierSurface.Get();
	cn.surface.format = kNotifierFormat;
	cn.surface.offsetInWords = NotifierSlotOffsetInWords(slot);
	cn.awaken = true;
}


static NvU32 VramBytesPerRow(NvU32 width)
{
	return (NvU32)AlignUp(4 * width, 256);
}


void NvKmsDisplay::SetupDisplay()
{
	NvKmsMode mode {};
	if (!PickConnectedDpy(fDpyId, mode)) {
		fprintf(stderr, "[!] no connected dpy with a preferred mode\n");
		RaiseErrno(ENODEV);
	}
	fHead = 0;
	fWidth = mode.timings.hVisible;
	fHeight = mode.timings.vVisible;
	fRefreshRateMilliHz = mode.timings.RRx1k;
	printf("dpyId %" PRIu32 ", head %" PRIu32 ", mode %s (%" PRIu32 "x%" PRIu32 " @ %" PRIu32 ".%03" PRIu32 " Hz)\n",
		nvDpyIdToIndex(fDpyId), fHead, mode.name, fWidth, fHeight,
		mode.timings.RRx1k / 1000, mode.timings.RRx1k % 1000);

	AllocateNotifier();
	NvRmApi &rm = *fRmDev.Device().Rm();
	NvU64 fbSize = (NvU64)VramBytesPerRow(fWidth) * fHeight;
	for (NvU32 i = 0; i < kNumBuffers; i++) {
		fFramebuffer[i] = NvKmsBitmap(fRmDev, fKmsDev, fWidth, fHeight, B_RGB32);
		// Zero the VRAM buffer. Source frames are blitted into a centered
		// aspect-preserving rectangle and the borders of the buffer are
		// never touched after this point — so the letterbox/pillarbox
		// bars stay black for the buffer's whole lifetime.
		memset(fFramebuffer[i].Bits(), 0, fFramebuffer[i].BitsLength());

		// Pre-map the VRAM physical handle into the blit channel's VA
		// space so BlitAndFlip can reference fFramebufferGpuAddr[i]
		// directly without re-mapping per frame.
		NV_MEMORY_ALLOCATION_PARAMS virtParams {
			.owner = rm.Client().Get(),
			.type = NVOS32_TYPE_IMAGE,
			.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
			.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _DEFAULT),
			.size = fbSize,
			.alignment = 0x1000,
			.hVASpace = fChannel.VaSpace(),
		};
		fFramebufferVirt[i] = NvRmObject(rm,
			rm.Alloc(NV50_MEMORY_VIRTUAL, fRmDev.Device().Get(), &virtParams, sizeof(virtParams)));
		rm.MapMemoryDma(fRmDev.Device().Get(),
			fFramebufferVirt[i].Get(),
			fFramebuffer[i].Memory().Get(),
			0, fbSize, 0, fFramebufferGpuAddr[i]);
	}

	SubmitInitialSetMode(mode);
}


void NvKmsDisplay::SubmitInitialSetMode(const NvKmsMode &mode)
{
	NvKmsSetModeParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.commit = true;
	params.request.requestedDispsBitMask = 1U << 0;
	params.request.disp[0].requestedHeadsBitMask = 1U << fHead;

	auto &headReq = params.request.disp[0].head[fHead];
	headReq.dpyIdList = nvAddDpyIdToEmptyDpyIdList(fDpyId);
	headReq.mode = mode;
	headReq.viewPortOut = {.x = 0, .y = 0, .width = fWidth, .height = fHeight};
	headReq.viewPortSizeIn = {.width = fWidth, .height = fHeight};

	auto &layer = headReq.flip.layer[NVKMS_MAIN_LAYER];
	layer.surface.handle[0] = fFramebuffer[0].Surface().Get();
	layer.surface.specified = true;
	layer.sizeIn.val = {.width = fWidth, .height = fHeight};
	layer.sizeIn.specified = true;
	layer.sizeOut.val = {.width = fWidth, .height = fHeight};
	layer.sizeOut.specified = true;

	FillFlipCompletionNotifier(layer.completionNotifier.val, 0);
	layer.completionNotifier.specified = true;

	try {
		fKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params));
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] SetMode failed: %s\n", ex.what());
		fprintf(stderr, "    status=%d disp[0].status=%d head[%" PRIu32 "].status=%d\n",
			params.reply.status,
			params.reply.disp[0].status,
			fHead,
			params.reply.disp[0].head[fHead].status);
		throw;
	}

	fQueuedBuffer = 0;
	fFlipCounter = 1; // SetMode counts as flip 0.
}


// Push the TWOD methods for one PIXELS_FROM_MEMORY blit (with bilinear
// scaling) plus the blend-constant + dst + src setup. Caller is
// responsible for issuing the trailing SEM_RELEASE / interrupt / submit.
static void EmitTwodBlit(NvCommandBuffer &cmdBuf,
	NvU64 srcGpuAddr, NvU32 srcWidth, NvU32 srcHeight, NvU32 srcPitch,
	NvU64 dstGpuAddr, NvU32 dstWidth, NvU32 dstHeight, NvU32 dstPitch,
	NvU32 outX, NvU32 outY, NvU32 outW, NvU32 outH)
{
	// SET_OBJECT every blit: cheap, and lets a future caller share the
	// channel with other engines via different subchannels.
	WriteSetObject(cmdBuf, kTwodSubch, FERMI_TWOD_A);

	{
		// BETA1 / BETA4 saturated — irrelevant for SRCCOPY but harmless.
		NvPushWriter wr = cmdBuf.Push(4);
		wr.IncMethod(kTwodSubch, NV902D_SET_BETA1, 1);
		wr.U32(0x7F800000);
		wr.IncMethod(kTwodSubch, NV902D_SET_BETA4, 1);
		wr.U32(0xFFFFFFFFu);
		cmdBuf.EndPush(wr);
	}
	{
		// Destination = full VRAM scanout. We only paint the active rect;
		// the surrounding pixels keep their existing (zero) value.
		NvPushWriter wr = cmdBuf.Push(11);
		wr.IncMethod(kTwodSubch, NV902D_SET_DST_FORMAT, 1);
		wr.U32(NV902D_SET_DST_FORMAT_V_A8R8G8B8);
		wr.IncMethod(kTwodSubch, NV902D_SET_DST_MEMORY_LAYOUT, 1);
		wr.U32(NV902D_SET_DST_MEMORY_LAYOUT_V_PITCH);
		wr.IncMethod(kTwodSubch, NV902D_SET_DST_PITCH, 1);
		wr.U32(dstPitch);
		wr.IncMethod(kTwodSubch, NV902D_SET_DST_WIDTH, 4);
		wr.U32(dstWidth);
		wr.U32(dstHeight);
		wr.U32((NvU32)(dstGpuAddr >> 32));
		wr.U32((NvU32)(dstGpuAddr & 0xFFFFFFFFu));
		cmdBuf.EndPush(wr);
	}
	{
		// Source = GART decoded frame.
		NvPushWriter wr = cmdBuf.Push(11);
		wr.IncMethod(kTwodSubch, NV902D_SET_SRC_FORMAT, 1);
		wr.U32(NV902D_SET_SRC_FORMAT_V_A8R8G8B8);
		wr.IncMethod(kTwodSubch, NV902D_SET_SRC_MEMORY_LAYOUT, 1);
		wr.U32(NV902D_SET_SRC_MEMORY_LAYOUT_V_PITCH);
		wr.IncMethod(kTwodSubch, NV902D_SET_SRC_PITCH, 1);
		wr.U32(srcPitch);
		wr.IncMethod(kTwodSubch, NV902D_SET_SRC_WIDTH, 4);
		wr.U32(srcWidth);
		wr.U32(srcHeight);
		wr.U32((NvU32)(srcGpuAddr >> 32));
		wr.U32((NvU32)(srcGpuAddr & 0xFFFFFFFFu));
		cmdBuf.EndPush(wr);
	}
	{
		// SRCCOPY + bilinear with ORIGIN_CORNER. step = src/dst in 32.32
		// fixed point so the engine samples src at srcStart + i * step
		// for dst pixel i, giving geometric correctness at any ratio.
		const NvU64 duDx = ScaleStep(srcWidth, outW);
		const NvU64 dvDy = ScaleStep(srcHeight, outH);
		const NvU32 sampleMode =
			(NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_ORIGIN_CORNER << 0) |
			(NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE_FILTER_BILINEAR << 4);
		NvPushWriter wr = cmdBuf.Push(17);
		wr.IncMethod(kTwodSubch, NV902D_SET_OPERATION, 1);
		wr.U32(NV902D_SET_OPERATION_V_SRCCOPY);
		wr.IncMethod(kTwodSubch, NV902D_SET_PIXELS_FROM_MEMORY_SAMPLE_MODE, 1);
		wr.U32(sampleMode);
		// 12 contiguous methods; the trailing SRC_Y0_INT triggers the blit.
		wr.IncMethod(kTwodSubch, NV902D_SET_PIXELS_FROM_MEMORY_DST_X0, 12);
		wr.U32(outX);
		wr.U32(outY);
		wr.U32(outW);
		wr.U32(outH);
		wr.U32((NvU32)(duDx & 0xFFFFFFFFu));
		wr.U32((NvU32)(duDx >> 32));
		wr.U32((NvU32)(dvDy & 0xFFFFFFFFu));
		wr.U32((NvU32)(dvDy >> 32));
		wr.U32(0); // SRC_X0_FRAC
		wr.U32(0); // SRC_X0_INT
		wr.U32(0); // SRC_Y0_FRAC
		wr.U32(0); // SRC_Y0_INT
		cmdBuf.EndPush(wr);
	}
}


void NvKmsDisplay::SubmitFlip(NvU32 buffer)
{
	NvKmsFlipRequestOneHead flipHead {};
	flipHead.sd = 0;
	flipHead.head = fHead;
	auto &layer = flipHead.flip.layer[NVKMS_MAIN_LAYER];
	layer.surface.handle[0] = fFramebuffer[buffer].Surface().Get();
	layer.surface.specified = true;
	FillFlipCompletionNotifier(layer.completionNotifier.val, buffer);
	layer.completionNotifier.specified = true;

	NvKmsFlipParams flipParams {};
	flipParams.request.deviceHandle = fDeviceHandle;
	flipParams.request.pFlipHead = nvKmsPointerToNvU64(&flipHead);
	flipParams.request.numFlipHeads = 1;
	flipParams.request.commit = true;
	fKms.Control(NVKMS_IOCTL_FLIP, &flipParams, sizeof(flipParams));

	fQueuedBuffer = buffer;
	fFlipCounter++;
}


void NvKmsDisplay::BlitAndFlip(NvU64 srcGpuAddr, NvU32 srcWidth, NvU32 srcHeight, NvU32 srcPitch)
{
	NvU32 nextBuffer = (fQueuedBuffer + 1) % kNumBuffers;

	// Aspect-preserving fit: scale so the longest source axis matches the
	// display, center on the other axis. Pixels outside the active rect
	// are not touched — they keep the zero (black) value written at
	// SetupDisplay so we get free letterbox/pillarbox bars.
	NvU32 outW = fWidth;
	NvU32 outH = fHeight;
	if ((NvU64)srcWidth * fHeight > (NvU64)srcHeight * fWidth) {
		// Source is wider than display: pin width, letterbox top/bottom.
		outH = (NvU32)(((NvU64)fWidth * srcHeight) / srcWidth);
		if (outH == 0) outH = 1;
	} else {
		// Source is taller than display: pin height, pillarbox sides.
		outW = (NvU32)(((NvU64)fHeight * srcWidth) / srcHeight);
		if (outW == 0) outW = 1;
	}
	NvU32 outX = (fWidth  - outW) / 2;
	NvU32 outY = (fHeight - outH) / 2;

	const NvU64 wSeq = ++fBlitSeq;
	fBlitSem.RecordSubmission(0, wSeq);
	fBlitSem.RegisterWaiter(0, wSeq, 0, (NvU64)fBlitEvent.Fd());

	EmitTwodBlit(fCmdBuf,
		srcGpuAddr, srcWidth, srcHeight, srcPitch,
		fFramebufferGpuAddr[nextBuffer], fWidth, fHeight, VramBytesPerRow(fWidth),
		outX, outY, outW, outH);

	fBlitSem.WriteRelease(fCmdBuf, 0, wSeq, /*waitForIdle=*/true);
	WriteNonStallInterrupt(fCmdBuf);
	fCmdBuf.SubmitTo(fChannel);

	while (fBlitSem.ReadValue(0) < wSeq) {
		fChannel.CheckError();
		struct pollfd pfd {
			.fd = fBlitEvent.Fd(),
			.events = POLLIN | POLLPRI,
		};
		int pr = poll(&pfd, 1, 1000);
		if (pr < 0) {
			CheckErrno(pr);
		}
	}

	SubmitFlip(nextBuffer);
}


bool NvKmsDisplay::PollFlipNotifierTimestamp(NvU64 &outPtimerNs, int timeoutMs)
{
	// Drain any pending event from the kernel-side queue first; only fall
	// through to poll() if the queue is empty.
	for (;;) {
		NvKmsGetNextEventParams params {};
		fKms.Control(NVKMS_IOCTL_GET_NEXT_EVENT, &params, sizeof(params));
		if (params.reply.valid) {
			if (params.reply.event.eventType == NVKMS_EVENT_TYPE_FLIP_OCCURRED) {
				// Each FLIP_OCCURRED transition is a BEGUN->FINISHED on the
				// previously queued buffer's notifier slot. The slot index
				// equals fQueuedBuffer at the time the event fires, but the
				// timestamp we want is in whichever slot just transitioned.
				// Both slots get written eventually; the caller doesn't care
				// which one — return the freshest valid timestamp.
				NvU64 best = 0;
				bool any = false;
				for (NvU32 s = 0; s < kNumBuffers; s++) {
					nvKmsParsedNotifier parsed {};
					nvKmsParseNotifier(kNotifierFormat, false, s, fNotifierMap.Address(), &parsed);
					if (parsed.timeStampValid && parsed.timeStamp > best) {
						best = parsed.timeStamp;
						any = true;
					}
				}
				if (any) {
					outPtimerNs = best;
					return true;
				}
				// No valid timestamp yet — keep draining.
				continue;
			}
			continue;
		}
		break;
	}

	if (timeoutMs == 0) {
		return false;
	}

	struct pollfd pollFds[1] = {
		{ .fd = fKms.Fd(), .events = POLLIN | POLLPRI },
	};
	int pr = poll(pollFds, 1, timeoutMs);
	if (pr <= 0) {
		return false;
	}
	// Recurse-ish: drain once more.
	return PollFlipNotifierTimestamp(outPtimerNs, 0);
}
