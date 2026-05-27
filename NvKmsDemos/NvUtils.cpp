#include "NvUtils.h"

#include <stdio.h>

extern "C" {
#include "nvos.h"
#include "class/cl003e.h" // NV01_MEMORY_SYSTEM
#include "class/cl0040.h" // NV01_MEMORY_LOCAL_USER
}

#include <NvRmApi.h>
#include <NvRmDevice.h>
#include <NvKmsDevice.h>
#include <ErrorUtils.h>


#define NVKMS_RM_HEAP_ID                    0xDCBA

#define NV_EVO_SURFACE_ALIGNMENT            0x1000


void nvKmsKapiAllocateSystemMemory(
	NvRmDevice &dev,
	NvKmsDevice &kmsDev,
	NvRmObject &hRmHandle,
	enum NvKmsSurfaceMemoryLayout layout,
	NvU64 size,
	enum NvKmsKapiAllocationType type,
	NvU8 *compressible
)
{
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
            RaiseErrno(EINVAL);
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
	            RaiseErrno(EINVAL);
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
            RaiseErrno(EINVAL);
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
}

void nvKmsKapiAllocateVideoMemory(
	NvRmDevice &dev,
	NvRmObject &hRmHandle,
	enum NvKmsSurfaceMemoryLayout layout,
	NvU64 size,
	enum NvKmsKapiAllocationType type,
	NvU8 *compressible
)
{
    NV_MEMORY_ALLOCATION_PARAMS memAllocParams = { };

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
            RaiseErrno(EINVAL);
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
	            RaiseErrno(EINVAL);
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
            RaiseErrno(EINVAL);
    }

    hRmHandle = dev.Device().Alloc(NV01_MEMORY_LOCAL_USER, &memAllocParams);

    if (FLD_TEST_DRF(OS32, _ATTR, _COMPR, _NONE, memAllocParams.attr)) {
        *compressible = 0;
    } else {
        *compressible = 1;
    }
}
