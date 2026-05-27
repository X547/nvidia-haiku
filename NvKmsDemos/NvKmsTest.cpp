#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "nvkms-ioctl.h"
#include "nvkms-api.h"
#include "nvkms-sync.h"

#include "nv-haiku.h"

#include "nvos.h"
#include "class/cl003e.h" // NV01_MEMORY_SYSTEM
#include "class/cl0040.h" // NV01_MEMORY_LOCAL_USER

#include <class/cl917c.h> /* NV_DISP_BASE_NOTIFIER_1, NV_DISP_NOTIFICATION_2 */
#include <class/clc37d.h> /* NV_DISP_NOTIFIER */
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


void WriteSet(FILE *f, uint32_t val)
{
	fprintf(f, "{");
	bool isFirst = true;
	for (uint32_t i = 0; i < 32; i++) {
		if (((1U << i) & val) == 0)
			continue;

		if (isFirst) {
			isFirst = false;
		} else {
			fprintf(f, ", ");
		}
		fprintf(f, "%" PRIu32, i);
	}
	fprintf(f, "}");
}


void AllocPhysicalMemory(NvRmDevice &dev, NvRmObject &hMemory, bool isSystemMemory, uint64_t length)
{
	bool bContiguousPhysAlloc = false;

	NvHandle hClass;
	const NvRmObject *hParent;

	NV_MEMORY_ALLOCATION_PARAMS memAllocParams {};

	memAllocParams.owner = 1;
	memAllocParams.size = length;
	memAllocParams.type = NVOS32_TYPE_IMAGE;

	if (isSystemMemory) {
		hClass = NV01_MEMORY_SYSTEM;
		hParent = &dev.Device();
		memAllocParams.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _PCI);
		memAllocParams.attr |= DRF_DEF(OS32, _ATTR, _COHERENCY, _CACHED);
	} else {
		hClass = NV01_MEMORY_LOCAL_USER;
		hParent = &dev.Device();
		memAllocParams.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM);
		memAllocParams.attr |= DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED);
	}
	if (bContiguousPhysAlloc) {
		memAllocParams.attr |= DRF_DEF(OS32, _ATTR, _PHYSICALITY, _CONTIGUOUS);
	} else {
		memAllocParams.attr |= DRF_DEF(OS32, _ATTR, _PHYSICALITY, _DEFAULT);
	}

	memAllocParams.attr |= DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB);

	hMemory = hParent->Alloc(hClass, &memAllocParams, sizeof(memAllocParams));
}


void EnumerateModes(NvKmsApi &nvKms, NvKmsDeviceHandle deviceHandle, NvKmsDispHandle dispHandle, NVDpyId dpyId)
{
	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = deviceHandle;
		params.request.dispHandle = dispHandle;
		params.request.dpyId = dpyId;
		params.request.modeIndex = i;
		nvKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params));
		if (params.reply.end)
			break;

		printf("mode %" PRIu32 "(%s)%s\n", i, params.reply.mode.name, params.reply.preferredMode ? "*" : "");
		printf("  pixelClockHz: %" PRIu64 "\n", params.reply.mode.timings.pixelClockHz);
		printf("  RRx1k: %" PRIu32 "\n", params.reply.mode.timings.RRx1k);
		printf("  hVisible: %" PRIu16 "\n", params.reply.mode.timings.hVisible);
		printf("  hSyncStart: %" PRIu16 "\n", params.reply.mode.timings.hSyncStart);
		printf("  hSyncEnd: %" PRIu16 "\n", params.reply.mode.timings.hSyncEnd);
		printf("  hTotal: %" PRIu16 "\n", params.reply.mode.timings.hTotal);
		printf("  hSkew: %" PRIu16 "\n", params.reply.mode.timings.hSkew);
		printf("  vVisible: %" PRIu16 "\n", params.reply.mode.timings.vVisible);
		printf("  vSyncStart: %" PRIu16 "\n", params.reply.mode.timings.vSyncStart);
		printf("  vSyncEnd: %" PRIu16 "\n", params.reply.mode.timings.vSyncEnd);
		printf("  vTotal: %" PRIu16 "\n", params.reply.mode.timings.vTotal);
		printf("  sizeMM.w: %" PRIu16 "\n", params.reply.mode.timings.sizeMM.w);
		printf("  sizeMM.h: %" PRIu16 "\n", params.reply.mode.timings.sizeMM.h);
		printf("  interlaced: %d\n", params.reply.mode.timings.interlaced);
		printf("  doubleScan: %d\n", params.reply.mode.timings.doubleScan);
		printf("  hSyncPos: %d\n", params.reply.mode.timings.hSyncPos);
		printf("  hSyncNeg: %d\n", params.reply.mode.timings.hSyncNeg);
		printf("  vSyncPos: %d\n", params.reply.mode.timings.vSyncPos);
		printf("  vSyncNeg: %d\n", params.reply.mode.timings.vSyncNeg);
		printf("  hdmi3D: %d\n", params.reply.mode.timings.hdmi3D);
		printf("  yuv420Mode: ");
		switch (params.reply.mode.timings.yuv420Mode) {
			case NV_YUV420_MODE_NONE: printf("NONE"); break;
			case NV_YUV420_MODE_SW: printf("SW"); break;
			case NV_YUV420_MODE_HW: printf("HW"); break;
			default: printf("?"); break;
		}
		printf("\n");
	}
}


NvKmsMode GetPreferredMode(NvKmsApi &nvKms, NvKmsDeviceHandle deviceHandle, NvKmsDispHandle dispHandle, NVDpyId dpyId)
{
	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = deviceHandle;
		params.request.dispHandle = dispHandle;
		params.request.dpyId = dpyId;
		params.request.modeIndex = i;
		nvKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params));
		if (params.reply.end)
			break;

		if (params.reply.preferredMode) {
			return params.reply.mode;
		}
	}
	abort();
}

NvKmsMode GetSelectedMode()
{
	NvKmsMode mode {
		.timings = {
			.RRx1k        =     60000, // 60.00Hz
			.pixelClockHz = 148500000, // 148.500MHz, 2200 * 1125 * 60
			.hVisible     =      1920,
			.hSyncStart   =      2008,
			.hSyncEnd     =      2052,
			.hTotal       =      2200,
			.vVisible     =      1080,
			.vSyncStart   =      1082,
			.vSyncEnd     =      1087,
			.vTotal       =      1125,
			.hSyncPos     = true,
			.vSyncPos     = true,
		},
		.name = "1920x1080",
	};
	return mode;
}

NvKmsMode GetSelectedMode2()
{
	NvKmsMode mode {
		.timings = {
			.RRx1k        =     60000, // 60.00Hz
			.pixelClockHz = 148500000, // 148.500MHz, 2200 * 1125 * 60
			.hVisible     =      1920,
			.hSyncStart   =      2008,
			.hSyncEnd     =      2052,
			.hTotal       =      2200,
			.vVisible     =      1080,
			.vSyncStart   =      1084,
			.vSyncEnd     =      1089,
			.vTotal       =      1125,
			.hSyncPos     = true,
			.vSyncPos     = true,
		},
		.name = "1920x1080",
	};
	return mode;
}


void PrintNotifier(enum NvKmsNIsoFormat format, NvBool overlay, NvU32 index, const void *base)
{
	nvKmsParsedNotifier parsedNotifier;
	nvKmsParseNotifier(NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY, overlay, index, base, &parsedNotifier);
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


// #pragma mark -

int main()
{
	FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
	CheckErrno(devFd.Get());

	NvU32 deviceId = 0;

	{

	NvRmApi rm;
	printf("hClient: %#" PRIx32 "\n", rm.Client().Get());
	NvRmDevice rmDev(rm, deviceId);
	printf("hDevice: %#" PRIx32 "\n", rmDev.Device().Get());
	printf("hSubdevice: %#" PRIx32 "\n", rmDev.Subdevice().Get());

	NvKmsApi nvKms;

	NvKmsDeviceHandle deviceHandle {};
	NvKmsDispHandle dispHandle = 1; // !!!
	NVDpyId dpyId;
	*reinterpret_cast<NvU32*>(&dpyId) = 1U << 13;
	printf("dpyId: "); WriteSet(stdout, *reinterpret_cast<NvU32*>(&dpyId)); printf("\n");

	NvKmsDevice nvKmsDevice(nvKms, deviceId);

	deviceHandle = nvKmsDevice.Info().deviceHandle;
	if (nvKmsDevice.Info().numDisps > 0) {
		dispHandle = nvKmsDevice.Info().dispHandles[0];
	}

	printf("AllocDevice\n");
	printf("  deviceHandle: %" PRIu32 "\n", nvKmsDevice.Info().deviceHandle);
	printf("  subDeviceMask: %#" PRIx32 "\n", nvKmsDevice.Info().subDeviceMask);
	printf("  numHeads: %" PRIu32 "\n", nvKmsDevice.Info().numHeads);
	printf("  numDisps: %" PRIu32 "\n", nvKmsDevice.Info().numDisps);
	for (NvU32 i = 0; i < nvKmsDevice.Info().numDisps; i++) {
		NvU32 val = reinterpret_cast<NvU32>(nvKmsDevice.Info().dispHandles[i]);
		printf("  dispHandles[%" PRIu32 "]: %" PRIu32 "\n", i, val);
	}
	printf("  requiresVrrSemaphores: %d\n", nvKmsDevice.Info().requiresVrrSemaphores);
	printf("  inputLutAppliesToBase: %d\n", nvKmsDevice.Info().inputLutAppliesToBase);
	printf("  supportsSwapGroups: %d\n", nvKmsDevice.Info().supportsSwapGroups);
	printf("  supportsWarpAndBlend: %d\n", nvKmsDevice.Info().supportsWarpAndBlend);
	printf("  nIsoSurfacesInVidmemOnly: %d\n", nvKmsDevice.Info().nIsoSurfacesInVidmemOnly);
	printf("  requiresAllAllocationsInSysmem: %d\n", nvKmsDevice.Info().requiresAllAllocationsInSysmem);
	printf("  supportsHeadSurface: %d\n", nvKmsDevice.Info().supportsHeadSurface);
	printf("  validNIsoFormatMask: {");
	{
		bool isFirst = true;
		for (NvU32 i = 0; i < 8; i++) {
			if (((1U << i) & nvKmsDevice.Info().validNIsoFormatMask) == 0) {
				continue;
			}
			if (isFirst) {
				isFirst = false;
			} else {
				printf(", ");
			}
			switch (i) {
				case NVKMS_NISO_FORMAT_LEGACY:
					printf("LEGACY");
					break;
				case NVKMS_NISO_FORMAT_FOUR_WORD:
					printf("FOUR_WORD");
					break;
				case NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY:
					printf("FOUR_WORD_NVDISPLAY");
					break;
				default:
					printf("?");
					break;
			}
		}
	}
	printf("}\n");
	printf("  surfaceAlignment: %" PRIu32 "\n", nvKmsDevice.Info().surfaceAlignment);
	printf("  maxWidthInBytes: %" PRIu32 "\n", nvKmsDevice.Info().maxWidthInBytes);
	printf("  maxWidthInPixels: %" PRIu32 "\n", nvKmsDevice.Info().maxWidthInPixels);
	printf("  maxHeightInPixels: %" PRIu32 "\n", nvKmsDevice.Info().maxHeightInPixels);
	printf("  maxCursorSize: %" PRIu32 "\n", nvKmsDevice.Info().maxCursorSize);
	printf("  maxCursorSize: %" PRIu32 "\n", nvKmsDevice.Info().maxCursorSize);
	printf("  isoIOCoherencyModes.coherent: %d\n", nvKmsDevice.Info().isoIOCoherencyModes.coherent);
	printf("  isoIOCoherencyModes.noncoherent: %d\n", nvKmsDevice.Info().isoIOCoherencyModes.noncoherent);
	printf("  nisoIOCoherencyModes.coherent: %d\n", nvKmsDevice.Info().nisoIOCoherencyModes.coherent);
	printf("  nisoIOCoherencyModes.noncoherent: %d\n", nvKmsDevice.Info().nisoIOCoherencyModes.noncoherent);
	printf("  displayIsGpuL2Coherent: %d\n", nvKmsDevice.Info().displayIsGpuL2Coherent);
	printf("  supportsSyncpts: %d\n", nvKmsDevice.Info().supportsSyncpts);
	printf("  supportsIndependentAcqRelSemaphore: %d\n", nvKmsDevice.Info().supportsIndependentAcqRelSemaphore);
	printf("  supportsVblankSyncObjects: %d\n", nvKmsDevice.Info().supportsVblankSyncObjects);
	printf("  supportsVblankSemControl: %d\n", nvKmsDevice.Info().supportsVblankSemControl);
#if 0
	printf("  supportsInputColorSpace: %d\n", nvKmsDevice.Info().supportsInputColorSpace);
	printf("  supportsInputColorRange: %d\n", nvKmsDevice.Info().supportsInputColorRange);
	printf("  vtFbBaseAddress: %#" PRIx64 "\n", nvKmsDevice.Info().vtFbBaseAddress);
	printf("  vtFbSize: %#" PRIx64 "\n", nvKmsDevice.Info().vtFbSize);
#endif
	{
		NvKmsGrabOwnershipParams params {};
		params.request.deviceHandle = deviceHandle;
		nvKms.Control(NVKMS_IOCTL_GRAB_OWNERSHIP, &params, sizeof(params));
		printf("GrabOwnership\n");
	}
	{
		NvKmsQueryDispParams params {};
		params.request.deviceHandle = deviceHandle;
		params.request.dispHandle = dispHandle;
		nvKms.Control(NVKMS_IOCTL_QUERY_DISP, &params, sizeof(params));
		printf("QueryDisp\n");
		printf("  validDpys: "); WriteSet(stdout, *reinterpret_cast<NvU32*>(&params.reply.validDpys)); printf("\n");
		printf("  bootDpys: "); WriteSet(stdout, *reinterpret_cast<NvU32*>(&params.reply.bootDpys)); printf("\n");
		printf("  muxDpys: "); WriteSet(stdout, *reinterpret_cast<NvU32*>(&params.reply.muxDpys)); printf("\n");
		printf("  gpuString: \"%s\"\n", params.reply.gpuString);
	}

	// calling QueryDpyDynamicData at least once is required for proper mode enumaration ands validating
	{
		NvKmsQueryDpyDynamicDataParams params {};
		params.request.deviceHandle = deviceHandle;
		params.request.dispHandle = dispHandle;
		params.request.dpyId = dpyId;
		nvKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params));
		printf("QueryDpyDynamicData\n");
		printf("  name: \"%s\"\n", params.reply.name);
		printf("  maxPixelClockKHz: %" PRIu32 "\n", params.reply.maxPixelClockKHz);
		printf("  connected: %d\n", params.reply.connected);
		printf("  isVirtualRealityHeadMountedDisplay: %d\n", params.reply.isVirtualRealityHeadMountedDisplay);
		printf("  physicalDimensions.heightInCM: %" PRIu8 "\n", params.reply.physicalDimensions.heightInCM);
		printf("  physicalDimensions.widthInCM: %" PRIu8 "\n", params.reply.physicalDimensions.widthInCM);
		printf("  vrrType: %d\n", params.reply.vrrType); // TODO: enum
		printf("  supportsHDR: %d\n", params.reply.supportsHDR);
		printf("  dp.guid.valid: %d\n", params.reply.dp.guid.valid);
		printf("  edid.valid: %d\n", params.reply.edid.valid);
	}

	EnumerateModes(nvKms, deviceHandle, dispHandle, dpyId);

	{
		char infoString[NVKMS_MODE_VALIDATION_MAX_INFO_STRING_LENGTH] {};
		NvKmsValidateModeParams params {};
		params.request.deviceHandle = deviceHandle;
		params.request.dispHandle = dispHandle;
		params.request.dpyId = dpyId;
		params.request.mode = GetSelectedMode2();
		params.request.infoStringSize = NVKMS_MODE_VALIDATION_MAX_INFO_STRING_LENGTH;
		params.request.pInfoString = nvKmsPointerToNvU64(infoString);
		nvKms.Control(NVKMS_IOCTL_VALIDATE_MODE, &params, sizeof(params));
		printf("ValidateMode\n");
		printf("  valid: %d\n", params.reply.valid);
		printf("%s", infoString);
	}

	NvRmObject hSemaphoreMemory;
	NvU64 semaphoreMemorySize = 4096;

	{
	NvU8 compressible = false;
	nvKmsKapiAllocateSystemMemory(
		rmDev,
		nvKmsDevice,
		hSemaphoreMemory,
		NvKmsSurfaceMemoryLayoutPitch,
		semaphoreMemorySize,
		NVKMS_KAPI_ALLOCATION_TYPE_NOTIFIER,
		&compressible
	);
	}

	AllocPhysicalMemory(rmDev, hSemaphoreMemory, true, semaphoreMemorySize);
	printf("hSemaphoreMemory: %#" PRIx32 "\n", hSemaphoreMemory.Get());
	NvRmMemoryMapping semaphoreMemoryMap = rmDev.MapMemory(hSemaphoreMemory.Get(), true, 0, semaphoreMemorySize, 0);
	NvKmsSurface semaphoreKmsSurface;
	NvU64 semaphoreValue = 0;
	{
		FileDesc memoryFd = rmDev.ExportObjectToFd(rmDev.Device().Get(), hSemaphoreMemory.Get());

		NvKmsRegisterSurfaceParams params {};
		params.request.deviceHandle = deviceHandle;
		params.request.useFd = true;
		params.request.planes[0].u.fd = memoryFd.Get();
		params.request.planes[0].pitch = semaphoreMemorySize;
		params.request.planes[0].rmObjectSizeInBytes = semaphoreMemorySize;
		params.request.widthInPixels = semaphoreMemorySize;
		params.request.heightInPixels = 1;
		params.request.layout = NvKmsSurfaceMemoryLayoutPitch;
		params.request.format = NvKmsSurfaceMemoryFormatI8;
		params.request.isoType = NVKMS_MEMORY_NISO;

		nvKms.Control(NVKMS_IOCTL_REGISTER_SURFACE, &params, sizeof(params));
		semaphoreKmsSurface = NvKmsSurface(nvKmsDevice, params.reply.surfaceHandle);
		printf("semaphoreKmsSurface: %#" PRIx32 "\n", semaphoreKmsSurface.Get());
	}
	nvKmsResetSemaphore(NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY, 0, semaphoreMemoryMap.Address(), 0);

	NvU32 framebufferWidth = 1920;
	NvU32 framebufferHeight = 1080;

	NvU32 currentSurface = 0;

	NvKmsBitmap bitmap[2];
	for (int i = 0; i < 2; i++) {
		bitmap[i] = NvKmsBitmap(rmDev, nvKmsDevice, framebufferWidth, framebufferHeight, B_RGB32);

		printf("surfaceHandle[%d]: %#" PRIx32 "\n", i, bitmap[i].Surface().Get());
	}

	{
		NvKmsDeclareEventInterestParams params {};
		params.request.interestMask = 1U << NVKMS_EVENT_TYPE_FLIP_OCCURRED;
		nvKms.Control(NVKMS_IOCTL_DECLARE_EVENT_INTEREST, &params, sizeof(params));
		printf("DeclareEventInterest\n");
	}

	nvKmsResetNotifier(NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY, false, 0, semaphoreMemoryMap.Address());
	nvKmsResetNotifier(NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY, false, 1, semaphoreMemoryMap.Address());
	for (int i = 0; i < 2; i++) {
		printf("notifier[%d]: ", i);
		PrintNotifier(NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY, false, i, semaphoreMemoryMap.Address());
		printf("\n");
	}

	{
		NvKmsSetModeParams params {};
		params.request.deviceHandle = deviceHandle;
		params.request.commit = true;
		params.request.requestedDispsBitMask |= 1U << 0;
		params.request.disp[0].requestedHeadsBitMask |= 1U << 0;
		params.request.disp[0].head[0].dpyIdList = nvAddDpyIdToEmptyDpyIdList(dpyId);
		params.request.disp[0].head[0].mode = GetPreferredMode(nvKms, deviceHandle, dispHandle, dpyId);
//		params.request.disp[0].head[0].mode = GetSelectedMode();
		params.request.disp[0].head[0].viewPortOut = {.x = 0, .y = 0, .width = framebufferWidth, .height = framebufferHeight};
		params.request.disp[0].head[0].viewPortSizeIn = {.width = framebufferWidth, .height = framebufferHeight};
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].surface.handle[0] = bitmap[currentSurface].Surface().Get();
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].surface.specified = true;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeIn.val = {.width = framebufferWidth, .height = framebufferHeight};
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeIn.specified = true;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeOut.val = {.width = framebufferWidth, .height = framebufferHeight};
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeOut.specified = true;
#if 0
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.useSyncpt = false;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.acquire.surface.surfaceHandle = semaphoreKmsSurface.Get();
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.acquire.surface.format = NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.acquire.surface.offsetInWords = 0;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.acquire.value = semaphoreValue;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.release.surface.surfaceHandle = semaphoreKmsSurface.Get();
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.release.surface.format = NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.release.surface.offsetInWords = 0;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.release.value = ++semaphoreValue;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].syncObjects.specified = true;
#endif
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].completionNotifier.val.surface.surfaceHandle = semaphoreKmsSurface.Get();
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].completionNotifier.val.surface.format = NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].completionNotifier.val.surface.offsetInWords = 0;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].completionNotifier.val.awaken = true;
		params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].completionNotifier.specified = true;

		nvKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params));
		printf("SetMode\n");
		printf("  status: %d\n", params.reply.status);
		printf("  disp[0].status: %d\n", params.reply.disp[0].status);
		printf("  disp[0].head[0].status: %d\n", params.reply.disp[0].head[0].status);
	}

#if 0
	{
		NvKmsSetDpyAttributeParams params {};
		params.request.deviceHandle = deviceHandle;
		params.request.dispHandle = dispHandle;
		params.request.dpyId = dpyId;
		params.request.attribute = NV_KMS_DPY_ATTRIBUTE_DPMS;
		params.request.value = NV_KMS_DPY_ATTRIBUTE_DPMS_ON;
		nvKms.Control(NVKMS_IOCTL_SET_DPY_ATTRIBUTE, &params, sizeof(params));
		printf("SetDpyAttribute\n");
	}
#endif

	int phase = 0;
	auto ProduceNextFrame = [&] () {
		currentSurface = (currentSurface + 1) % 2;
		phase = (phase + 1) % 0x100;

		{
			int stride = framebufferWidth*4;
			auto line = (NvU8*)bitmap[currentSurface].Bits();
			for (int y = 0; y < framebufferHeight; y++) {
				auto color = (ColorRgba*)line;
				for (int x = 0; x < framebufferWidth; x++) {
					color->r = (x + phase) % 0x100;
					color->g = (y + phase) % 0x100;
					color->b = 0x80;
					color->a = 0xff;
					color++;
				}
				line += stride;
			}
		}

#if 0
		nvKmsParsedSemaphore semVal;
		nvKmsParseSemaphore(NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY, 0, semaphoreMemoryMap.Address(), &semVal);
		printf("sem: %" PRIu32 "\n", semVal.payload);
		nvKmsResetSemaphore(NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY, 0, semaphoreMemoryMap.Address(), 0);
#endif

		for (int i = 0; i < 2; i++) {
			printf("notifier[%d]: ", i);
			PrintNotifier(NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY, false, i, semaphoreMemoryMap.Address());
			printf("\n");
		}

		NvKmsFlipRequestOneHead flipHead {};
		flipHead.flip.layer[NVKMS_MAIN_LAYER].surface.handle[0] = bitmap[currentSurface].Surface().Get();
		flipHead.flip.layer[NVKMS_MAIN_LAYER].surface.specified = true;
#if 0
		flipHead.flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.useSyncpt = false;
		flipHead.flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.acquire.surface.surfaceHandle = semaphoreKmsSurface.Get();
		flipHead.flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.acquire.surface.format = NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY;
		flipHead.flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.acquire.surface.offsetInWords = 0;
		flipHead.flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.acquire.value = semaphoreValue;
		flipHead.flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.release.surface.surfaceHandle = semaphoreKmsSurface.Get();
		flipHead.flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.release.surface.format = NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY;
		flipHead.flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.release.surface.offsetInWords = 0;
		flipHead.flip.layer[NVKMS_MAIN_LAYER].syncObjects.val.u.semaphores.release.value = ++semaphoreValue;
		flipHead.flip.layer[NVKMS_MAIN_LAYER].syncObjects.specified = true;
#endif
#if 1
		flipHead.flip.layer[NVKMS_MAIN_LAYER].completionNotifier.val.surface.surfaceHandle = semaphoreKmsSurface.Get();
		flipHead.flip.layer[NVKMS_MAIN_LAYER].completionNotifier.val.surface.format = NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY;
		flipHead.flip.layer[NVKMS_MAIN_LAYER].completionNotifier.val.surface.offsetInWords = currentSurface * 0x10 / 4;
		flipHead.flip.layer[NVKMS_MAIN_LAYER].completionNotifier.val.awaken = true;
		flipHead.flip.layer[NVKMS_MAIN_LAYER].completionNotifier.specified = true;
#endif
		NvKmsFlipParams flipParams {};
		flipParams.request.deviceHandle = deviceHandle;
		flipParams.request.pFlipHead = nvKmsPointerToNvU64(&flipHead);
		flipParams.request.numFlipHeads = 1;
		flipParams.request.commit = true;
		nvKms.Control(NVKMS_IOCTL_FLIP, &flipParams, sizeof(flipParams));
		printf("Flip\n");
	};

#if 0
	for (;;) {
		//usleep(1000000 / 120);
		//printf("[WAIT]"); fgetc(stdin);
#if 0
		{
			NvKmsGetNextEventParams params {};
			nvKms.Control(NVKMS_IOCTL_GET_NEXT_EVENT, &params, sizeof(params));
			if (params.reply.valid) {
				switch (params.reply.event.eventType) {
					case NVKMS_EVENT_TYPE_FLIP_OCCURRED: {
						printf("FlipOccured\n");
						break;
					}
					default:
						break;
				}
			}
		}
#endif
		ProduceNextFrame();
	}
#endif

#if 1
	{
		ProduceNextFrame();
		for (;;) {
			NvKmsGetNextEventParams params {};
			nvKms.Control(NVKMS_IOCTL_GET_NEXT_EVENT, &params, sizeof(params));
			if (!params.reply.valid) {
				struct pollfd pollFds[1] = {
					{
						.fd = nvKms.Fd(),
						.events = POLLIN|POLLPRI
					}
				};
				poll(pollFds, 1, -1);
				continue;
			}
			switch (params.reply.event.eventType) {
				case NVKMS_EVENT_TYPE_FLIP_OCCURRED: {
					printf("FlipOccured\n");
					ProduceNextFrame();
					break;
				}
			}
		}
	}
#endif

	printf("[WAIT]"); fgetc(stdin);

	}

	return 0;
}
