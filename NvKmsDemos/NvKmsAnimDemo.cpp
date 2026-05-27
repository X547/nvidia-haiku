// NvKmsAnimDemo
//
// Drives a single display through NVKMS with a double-buffered animated
// gradient. The demo enumerates valid dpys, picks the first connected
// one with a preferred mode, allocates three pitch-linear framebuffers,
// performs the initial SetMode, then loops on FLIP_OCCURRED events
// rendering the next-next frame into the free buffer while the previously
// submitted flip is being shown.
//
// A single sysmem block holds two completion-notifier slots (one per
// buffer index), used both to gate the per-flip awaken and to make the
// FLIP_OCCURRED event fire.

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>

extern "C" {
#include "nvkms-ioctl.h"
#include "nvkms-api.h"
#include "nvkms-sync.h"
#include "nv-haiku.h"
}

#include "ErrorUtils.h"
#include "FileDesc.h"

#include "NvRmApi.h"
#include "NvRmDevice.h"

#include "NvKmsApi.h"
#include "NvKmsDevice.h"
#include "NvKmsSurface.h"

#include "NvUtils.h"
#include "NvKmsBitmap.h"


struct ColorRgba {
	NvU8 r;
	NvU8 g;
	NvU8 b;
	NvU8 a;
};


static const NvKmsNIsoFormat kNotifierFormat = NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY;
static const NvU64 kNotifierMemSize = 4096;
static const NvU32 kNumBuffers = 2;


// Per-slot offset inside the shared notifier sysmem block, in dwords —
// which is what NvKmsRegisterSurfaceParams takes via offsetInWords.
static NvU32 NotifierSlotOffsetInWords(NvU32 slot)
{
	return slot * (nvKmsSizeOfNotifier(kNotifierFormat, false) / (NvU32)sizeof(NvU32));
}


void PrintNotifier(enum NvKmsNIsoFormat format, NvBool overlay, NvU32 index, const void *base)
{
	nvKmsParsedNotifier parsedNotifier {};
	nvKmsParseNotifier(format, overlay, index, base, &parsedNotifier);
	printf("(");
	printf("timeStamp: %" PRIu64 ", ", parsedNotifier.timeStamp);
	printf("timeStampValid: %d, ", parsedNotifier.timeStampValid);
	printf("status: ");
	switch (parsedNotifier.status) {
		case NVKMS_NOTIFIER_STATUS_NOT_BEGUN: printf("NOT_BEGUN"); break;
		case NVKMS_NOTIFIER_STATUS_BEGUN: printf("BEGUN"); break;
		case NVKMS_NOTIFIER_STATUS_FINISHED: printf("FINISHED"); break;
		default: printf("?"); break;
	}
	printf(", ");
	printf("presentCount: %" PRIu8, parsedNotifier.presentCount);
	printf(")");
}


class NvKmsAnimDemo {
private:
	FileDesc fDevFd;
	NvRmApi fRm;
	NvRmDevice fRmDev;
	NvKmsApi fKms;
	NvKmsDevice fKmsDev;

	NvKmsDeviceHandle fDeviceHandle;
	NvKmsDispHandle fDispHandle;
	NVDpyId fDpyId;
	NvU32 fHead;
	NvU32 fWidth;
	NvU32 fHeight;

	// Shared notifier surface backing per-buffer completion slots.
	NvRmObject fNotifierMem;
	NvRmMemoryMapping fNotifierMap;
	NvKmsSurface fNotifierSurface;

	NvKmsBitmap fFramebuffer[kNumBuffers];
	// Index of the buffer just submitted via Flip (i.e. the one queued for
	// the next vsync). The "free" buffer the next render uses is
	// (fQueuedBuffer + 1) % kNumBuffers;
	NvU32 fQueuedBuffer;
	NvU32 fPhase;

	bool QueryConnected(NVDpyId dpyId);
	bool EnumDpyMode(NVDpyId dpyId, NvU32 index, NvKmsValidateModeIndexReply &outReply);
	bool QueryBestMode(NVDpyId dpyId, NvKmsMode &outMode);
	bool PickConnectedDpy(NVDpyId &outDpyId, NvKmsMode &outMode);

	void AllocateNotifier();
	void FillFlipCompletionNotifier(NvKmsCompletionNotifierDescription &cn, NvU32 slot);
	void RenderFrame(NvKmsBitmap &bitmap, NvU32 phase);
	void SubmitFlip(NvU32 buffer);
	void WaitForFlipEvent();

public:
	NvKmsAnimDemo();

	void SetupDisplay();
	void RunEventLoop();
};


NvKmsAnimDemo::NvKmsAnimDemo():
	fDevFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC)),
	fRmDev(fRm, 0),
	fKmsDev(fKms, 0),
	fHead(0),
	fWidth(0),
	fHeight(0),
	fQueuedBuffer(0),
	fPhase(0)
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
	}
	{
		NvKmsDeclareEventInterestParams params {};
		params.request.interestMask = 1U << NVKMS_EVENT_TYPE_FLIP_OCCURRED;
		fKms.Control(NVKMS_IOCTL_DECLARE_EVENT_INTEREST, &params, sizeof(params));
	}
}


bool NvKmsAnimDemo::QueryConnected(NVDpyId dpyId)
{
	NvKmsQueryDpyDynamicDataParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.dispHandle = fDispHandle;
	params.request.dpyId = dpyId;
	fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params));
	return params.reply.connected;
}

bool NvKmsAnimDemo::EnumDpyMode(NVDpyId dpyId, NvU32 index, NvKmsValidateModeIndexReply &outReply)
{
	NvKmsValidateModeIndexParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.dispHandle = fDispHandle;
	params.request.dpyId = dpyId;
	params.request.modeIndex = index;
	fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params));
	outReply = params.reply;
	return !params.reply.end;
}

bool NvKmsAnimDemo::QueryBestMode(NVDpyId dpyId, NvKmsMode &outMode)
{
	// The EDID marks exactly one mode as "preferred": the panel's native
	// timing (base-block detailed timing #1), typically at 60 Hz. High
	// refresh variants live in extension blocks and are never flagged
	// preferred, so the preferred flag alone can't pick them. Use it only
	// to learn the native resolution, then take the fastest valid mode at
	// that resolution. Fall back to the largest valid resolution if the
	// EDID nominates no preferred mode.
	NvU32 nativeW = 0, nativeH = 0;
	NvKmsValidateModeIndexReply reply;

	for (NvU32 i = 0; EnumDpyMode(dpyId, i, reply); i++) {
		if (!reply.valid) {
			continue;
		}
		if (reply.preferredMode) {
			nativeW = reply.mode.timings.hVisible;
			nativeH = reply.mode.timings.vVisible;
			break;
		}
		if ((NvU64)reply.mode.timings.hVisible * reply.mode.timings.vVisible >
		    (NvU64)nativeW * nativeH) {
			nativeW = reply.mode.timings.hVisible;
			nativeH = reply.mode.timings.vVisible;
		}
	}

	if (nativeW == 0 || nativeH == 0) {
		return false;
	}

	bool found = false;
	for (NvU32 i = 0; EnumDpyMode(dpyId, i, reply); i++) {
		if (!reply.valid ||
		    reply.mode.timings.hVisible != nativeW ||
		    reply.mode.timings.vVisible != nativeH ||
		    reply.mode.timings.interlaced) {
			continue;
		}
		if (!found || reply.mode.timings.RRx1k > outMode.timings.RRx1k) {
			outMode = reply.mode;
			found = true;
		}
	}
	return found;
}

bool NvKmsAnimDemo::PickConnectedDpy(NVDpyId &outDpyId, NvKmsMode &outMode)
{
	// QueryDpyDynamicData must be called at least once before mode
	// enumeration returns anything meaningful — the loop below covers
	// both the QueryConnected probe and the preferred-mode lookup.
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
		if (QueryConnected(cur) && QueryBestMode(cur, outMode)) {
			outDpyId = cur;
			return true;
		}
		cur = nvNextDpyIdInDpyIdListUnsorted(cur, validDpys);
	}
	return false;
}


void NvKmsAnimDemo::AllocateNotifier()
{
	// nvKmsKapiAllocateSystemMemory tags the allocation with the proper
	// NISO_DISPLAY attr2 bit and picks coherency from the kms device's
	// nisoIOCoherencyModes — the display engine writes don't land in a
	// CPU-readable form without those flags set up correctly.
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


void NvKmsAnimDemo::FillFlipCompletionNotifier(NvKmsCompletionNotifierDescription &cn, NvU32 slot)
{
	cn.surface.surfaceHandle = fNotifierSurface.Get();
	cn.surface.format = kNotifierFormat;
	cn.surface.offsetInWords = NotifierSlotOffsetInWords(slot);
	cn.awaken = true;
}


void NvKmsAnimDemo::RenderFrame(NvKmsBitmap &bitmap, NvU32 phase)
{
	auto line = (NvU8*)bitmap.Bits();
	NvU32 stride = bitmap.BytesPerRow();
	for (NvU32 y = 0; y < bitmap.Height(); y++) {
		auto color = (ColorRgba*)line;
		for (NvU32 x = 0; x < bitmap.Width(); x++) {
			color->r = (NvU8)((x + phase) & 0xff);
			color->g = (NvU8)((y + phase) & 0xff);
			color->b = 0x80;
			color->a = 0xff;
			color++;
		}
		line += stride;
	}
}


void NvKmsAnimDemo::SetupDisplay()
{
	NvKmsMode mode {};
	if (!PickConnectedDpy(fDpyId, mode)) {
		fprintf(stderr, "[!] no connected dpy with a usable mode\n");
		RaiseErrno(ENODEV);
	}
	fHead = 0;
	fWidth = mode.timings.hVisible;
	fHeight = mode.timings.vVisible;
	printf("dpyId %" PRIu32 ", head %" PRIu32 ", mode %s (%" PRIu32 "x%" PRIu32 " @ %" PRIu32 ".%03" PRIu32 " Hz)\n",
		nvDpyIdToIndex(fDpyId), fHead, mode.name, fWidth, fHeight,
		mode.timings.RRx1k / 1000, mode.timings.RRx1k % 1000);

	AllocateNotifier();
	for (NvU32 i = 0; i < kNumBuffers; i++) {
		fFramebuffer[i] = NvKmsBitmap(fRmDev, fKmsDev, fWidth, fHeight, B_RGB32);
	}

	// Seed framebuffer[0] with the first frame and present it via SetMode.
	fPhase = 0;
	RenderFrame(fFramebuffer[0], fPhase);

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
		printf("[!] SetMode failed: %s\n", ex.what());
		printf("    status=%d disp[0].status=%d head[%" PRIu32 "].status=%d\n",
			params.reply.status,
			params.reply.disp[0].status,
			fHead,
			params.reply.disp[0].head[fHead].status);
		throw;
	}
}


void NvKmsAnimDemo::SubmitFlip(NvU32 buffer)
{
	NvKmsFlipRequestOneHead flipHead {};
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
}


void NvKmsAnimDemo::WaitForFlipEvent()
{
	// NOTE: Flip occured event is generated when notifier change state from
	// BEGUN to FINISHED. So first flip request do NOT produce flip occured
	// event. Flip request specified as a part of set mode request works in the
	// same way as dedicated flip request.

	for (;;) {
		NvKmsGetNextEventParams params {};
		fKms.Control(NVKMS_IOCTL_GET_NEXT_EVENT, &params, sizeof(params));
		if (params.reply.valid) {
			if (params.reply.event.eventType == NVKMS_EVENT_TYPE_FLIP_OCCURRED) {
				return;
			}
			continue;
		}
		struct pollfd pollFds[1] = {
			{
				.fd = fKms.Fd(),
				.events = POLLIN | POLLPRI,
			}
		};
		poll(pollFds, 1, -1);
	}
}


void NvKmsAnimDemo::RunEventLoop()
{
	for (;;) {
		NvU32 nextBuffer = (fQueuedBuffer + 1) % kNumBuffers;
		fPhase = (fPhase + 1) & 0xff;
		RenderFrame(fFramebuffer[nextBuffer], fPhase);

		SubmitFlip(nextBuffer);

#if 0
	for (int i = 0; i < kNumBuffers; i++) {
		printf("notifier[%d]: ", i);
		PrintNotifier(kNotifierFormat, false, i, fNotifierMap.Address());
		printf("\n");
	}
#endif

		WaitForFlipEvent();
	}
}


int main()
{
	try {
		NvKmsAnimDemo demo;
		demo.SetupDisplay();
		demo.RunEventLoop();
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvKmsAnimDemo: %s\n", ex.what());
		return 1;
	}
	return 0;
}
