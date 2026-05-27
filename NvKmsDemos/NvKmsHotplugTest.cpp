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


#define NVKMS_RM_HEAP_ID                    0xDCBA

#define NV_EVO_SURFACE_ALIGNMENT            0x1000

enum NvKmsKapiAllocationType {
    NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT = 0,
    NVKMS_KAPI_ALLOCATION_TYPE_NOTIFIER = 1,
    NVKMS_KAPI_ALLOCATION_TYPE_OFFSCREEN = 2,
};


NvBool nvKmsKapiAllocateSystemMemory(NvRmDevice &dev,
                                     NvKmsDevice &kmsDev,
                                     NvRmObject &hRmHandle,
                                     enum NvKmsSurfaceMemoryLayout layout,
                                     NvU64 size,
                                     enum NvKmsKapiAllocationType type,
                                     NvU8 *compressible)
{
    NvU32 ret;
    NV_MEMORY_ALLOCATION_PARAMS memAllocParams = { };
    const NvKmsDispIOCoherencyModes *pIOCoherencyModes = NULL;

    memAllocParams.owner = NVKMS_RM_HEAP_ID;
    memAllocParams.size = size;

    switch (layout) {
        case NvKmsSurfaceMemoryLayoutBlockLinear:
            memAllocParams.attr =
                FLD_SET_DRF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR,
                            memAllocParams.attr);
            if (*compressible) {
                /*
                 * RM will choose a compressed page kind and hence allocate
                 * comptags for color surfaces >= 32bpp.  The actual kind
                 * chosen isn't important, as it can be overridden by creating
                 * a virtual alloc with a different kind when mapping the
                 * memory into the GPU.
                 */
                memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _DEPTH, _32,
                                                  memAllocParams.attr);
                memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _COMPR, _ANY,
                                                  memAllocParams.attr);
            } else {
                memAllocParams.attr =
                    FLD_SET_DRF(OS32, _ATTR, _DEPTH, _UNKNOWN,
                                memAllocParams.attr);
            }
            break;

        case NvKmsSurfaceMemoryLayoutPitch:
            memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _FORMAT, _PITCH,
                                              memAllocParams.attr);
            break;

        default:
            printf("Unknown Memory Layout");
            return NV_FALSE;
    }

    switch (type) {
        case NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT:
            /* XXX Note compression and scanout do not work together on
             * any current GPUs.  However, some use cases do involve scanning
             * out a compression-capable surface:
             *
             * 1) Mapping the compressible surface as non-compressed when
             *    generating its content.
             *
             * 2) Using decompress-in-place to decompress the surface content
             *    before scanning it out.
             *
             * Hence creating compressed allocations of TYPE_SCANOUT is allowed.
             */

            pIOCoherencyModes = &kmsDev.Info().isoIOCoherencyModes;

            memAllocParams.attr2 = FLD_SET_DRF(OS32, _ATTR2, _ISO,
                                               _YES, memAllocParams.attr2);

            break;
        case NVKMS_KAPI_ALLOCATION_TYPE_NOTIFIER:
            if (layout == NvKmsSurfaceMemoryLayoutBlockLinear) {
                printf("Attempting creation of BlockLinear notifier memory");
                return NV_FALSE;
            }

            memAllocParams.attr2 = FLD_SET_DRF(OS32, _ATTR2, _NISO_DISPLAY,
                                               _YES, memAllocParams.attr2);

            pIOCoherencyModes = &kmsDev.Info().nisoIOCoherencyModes;

            break;
        case NVKMS_KAPI_ALLOCATION_TYPE_OFFSCREEN:
            memAllocParams.flags |= NVOS32_ALLOC_FLAGS_NO_SCANOUT;
            break;
        default:
            printf("Unknown Allocation Type");
            return NV_FALSE;
    }

    memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _LOCATION, _PCI,
                                      memAllocParams.attr);
    memAllocParams.attr2 = FLD_SET_DRF(OS32, _ATTR2, _GPU_CACHEABLE, _NO,
                                       memAllocParams.attr2);

    if (pIOCoherencyModes == NULL || !pIOCoherencyModes->coherent) {
        memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _COHERENCY,
                                          _WRITE_COMBINE, memAllocParams.attr);
    } else {
        memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _COHERENCY,
                                          _WRITE_BACK, memAllocParams.attr);
    }

    memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _PHYSICALITY, _NONCONTIGUOUS,
                                      memAllocParams.attr);

    hRmHandle = dev.Device().Alloc(NV01_MEMORY_SYSTEM, &memAllocParams);

    if (FLD_TEST_DRF(OS32, _ATTR, _COMPR, _NONE,
                     memAllocParams.attr)) {
        *compressible = 0;
    } else {
        *compressible = 1;
    }

    return TRUE;
}

NvBool nvKmsKapiAllocateVideoMemory(NvRmDevice &dev,
                                    NvRmObject &hRmHandle,
                                    enum NvKmsSurfaceMemoryLayout layout,
                                    NvU64 size,
                                    enum NvKmsKapiAllocationType type,
                                    NvU8 *compressible)
{
    NV_MEMORY_ALLOCATION_PARAMS memAllocParams = { };
    NvU32 ret;

    memAllocParams.owner = NVKMS_RM_HEAP_ID;
    memAllocParams.size = size;

    switch (layout) {
        case NvKmsSurfaceMemoryLayoutBlockLinear:
            memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR, memAllocParams.attr);

            if (*compressible) {
                /*
                 * RM will choose a compressed page kind and hence allocate
                 * comptags for color surfaces >= 32bpp.  The actual kind
                 * chosen isn't important, as it can be overridden by creating
                 * a virtual alloc with a different kind when mapping the
                 * memory into the GPU.
                 */
                memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _DEPTH, _32, memAllocParams.attr);
                memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _COMPR, _ANY, memAllocParams.attr);
            } else {
                memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _DEPTH, _UNKNOWN, memAllocParams.attr);
            }
            break;

        case NvKmsSurfaceMemoryLayoutPitch:
            memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _FORMAT, _PITCH, memAllocParams.attr);
            break;

        default:
            printf("Unknown Memory Layout");
            return NV_FALSE;
    }


    memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _LOCATION, _VIDMEM, memAllocParams.attr);
    memAllocParams.attr2 = FLD_SET_DRF(OS32, _ATTR2, _GPU_CACHEABLE, _NO, memAllocParams.attr2);

    switch (type) {
        case NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT:
            /* XXX [JRJ] Not quite right.  This can also be used to allocate
             * cursor images.  The stuff RM does with this field is kind of
             * black magic, and I can't tell if it actually matters.
             */
            memAllocParams.type = NVOS32_TYPE_PRIMARY;

            memAllocParams.alignment = NV_EVO_SURFACE_ALIGNMENT;
            memAllocParams.flags |=
                NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE |   /* Pick up above EVO alignment */
                NVOS32_ALLOC_FLAGS_FORCE_MEM_GROWS_UP; /* X sets this for cursors */
            memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _PHYSICALITY, _CONTIGUOUS, memAllocParams.attr);

            memAllocParams.attr2 = FLD_SET_DRF(OS32, _ATTR2, _ISO, _YES, memAllocParams.attr2);

            /* XXX [JRJ] Note compression and scanout do not work together on
             * any current GPUs.  However, some use cases do involve scanning
             * out a compression-capable surface:
             *
             * 1) Mapping the compressible surface as non-compressed when
             *    generating its content.
             *
             * 2) Using decompress-in-place to decompress the surface content
             *    before scanning it out.
             *
             * Hence creating compressed allocations of TYPE_SCANOUT is allowed.
             */

            break;
        case NVKMS_KAPI_ALLOCATION_TYPE_NOTIFIER:
            if (layout == NvKmsSurfaceMemoryLayoutBlockLinear) {
                printf("Attempting creation of BlockLinear notifier memory");
                return NV_FALSE;
            }

            memAllocParams.type = NVOS32_TYPE_DMA;

            memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _PAGE_SIZE, _4KB, memAllocParams.attr);
            memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _COHERENCY, _UNCACHED, memAllocParams.attr);

            break;
        case NVKMS_KAPI_ALLOCATION_TYPE_OFFSCREEN:
            memAllocParams.type = NVOS32_TYPE_IMAGE;
            memAllocParams.flags |=
                NVOS32_ALLOC_FLAGS_NO_SCANOUT |
                NVOS32_ALLOC_FLAGS_FORCE_MEM_GROWS_UP;
            memAllocParams.attr = FLD_SET_DRF(OS32, _ATTR, _PHYSICALITY, _NONCONTIGUOUS, memAllocParams.attr);
            break;
        default:
            printf("Unknown Allocation Type");
            return NV_FALSE;
    }

    hRmHandle = dev.Device().Alloc(NV01_MEMORY_LOCAL_USER, &memAllocParams);

    if (FLD_TEST_DRF(OS32, _ATTR, _COMPR, _NONE, memAllocParams.attr)) {
        *compressible = 0;
    } else {
        *compressible = 1;
    }

    return NV_TRUE;
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
	FileDesc devFd(open("/dev/graphics/nvidia0", O_RDWR | O_CLOEXEC));
	CheckErrno(devFd.Get());

	NvU32 deviceId = 0;

	NvRmApi rm;
	NvRmDevice rmDev(rm, deviceId);
	NvKmsApi nvKms;

	NvKmsDeviceHandle deviceHandle {};
	NvKmsDispHandle dispHandle = 1; // !!!
	NVDpyId dpyId;

	NvKmsDevice nvKmsDevice(nvKms, deviceId);

	deviceHandle = nvKmsDevice.Info().deviceHandle;
	if (nvKmsDevice.Info().numDisps > 0) {
		dispHandle = nvKmsDevice.Info().dispHandles[0];
	}

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

	{
		NvKmsDeclareEventInterestParams params {};
		params.request.interestMask =
			(1U << NVKMS_EVENT_TYPE_DPY_CHANGED) |
			(1U << NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED) |
			(1U << NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED);
		nvKms.Control(NVKMS_IOCTL_DECLARE_EVENT_INTEREST, &params, sizeof(params));
		printf("DeclareEventInterest\n");
	}

	for (;;) {
		NvKmsGetNextEventParams nextEventParams {};
		nvKms.Control(NVKMS_IOCTL_GET_NEXT_EVENT, &nextEventParams, sizeof(nextEventParams));
		if (!nextEventParams.reply.valid) {
			struct pollfd pollFds[1] = {
				{
					.fd = nvKms.Fd(),
					.events = POLLIN|POLLPRI
				}
			};
			poll(pollFds, 1, -1);
			continue;
		}
		switch (nextEventParams.reply.event.eventType) {
			case NVKMS_EVENT_TYPE_DPY_CHANGED: {
				printf("DpyChanged(");
				printf("dpyId: "); WriteSet(stdout, *reinterpret_cast<NvU32*>(&nextEventParams.reply.event.u.dpyChanged.dpyId));
				printf(")\n");
				{
					NvKmsQueryDpyDynamicDataParams params {};
					params.request.deviceHandle = nextEventParams.reply.event.u.dpyChanged.deviceHandle;
					params.request.dispHandle = nextEventParams.reply.event.u.dpyChanged.dispHandle;
					params.request.dpyId = nextEventParams.reply.event.u.dpyChanged.dpyId;
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
				break;
			}
			// NOTE: Display hotplug event may be delivered as `NVKMS_EVENT_TYPE_DPY_CHANGED`.
			case NVKMS_EVENT_TYPE_DYNAMIC_DPY_CONNECTED: {
				printf("DpyConnected\n");
				break;
			}
			case NVKMS_EVENT_TYPE_DYNAMIC_DPY_DISCONNECTED: {
				printf("DpyDisconnected\n");
				break;
			}
		}
	}

	printf("[WAIT]"); fgetc(stdin);

	return 0;
}
