#pragma once

extern "C" {
#include "nvkms-api.h"
}


class NvRmDevice;
class NvKmsDevice;
class NvRmObject;


enum NvKmsKapiAllocationType {
    NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT = 0,
    NVKMS_KAPI_ALLOCATION_TYPE_NOTIFIER = 1,
    NVKMS_KAPI_ALLOCATION_TYPE_OFFSCREEN = 2,
};


void nvKmsKapiAllocateSystemMemory(
	NvRmDevice &dev,
	NvKmsDevice &kmsDev,
	NvRmObject &hRmHandle,
	enum NvKmsSurfaceMemoryLayout layout,
	NvU64 size,
	enum NvKmsKapiAllocationType type,
	NvU8 *compressible
);

void nvKmsKapiAllocateVideoMemory(
	NvRmDevice &dev,
	NvRmObject &hRmHandle,
	enum NvKmsSurfaceMemoryLayout layout,
	NvU64 size,
	enum NvKmsKapiAllocationType type,
	NvU8 *compressible
);
