#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <utility>

extern "C" {
#include "nvkms-ioctl.h"
#include "nvkms-api.h"

#include "nv-haiku.h"
}

#include "ErrorUtils.h"
#include "FileDesc.h"

#include "NvRmApi.h"
#include "NvRmDevice.h"

#include "NvKmsApi.h"
#include "NvKmsDevice.h"
#include "NvKmsSurface.h"

#include "NvKmsBitmap.h"


struct ColorRgba {
	NvU8 r;
	NvU8 g;
	NvU8 b;
	NvU8 a;
};


static const NvU32 kInvalidHead = ~0U;


static const ColorRgba kPalette[] = {
	{0xff, 0x40, 0x40, 0xff},
	{0x40, 0xff, 0x40, 0xff},
	{0x40, 0x40, 0xff, 0xff},
	{0xff, 0xff, 0x40, 0xff},
	{0xff, 0x40, 0xff, 0xff},
	{0x40, 0xff, 0xff, 0xff},
	{0xff, 0x80, 0x00, 0xff},
	{0x80, 0x00, 0xff, 0xff},
};


static ColorRgba ColorForDpyId(NVDpyId dpyId)
{
	NvU32 idx = nvDpyIdToIndex(dpyId);
	return kPalette[idx % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

static void FillFramebuffer(NvKmsBitmap &bitmap, ColorRgba color)
{
	NvU32 pixel32 =
		((NvU32)color.a << 24) |
		((NvU32)color.r << 16) |
		((NvU32)color.g <<  8) |
		((NvU32)color.b      );

	auto line = (NvU8*)bitmap.Bits();
	int stride = bitmap.BytesPerRow();
	for (uint32 y = 0; y < bitmap.Height(); y++) {
		auto p = (NvU32*)line;
		for (uint32 x = 0; x < bitmap.Width(); x++) {
			p[x] = pixel32;
		}
		line += stride;
	}
}


struct DisplayState {
	NvU32 head;
	NvKmsBitmap framebuffer;
};


class NvKmsHotplugDemo {
private:
	FileDesc fDevFd;
	NvRmApi fRm;
	NvRmDevice fRmDev;
	NvKmsApi fKms;
	NvKmsDevice fKmsDev;
	NvKmsDeviceHandle fDeviceHandle;
	NvKmsDispHandle fDispHandle;
	NvU32 fNumHeads;
	NvU32 fHeadUsageMask;

	// keyed by NVDpyId.opaqueDpyId (as NvU32)
	std::map<NvU32, DisplayState> fDisplays;

	bool QueryConnected(NVDpyId dpyId);
	bool QueryPreferredMode(NVDpyId dpyId, NvKmsMode &outMode);
	NvU32 AllocateHead();

	void HandleConnected(NVDpyId dpyId);
	void HandleDisconnected(NVDpyId dpyId);
	void HandleDpyChanged(NVDpyId dpyId);

public:
	NvKmsHotplugDemo();

	void EnumerateInitialDisplays();
	void RunEventLoop();
};


NvKmsHotplugDemo::NvKmsHotplugDemo():
	fDevFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC)),
	fRmDev(fRm, 0),
	fKmsDev(fKms, 0),
	fHeadUsageMask(0)
{
	CheckErrno(fDevFd.Get());

	if (fKmsDev.Info().numDisps == 0) {
		RaiseErrno(ENODEV);
	}
	fDeviceHandle = fKmsDev.Info().deviceHandle;
	fDispHandle = fKmsDev.Info().dispHandles[0];
	fNumHeads = fKmsDev.Info().numHeads;

	{
		NvKmsGrabOwnershipParams params {};
		params.request.deviceHandle = fDeviceHandle;
		fKms.Control(NVKMS_IOCTL_GRAB_OWNERSHIP, &params, sizeof(params));
		printf("GrabOwnership (numHeads=%" PRIu32 ")\n", fNumHeads);
	}
	{
		NvKmsDeclareEventInterestParams params {};
		params.request.interestMask =
			(1U << NVKMS_EVENT_TYPE_DPY_CHANGED) |
			(1U << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED) |
			(1U << NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED);
		fKms.Control(NVKMS_IOCTL_DECLARE_EVENT_INTEREST, &params, sizeof(params));
	}
}

bool NvKmsHotplugDemo::QueryConnected(NVDpyId dpyId)
{
	NvKmsQueryDpyDynamicDataParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.dispHandle = fDispHandle;
	params.request.dpyId = dpyId;
	fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params));
	return params.reply.connected;
}

bool NvKmsHotplugDemo::QueryPreferredMode(NVDpyId dpyId, NvKmsMode &outMode)
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

NvU32 NvKmsHotplugDemo::AllocateHead()
{
	for (NvU32 h = 0; h < fNumHeads; h++) {
		if ((fHeadUsageMask & (1U << h)) == 0) {
			fHeadUsageMask |= (1U << h);
			return h;
		}
	}
	return kInvalidHead;
}

void NvKmsHotplugDemo::HandleConnected(NVDpyId dpyId)
{
	NvU32 raw = nvDpyIdToNvU32(dpyId);
	if (fDisplays.count(raw) != 0) {
		return;
	}

	NvKmsMode mode {};
	if (!QueryPreferredMode(dpyId, mode)) {
		printf("dpyId %" PRIu32 ": no preferred mode\n", nvDpyIdToIndex(dpyId));
		return;
	}

	NvU32 head = AllocateHead();
	if (head == kInvalidHead) {
		printf("dpyId %" PRIu32 ": no free heads\n", nvDpyIdToIndex(dpyId));
		return;
	}

	NvU32 width = mode.timings.hVisible;
	NvU32 height = mode.timings.vVisible;

	NvKmsBitmap framebuffer(fRmDev, fKmsDev, width, height, B_RGB32);
	ColorRgba color = ColorForDpyId(dpyId);
	FillFramebuffer(framebuffer, color);

	NvKmsSetModeParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.commit = true;
	params.request.requestedDispsBitMask = 1U << 0;
	params.request.disp[0].requestedHeadsBitMask = 1U << head;

	auto &headReq = params.request.disp[0].head[head];
	headReq.dpyIdList = nvAddDpyIdToEmptyDpyIdList(dpyId);
	headReq.mode = mode;
	headReq.viewPortOut = {.x = 0, .y = 0, .width = width, .height = height};
	headReq.viewPortSizeIn = {.width = width, .height = height};

	auto &layer = headReq.flip.layer[NVKMS_MAIN_LAYER];
	layer.surface.handle[0] = framebuffer.Surface().Get();
	layer.surface.specified = true;
	layer.sizeIn.val = {.width = width, .height = height};
	layer.sizeIn.specified = true;
	layer.sizeOut.val = {.width = width, .height = height};
	layer.sizeOut.specified = true;

	try {
		fKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params));
	} catch (const std::system_error &ex) {
		printf("[!] SetMode failed for dpyId %" PRIu32 ": %s\n", nvDpyIdToIndex(dpyId), ex.what());
		printf("    status=%d disp[0].status=%d head[%" PRIu32 "].status=%d\n",
			params.reply.status,
			params.reply.disp[0].status,
			head,
			params.reply.disp[0].head[head].status);
		fHeadUsageMask &= ~(1U << head);
		return;
	}

	printf("Connected dpyId %" PRIu32 " -> head %" PRIu32 ", mode %s (%" PRIu32 "x%" PRIu32 " @ %" PRIu32 ".%03" PRIu32 " Hz), RGB(%02x,%02x,%02x)\n",
		nvDpyIdToIndex(dpyId), head, mode.name, width, height,
		mode.timings.RRx1k / 1000, mode.timings.RRx1k % 1000,
		color.r, color.g, color.b);

	fDisplays.emplace(raw, DisplayState{head, std::move(framebuffer)});
}

void NvKmsHotplugDemo::HandleDisconnected(NVDpyId dpyId)
{
	NvU32 raw = nvDpyIdToNvU32(dpyId);
	auto it = fDisplays.find(raw);
	if (it == fDisplays.end()) {
		return;
	}
	NvU32 head = it->second.head;

	NvKmsSetModeParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.commit = true;
	params.request.requestedDispsBitMask = 1U << 0;
	params.request.disp[0].requestedHeadsBitMask = 1U << head;
	params.request.disp[0].head[head].dpyIdList = nvEmptyDpyIdList();

	try {
		fKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params));
	} catch (const std::system_error &ex) {
		printf("[!] SetMode (detach) failed for dpyId %" PRIu32 ": %s\n", nvDpyIdToIndex(dpyId), ex.what());
	}

	fHeadUsageMask &= ~(1U << head);
	fDisplays.erase(it);
	printf("Disconnected dpyId %" PRIu32 ", freed head %" PRIu32 "\n", nvDpyIdToIndex(dpyId), head);
}

void NvKmsHotplugDemo::HandleDpyChanged(NVDpyId dpyId)
{
	bool connected = QueryConnected(dpyId);
	NvU32 raw = nvDpyIdToNvU32(dpyId);
	bool tracked = fDisplays.count(raw) != 0;

	if (connected && !tracked) {
		HandleConnected(dpyId);
	} else if (!connected && tracked) {
		HandleDisconnected(dpyId);
	}
}

void NvKmsHotplugDemo::EnumerateInitialDisplays()
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
		if (QueryConnected(cur)) {
			HandleConnected(cur);
		}
		cur = nvNextDpyIdInDpyIdListUnsorted(cur, validDpys);
	}
}

void NvKmsHotplugDemo::RunEventLoop()
{
	for (;;) {
		NvKmsGetNextEventParams params {};
		fKms.Control(NVKMS_IOCTL_GET_NEXT_EVENT, &params, sizeof(params));
		if (!params.reply.valid) {
			struct pollfd pollFds[1] = {
				{
					.fd = fKms.Fd(),
					.events = POLLIN | POLLPRI,
				}
			};
			poll(pollFds, 1, -1);
			continue;
		}
		switch (params.reply.event.eventType) {
			case NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED:
				HandleConnected(params.reply.event.u.dynamicDpyConnected.dpyId);
				break;
			case NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED:
				HandleDisconnected(params.reply.event.u.dynamicDpyDisconnected.dpyId);
				break;
			case NVKMS_EVENT_TYPE_DPY_CHANGED:
				// Hotplug on non-dynamic connectors arrives as DPY_CHANGED.
				HandleDpyChanged(params.reply.event.u.dpyChanged.dpyId);
				break;
			default:
				break;
		}
	}
}


int main()
{
	try {
		NvKmsHotplugDemo demo;
		demo.EnumerateInitialDisplays();
		demo.RunEventLoop();
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvKmsHotplugDemo: %s\n", ex.what());
		return 1;
	}
	return 0;
}
