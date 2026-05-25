#include "NvKmsBitmap.h"

#include <ErrorUtils.h>
#include <NvKmsApi.h>

#include "NvUtils.h"


static inline size_t AlignUp(size_t val, size_t align)
{
	return (val + (align - 1)) / align * align;
}


NvKmsBitmap::NvKmsBitmap(NvRmDevice &rmDev, NvKmsDevice &kmsDev, uint32 width, uint32 height, color_space colorSpace):
	fWidth(width),
	fHeight(height),
	fBytesPerRow(AlignUp(4*width, 256)),
	fColorSpace(colorSpace)
{
	NvKmsSurfaceMemoryFormat nvKmsFormat;
	switch (colorSpace) {
		case B_RGB32:
			nvKmsFormat = NvKmsSurfaceMemoryFormatX8R8G8B8;
			break;
		case B_RGBA32:
			nvKmsFormat = NvKmsSurfaceMemoryFormatA8R8G8B8;
			break;
		default:
			RaiseErrno(EINVAL);
	}

	NvKmsApi &kms = kmsDev.Kms();

	uint32 size = fBytesPerRow*height;

	NvU8 compressible = 0;
	nvKmsKapiAllocateVideoMemory(rmDev, fMemory, NvKmsSurfaceMemoryLayoutPitch, AlignUp(size, B_PAGE_SIZE), NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT, &compressible);

	FileDesc memoryFd = rmDev.ExportObjectToFd(rmDev.Device().Get(), fMemory.Get());

	NvKmsRegisterSurfaceParams params {};
	params.request.deviceHandle = kmsDev.Get();
	params.request.useFd = true;
	params.request.planes[0].u.fd = memoryFd.Get();
	params.request.planes[0].offset = 0;
	params.request.planes[0].pitch = fBytesPerRow;
	params.request.planes[0].rmObjectSizeInBytes = size;
	params.request.widthInPixels = width;
	params.request.heightInPixels = height;
	params.request.layout = NvKmsSurfaceMemoryLayoutPitch;
	params.request.format = nvKmsFormat;

	kms.Control(NVKMS_IOCTL_REGISTER_SURFACE, &params, sizeof(params));
	fSurface = NvKmsSurface(kmsDev, params.reply.surfaceHandle);

	fMapping = rmDev.MapMemory(fMemory.Get(), false, 0, AlignUp(size, B_PAGE_SIZE), 0);
}
