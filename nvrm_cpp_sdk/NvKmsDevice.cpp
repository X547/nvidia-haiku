#include "NvKmsDevice.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <string.h>

extern "C" {
#include "nvUnixVersion.h"
}

#include "NvKmsApi.h"


NvKmsDevice::NvKmsDevice(NvKmsApi &nvKmsApi, NvU32 rmDeviceId):
	fNvKmsApi(nvKmsApi)
{
	NvKmsAllocDeviceParams params {};
	strcpy(params.request.versionString, NV_VERSION_STRING);
#if 1
	params.request.deviceId = rmDeviceId;
#else
	params.request.deviceId.rmDeviceId = rmDeviceId;
#endif
	fNvKmsApi.Control(NVKMS_IOCTL_ALLOC_DEVICE, &params, sizeof(params));
	memcpy(&fInfo, &params.reply, sizeof(fInfo));
}

NvKmsDevice::~NvKmsDevice()
{
	NvKmsFreeDeviceParams params {};
	params.request.deviceHandle = fInfo.deviceHandle;
	try {
		fNvKmsApi.Control(NVKMS_IOCTL_FREE_DEVICE, &params, sizeof(params));
	} catch (...) {
	}
}
