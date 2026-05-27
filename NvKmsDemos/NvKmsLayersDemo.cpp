// NvKmsLayersDemo
//
// Demonstrates driving the main layer, overlay layer, and the hardware
// cursor of a single head in a single atomic NVKMS_IOCTL_FLIP request:
// each frame submits new main + overlay buffers, repositions the cursor,
// and waits for both layers' FLIP_OCCURRED events before recycling buffers.
//
// Layout:
//   * Main layer (X8R8G8B8, full screen) shows a scrolling gradient.
//   * Overlay layer (A8R8G8B8, 480x270, PREMULT alpha, windowed) bounces
//     around as a translucent panel on top of the main layer.
//   * Hardware cursor (A8R8G8B8, 64x64, PREMULT alpha) circles the screen.
//
// A single sysmem block backs the per-buffer completion notifiers. Each
// flipped buffer gets two slots — one for the main layer, one for the
// overlay layer — laid out as (buffer*2 + layer).

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <inttypes.h>
#include <math.h>
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
	NvU8 b;
	NvU8 g;
	NvU8 r;
	NvU8 a;
};


static const NvKmsNIsoFormat kNotifierFormat = NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY;
static const NvU64 kNotifierMemSize = 4096;
static const NvU32 kNumBuffers = 2;
static const NvU32 kNotifierSlotsPerBuffer = 2; // [0]=main, [1]=overlay

static const NvU32 kOverlayWidth = 480;
static const NvU32 kOverlayHeight = 270;
static const NvU32 kCursorSize = 64;


static inline size_t AlignUp(size_t val, size_t align)
{
	return (val + (align - 1)) / align * align;
}


// Per-slot offset inside the shared notifier sysmem block, in dwords.
static NvU32 NotifierSlotOffsetInWords(NvU32 buffer, NvU32 layerSlot)
{
	NvU32 slot = buffer * kNotifierSlotsPerBuffer + layerSlot;
	return slot * (nvKmsSizeOfNotifier(kNotifierFormat, false) / (NvU32)sizeof(NvU32));
}


// Minimal RAII wrapper around an A8R8G8B8 NvKms scanout surface — used for
// the overlay and cursor since NvKmsBitmap hardcodes X8R8G8B8 (no alpha).
class AlphaSurface {
private:
	NvRmObject fMemory;
	NvKmsSurface fSurface;
	NvRmMemoryMapping fMapping;
	NvU32 fWidth;
	NvU32 fHeight;
	NvU32 fBytesPerRow;

public:
	AlphaSurface(): fWidth(0), fHeight(0), fBytesPerRow(0) {}

	AlphaSurface(AlphaSurface &&other):
		fMemory(std::move(other.fMemory)),
		fSurface(std::move(other.fSurface)),
		fMapping(std::move(other.fMapping)),
		fWidth(other.fWidth), fHeight(other.fHeight), fBytesPerRow(other.fBytesPerRow)
	{
		other.fWidth = other.fHeight = other.fBytesPerRow = 0;
	}

	AlphaSurface &operator=(AlphaSurface &&other)
	{
		fMemory = std::move(other.fMemory);
		fSurface = std::move(other.fSurface);
		fMapping = std::move(other.fMapping);
		fWidth = other.fWidth;
		fHeight = other.fHeight;
		fBytesPerRow = other.fBytesPerRow;
		other.fWidth = other.fHeight = other.fBytesPerRow = 0;
		return *this;
	}

	AlphaSurface(NvRmDevice &rmDev, NvKmsDevice &kmsDev, NvU32 width, NvU32 height):
		fWidth(width), fHeight(height), fBytesPerRow((NvU32)AlignUp(4 * width, 256))
	{
		NvU32 size = fBytesPerRow * height;

		NvU8 compressible = 0;
		nvKmsKapiAllocateVideoMemory(rmDev, fMemory, NvKmsSurfaceMemoryLayoutPitch,
			AlignUp(size, B_PAGE_SIZE), NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT, &compressible);

		FileDesc memoryFd = rmDev.ExportObjectToFd(rmDev.Device().Get(), fMemory.Get());

		NvKmsRegisterSurfaceParams params {};
		params.request.deviceHandle = kmsDev.Get();
		params.request.useFd = true;
		params.request.planes[0].u.fd = memoryFd.Get();
		params.request.planes[0].pitch = fBytesPerRow;
		params.request.planes[0].rmObjectSizeInBytes = size;
		params.request.widthInPixels = width;
		params.request.heightInPixels = height;
		params.request.layout = NvKmsSurfaceMemoryLayoutPitch;
		params.request.format = NvKmsSurfaceMemoryFormatA8R8G8B8;
		kmsDev.Kms().Control(NVKMS_IOCTL_REGISTER_SURFACE, &params, sizeof(params));
		fSurface = NvKmsSurface(kmsDev, params.reply.surfaceHandle);

		fMapping = rmDev.MapMemory(fMemory.Get(), false, 0, AlignUp(size, B_PAGE_SIZE), 0);
	}

	NvU32 Width() const {return fWidth;}
	NvU32 Height() const {return fHeight;}
	NvU32 BytesPerRow() const {return fBytesPerRow;}
	void *Bits() const {return fMapping.Address();}
	const NvKmsSurface &Surface() const {return fSurface;}
};


class NvKmsLayersDemo {
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

	NvRmObject fNotifierMem;
	NvRmMemoryMapping fNotifierMap;
	NvKmsSurface fNotifierSurface;

	NvKmsBitmap fMainFb[kNumBuffers];
	AlphaSurface fOverlayFb[kNumBuffers];
	AlphaSurface fCursor;
	bool fOverlayEnabled;

	NvU32 fQueuedBuffer;
	NvU32 fFrame;

	bool QueryConnected(NVDpyId dpyId);
	bool QueryPreferredMode(NVDpyId dpyId, NvKmsMode &outMode);
	bool PickConnectedDpy(NVDpyId &outDpyId, NvKmsMode &outMode);

	void AllocateNotifier();
	void FillFlipCompletionNotifier(NvKmsCompletionNotifierDescription &cn, NvU32 buffer, NvU32 layerSlot);

	void RenderMain(NvKmsBitmap &fb, NvU32 frame);
	void RenderOverlay(AlphaSurface &fb, NvU32 frame);
	void RenderCursor(AlphaSurface &fb);

	void ComputeOverlayPosition(NvU32 frame, NvS16 &outX, NvS16 &outY);
	void ComputeCursorPosition(NvU32 frame, NvS16 &outX, NvS16 &outY);

	void SubmitAtomicFlip(NvU32 buffer);
	void WaitForFlipEvents();

public:
	NvKmsLayersDemo();

	void SetupDisplay();
	void RunEventLoop();
};


NvKmsLayersDemo::NvKmsLayersDemo():
	fDevFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC)),
	fRmDev(fRm, 0),
	fKmsDev(fKms, 0),
	fHead(0),
	fWidth(0),
	fHeight(0),
	fOverlayEnabled(false),
	fQueuedBuffer(0),
	fFrame(0)
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


bool NvKmsLayersDemo::QueryConnected(NVDpyId dpyId)
{
	NvKmsQueryDpyDynamicDataParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.dispHandle = fDispHandle;
	params.request.dpyId = dpyId;
	fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params));
	return params.reply.connected;
}

bool NvKmsLayersDemo::QueryPreferredMode(NVDpyId dpyId, NvKmsMode &outMode)
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

bool NvKmsLayersDemo::PickConnectedDpy(NVDpyId &outDpyId, NvKmsMode &outMode)
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


void NvKmsLayersDemo::AllocateNotifier()
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

	for (NvU32 b = 0; b < kNumBuffers; b++) {
		for (NvU32 s = 0; s < kNotifierSlotsPerBuffer; s++) {
			nvKmsResetNotifier(kNotifierFormat, false,
				b * kNotifierSlotsPerBuffer + s, fNotifierMap.Address());
		}
	}
}


void NvKmsLayersDemo::FillFlipCompletionNotifier(NvKmsCompletionNotifierDescription &cn, NvU32 buffer, NvU32 layerSlot)
{
	cn.surface.surfaceHandle = fNotifierSurface.Get();
	cn.surface.format = kNotifierFormat;
	cn.surface.offsetInWords = NotifierSlotOffsetInWords(buffer, layerSlot);
	cn.awaken = true;
}


void NvKmsLayersDemo::RenderMain(NvKmsBitmap &fb, NvU32 frame)
{
	auto line = (NvU8*)fb.Bits();
	NvU32 stride = fb.BytesPerRow();
	NvU32 phase = frame & 0xff;
	for (NvU32 y = 0; y < fb.Height(); y++) {
		auto color = (ColorRgba*)line;
		for (NvU32 x = 0; x < fb.Width(); x++) {
			color->r = (NvU8)((x + phase) & 0xff);
			color->g = (NvU8)((y - phase) & 0xff);
			color->b = 0x40;
			color->a = 0xff;
			color++;
		}
		line += stride;
	}
}


void NvKmsLayersDemo::RenderOverlay(AlphaSurface &fb, NvU32 frame)
{
	// Translucent rounded panel: pre-multiplied alpha, ~50% opacity in the
	// body fading to 0 over a 32-pixel border. Inside-border pixels animate
	// a horizontal cyan stripe so the user sees motion within the overlay
	// independent of its position on the main layer.
	NvU32 stride = fb.BytesPerRow();
	NvU8 *line = (NvU8*)fb.Bits();
	const NvU32 border = 32;
	const NvU32 stripeWidth = 24;
	NvU32 stripeX = frame % (fb.Width() + stripeWidth);
	for (NvU32 y = 0; y < fb.Height(); y++) {
		auto color = (ColorRgba*)line;
		NvU32 dyTop = y;
		NvU32 dyBot = (fb.Height() - 1) - y;
		NvU32 dy = dyTop < dyBot ? dyTop : dyBot;
		for (NvU32 x = 0; x < fb.Width(); x++) {
			NvU32 dxLeft = x;
			NvU32 dxRight = (fb.Width() - 1) - x;
			NvU32 dx = dxLeft < dxRight ? dxLeft : dxRight;
			NvU32 dist = dx < dy ? dx : dy;
			NvU32 alpha;
			if (dist >= border) {
				alpha = 128;
			} else {
				alpha = (dist * 128) / border;
			}
			NvU8 r = 0x20;
			NvU8 g = 0x80;
			NvU8 b = 0xe0;
			if (x + stripeWidth >= stripeX && x < stripeX) {
				r = 0xff;
				g = 0xff;
				b = 0xff;
			}
			// Pre-multiply.
			color->r = (NvU8)((r * alpha) / 255);
			color->g = (NvU8)((g * alpha) / 255);
			color->b = (NvU8)((b * alpha) / 255);
			color->a = (NvU8)alpha;
			color++;
		}
		line += stride;
	}
}


void NvKmsLayersDemo::RenderCursor(AlphaSurface &fb)
{
	// Solid yellow disk with a 1-pixel black outline, the rest fully
	// transparent — premultiplied so it composes correctly on the overlay
	// and main beneath it.
	NvU32 stride = fb.BytesPerRow();
	NvU8 *line = (NvU8*)fb.Bits();
	const NvS32 cx = (NvS32)fb.Width() / 2;
	const NvS32 cy = (NvS32)fb.Height() / 2;
	const NvS32 rOuter = (NvS32)fb.Width() / 2 - 2;
	const NvS32 rInner = rOuter - 2;
	for (NvU32 y = 0; y < fb.Height(); y++) {
		auto color = (ColorRgba*)line;
		for (NvU32 x = 0; x < fb.Width(); x++) {
			NvS32 dx = (NvS32)x - cx;
			NvS32 dy = (NvS32)y - cy;
			NvS32 d2 = dx * dx + dy * dy;
			NvU8 r = 0, g = 0, b = 0, a = 0;
			if (d2 <= rInner * rInner) {
				r = 0xff; g = 0xd0; b = 0x00; a = 0xff;
			} else if (d2 <= rOuter * rOuter) {
				r = 0x00; g = 0x00; b = 0x00; a = 0xff;
			}
			color->r = r;
			color->g = g;
			color->b = b;
			color->a = a;
			color++;
		}
		line += stride;
	}
}


void NvKmsLayersDemo::ComputeOverlayPosition(NvU32 frame, NvS16 &outX, NvS16 &outY)
{
	// Bounce around inside the visible region. Both axes use a triangle
	// wave so the path is deterministic and the demo's motion is easy to
	// follow.
	NvS32 maxX = (NvS32)fWidth - (NvS32)kOverlayWidth;
	NvS32 maxY = (NvS32)fHeight - (NvS32)kOverlayHeight;
	if (maxX < 0) maxX = 0;
	if (maxY < 0) maxY = 0;
	NvU32 px = frame % (2 * maxX + 1);
	NvU32 py = (frame * 7 / 11) % (2 * maxY + 1);
	NvS32 x = (NvS32)px > maxX ? (2 * maxX - (NvS32)px) : (NvS32)px;
	NvS32 y = (NvS32)py > maxY ? (2 * maxY - (NvS32)py) : (NvS32)py;
	outX = (NvS16)x;
	outY = (NvS16)y;
}


void NvKmsLayersDemo::ComputeCursorPosition(NvU32 frame, NvS16 &outX, NvS16 &outY)
{
	// Circle around the center of the screen.
	double t = frame * (M_PI / 90.0);
	double radius = (double)(fHeight < fWidth ? fHeight : fWidth) * 0.30;
	double cx = (double)fWidth * 0.5;
	double cy = (double)fHeight * 0.5;
	outX = (NvS16)(cx + radius * cos(t) - kCursorSize / 2);
	outY = (NvS16)(cy + radius * sin(t) - kCursorSize / 2);
}


void NvKmsLayersDemo::SetupDisplay()
{
	NvKmsMode mode {};
	if (!PickConnectedDpy(fDpyId, mode)) {
		fprintf(stderr, "[!] no connected dpy with a preferred mode\n");
		RaiseErrno(ENODEV);
	}
	fHead = 0;
	fWidth = mode.timings.hVisible;
	fHeight = mode.timings.vVisible;
	printf("dpyId %" PRIu32 ", head %" PRIu32 ", mode %s (%" PRIu32 "x%" PRIu32 " @ %" PRIu32 ".%03" PRIu32 " Hz)\n",
		nvDpyIdToIndex(fDpyId), fHead, mode.name, fWidth, fHeight,
		mode.timings.RRx1k / 1000, mode.timings.RRx1k % 1000);

	fOverlayEnabled = true;
	if (fKmsDev.Info().numLayers[fHead] < 2) {
		printf("[i] head %" PRIu32 " has only %" PRIu32 " layer(s); skipping overlay\n",
			fHead, fKmsDev.Info().numLayers[fHead]);
		fOverlayEnabled = false;
	} else if (!fKmsDev.Info().layerCaps[NVKMS_OVERLAY_LAYER].supportsWindowMode) {
		printf("[i] overlay layer does not support window mode; skipping overlay\n");
		fOverlayEnabled = false;
	}
	NvU32 cursorMax = fKmsDev.Info().maxCursorSize;
	if (cursorMax < kCursorSize) {
		fprintf(stderr, "[!] cursor size %" PRIu32 " not supported (max %" PRIu32 ")\n",
			kCursorSize, cursorMax);
		RaiseErrno(ENOTSUP);
	}

	AllocateNotifier();
	for (NvU32 i = 0; i < kNumBuffers; i++) {
		fMainFb[i] = NvKmsBitmap(fRmDev, fKmsDev, fWidth, fHeight, B_RGB32);
		if (fOverlayEnabled) {
			fOverlayFb[i] = AlphaSurface(fRmDev, fKmsDev, kOverlayWidth, kOverlayHeight);
		}
	}
	fCursor = AlphaSurface(fRmDev, fKmsDev, kCursorSize, kCursorSize);
	RenderCursor(fCursor);

	fFrame = 0;
	RenderMain(fMainFb[0], fFrame);
	if (fOverlayEnabled) {
		RenderOverlay(fOverlayFb[0], fFrame);
	}

	NvS16 overlayX, overlayY;
	NvS16 cursorX, cursorY;
	ComputeOverlayPosition(fFrame, overlayX, overlayY);
	ComputeCursorPosition(fFrame, cursorX, cursorY);

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

	// Main layer covers the whole head.
	{
		auto &layer = headReq.flip.layer[NVKMS_MAIN_LAYER];
		layer.surface.handle[0] = fMainFb[0].Surface().Get();
		layer.surface.specified = true;
		layer.sizeIn.val = {.width = fWidth, .height = fHeight};
		layer.sizeIn.specified = true;
		layer.sizeOut.val = {.width = fWidth, .height = fHeight};
		layer.sizeOut.specified = true;
		FillFlipCompletionNotifier(layer.completionNotifier.val, 0, 0);
		layer.completionNotifier.specified = true;
	}

	// Overlay layer is windowed: smaller than the head and positioned via
	// outputPosition. supportsWindowMode was checked above.
	if (fOverlayEnabled) {
		auto &layer = headReq.flip.layer[NVKMS_OVERLAY_LAYER];
		layer.surface.handle[0] = fOverlayFb[0].Surface().Get();
		layer.surface.specified = true;
		layer.sizeIn.val = {.width = kOverlayWidth, .height = kOverlayHeight};
		layer.sizeIn.specified = true;
		layer.sizeOut.val = {.width = kOverlayWidth, .height = kOverlayHeight};
		layer.sizeOut.specified = true;
		layer.outputPosition.val = {.x = overlayX, .y = overlayY};
		layer.outputPosition.specified = true;
		layer.compositionParams.val = {
			.colorKeySelect = NVKMS_COMPOSITION_COLOR_KEY_SELECT_DISABLE,
			.blendingMode = {
				NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA,
				NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA,
			},
			.surfaceAlpha = 0,
			.depth = 0, // default depth (above main).
		};
		layer.compositionParams.specified = true;
		FillFlipCompletionNotifier(layer.completionNotifier.val, 0, 1);
		layer.completionNotifier.specified = true;
	}

	// Cursor image + initial position attached to the modeset's flip.
	headReq.flip.cursor.image.surfaceHandle[0] = fCursor.Surface().Get();
	headReq.flip.cursor.image.cursorCompParams.blendingMode[0] = NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA;
	headReq.flip.cursor.image.cursorCompParams.blendingMode[1] = NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA;
	headReq.flip.cursor.imageSpecified = true;
	headReq.flip.cursor.position.x = cursorX;
	headReq.flip.cursor.position.y = cursorY;
	headReq.flip.cursor.positionSpecified = true;

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

	// SetMode counts as the initial flip; the next render targets buffer 1.
	fQueuedBuffer = 0;
}


void NvKmsLayersDemo::SubmitAtomicFlip(NvU32 buffer)
{
	NvS16 overlayX, overlayY;
	NvS16 cursorX, cursorY;
	ComputeOverlayPosition(fFrame, overlayX, overlayY);
	ComputeCursorPosition(fFrame, cursorX, cursorY);

	NvKmsFlipRequestOneHead flipHead {};
	flipHead.sd = 0;
	flipHead.head = fHead;

	{
		auto &layer = flipHead.flip.layer[NVKMS_MAIN_LAYER];
		layer.surface.handle[0] = fMainFb[buffer].Surface().Get();
		layer.surface.specified = true;
		FillFlipCompletionNotifier(layer.completionNotifier.val, buffer, 0);
		layer.completionNotifier.specified = true;
	}
	if (fOverlayEnabled) {
		auto &layer = flipHead.flip.layer[NVKMS_OVERLAY_LAYER];
		layer.surface.handle[0] = fOverlayFb[buffer].Surface().Get();
		layer.surface.specified = true;
		layer.outputPosition.val = {.x = overlayX, .y = overlayY};
		layer.outputPosition.specified = true;
		FillFlipCompletionNotifier(layer.completionNotifier.val, buffer, 1);
		layer.completionNotifier.specified = true;
	}

	// Move the cursor as part of the same atomic flip; image is left alone.
	flipHead.flip.cursor.position.x = cursorX;
	flipHead.flip.cursor.position.y = cursorY;
	flipHead.flip.cursor.positionSpecified = true;

	NvKmsFlipParams flipParams {};
	flipParams.request.deviceHandle = fDeviceHandle;
	flipParams.request.pFlipHead = nvKmsPointerToNvU64(&flipHead);
	flipParams.request.numFlipHeads = 1;
	flipParams.request.commit = true;
	fKms.Control(NVKMS_IOCTL_FLIP, &flipParams, sizeof(flipParams));

	fQueuedBuffer = buffer;
}


void NvKmsLayersDemo::WaitForFlipEvents()
{
	// One FLIP_OCCURRED event is generated per flipped layer that has an
	// awaken notifier — main always, overlay only when enabled. The first
	// flip (from SetMode) leaves the notifier in BEGUN, so the first
	// flip-loop iteration's wait completes only when the *second* atomic
	// flip retires the original buffer.
	NvU32 remaining = fOverlayEnabled ? 2 : 1;
	while (remaining > 0) {
		NvKmsGetNextEventParams params {};
		fKms.Control(NVKMS_IOCTL_GET_NEXT_EVENT, &params, sizeof(params));
		if (params.reply.valid) {
			if (params.reply.event.eventType == NVKMS_EVENT_TYPE_FLIP_OCCURRED) {
				remaining--;
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


void NvKmsLayersDemo::RunEventLoop()
{
	for (;;) {
		NvU32 nextBuffer = (fQueuedBuffer + 1) % kNumBuffers;
		fFrame++;
		RenderMain(fMainFb[nextBuffer], fFrame);
		if (fOverlayEnabled) {
			RenderOverlay(fOverlayFb[nextBuffer], fFrame);
		}

		SubmitAtomicFlip(nextBuffer);
		WaitForFlipEvents();
	}
}


int main()
{
	try {
		NvKmsLayersDemo demo;
		demo.SetupDisplay();
		demo.RunEventLoop();
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvKmsLayersDemo: %s\n", ex.what());
		return 1;
	}
	return 0;
}
