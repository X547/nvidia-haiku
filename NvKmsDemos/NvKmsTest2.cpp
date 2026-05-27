#include <fcntl.h>
#include <unistd.h>

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "ErrorUtils.h"
#include "FileDesc.h"

#include "NvRmApi.h"
#include "NvRmDevice.h"

#include "NvKmsApi.h"
#include "NvKmsDevice.h"
#include "NvKmsSurface.h"

#include "NvKmsBitmap.h"

extern "C" {
#include "nv-haiku.h"
}


int main()
{
	FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
	CheckErrno(devFd.Get());

	NvU32 deviceId = 0;

	NvRmApi rm;
	NvRmDevice rmDev(rm, deviceId);

	NvKmsApi nvKms;
	NvKmsDevice nvKmsDevice(nvKms, deviceId);
	NvKmsDeviceHandle deviceHandle = nvKmsDevice.Info().deviceHandle;

	for (int32_t i = 0; i < 1000; i++) {
		NvKmsBitmap bitmap = NvKmsBitmap(rmDev, nvKmsDevice, 1024, 768, B_RGB32);
		printf("%" PRId32 "\n", i);
	}

	return 0;
}
