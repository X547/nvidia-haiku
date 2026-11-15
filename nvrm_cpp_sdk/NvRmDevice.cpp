#include "NvRmDevice.h"

#include <assert.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#ifdef __HAIKU__
#include "nv-haiku.h"
#endif

extern "C" {
#include "class/cl0080.h" // NV01_DEVICE_0
#include "class/cl2080.h" // NV20_SUBDEVICE_0

#include "ctrl/ctrl0000/ctrl0000unix.h"
}

#include "ErrorUtils.h"


void NvRmMemoryMapping::Delete()
{
#ifdef __HAIKU__
	delete_area(fArea);
#else
	munmap(fAddress, fSize);
#endif
	fRm->UnmapMemory(fHDevice, fHMemory, fPLinearAddress, fFlags);
}


NvRmDevice::NvRmDevice(NvRmApi &rm, NvU32 deviceId)
{
	NV0080_ALLOC_PARAMETERS deviceParams = {
		.deviceId = deviceId,
		.hClientShare = rm.Client().Get()
	};
	fDevice = rm.Client().Alloc(NV01_DEVICE_0, &deviceParams);
	fSubdevice = fDevice.Alloc(NV20_SUBDEVICE_0, nullptr);
}


NvU32 NvRmDevice::DeviceId() const
{
	// TODO: implement
	return 0;
}

NvU32 NvRmDevice::DevFsIndex() const
{
	// TODO: implement
	return 0;
}

NvU32 NvRmDevice::GpuId() const
{
	// TODO: implement
	return 0;
}


NvRmMemoryMapping NvRmDevice::MapMemory(NvHandle hMemory, bool isSystemMemory, NvU64 offset, NvU64 length, NvU32 flags)
{
	NvRmApi &rm = *fDevice.Rm();

	void *pLinearAddress {};

	FileDesc memFd;
	if (isSystemMemory) {
		memFd = FileDesc(open("/dev/" NVIDIA_CONTROL_DEVICE_NAME, O_RDWR | O_CLOEXEC));
	} else {
		char fileName[256];
		sprintf(fileName, "/dev/" NVIDIA_DEVICE_NAME "%" PRIu32, DevFsIndex());
		memFd = FileDesc(open(fileName, O_RDWR | O_CLOEXEC));
	}
	assert(memFd.IsSet());

	rm.MapMemory(fDevice.Get(), hMemory, offset, length, pLinearAddress, flags, memFd.Get());

#ifdef __HAIKU__
	nv_haiku_map_params mapParams {
		.name = "NVRM",
		.addressSpec = B_ANY_ADDRESS,
		.protection = B_READ_AREA | B_WRITE_AREA | (isSystemMemory ? B_CLONEABLE_AREA : 0),
	};
	int ret = NvRmIoctl(memFd.Get(), NV_HAIKU_MAP, &mapParams, sizeof(mapParams));
	CheckErrno(ret); // TODO: call rm.UnmapMemory on fail
	return NvRmMemoryMapping(rm, fDevice.Get(), hMemory, pLinearAddress, flags, mapParams.address, length, ret);
#else
	void *address = (void*)mmap(0, length, PROT_READ|PROT_WRITE, MAP_SHARED, memFd.Get(), 0);
	assert(address != MAP_FAILED); // TODO: call rm.UnmapMemory on fail
	return NvRmMemoryMapping(rm, fDevice.Get(), hMemory, pLinearAddress, flags, address, length);
#endif
}

FileDesc NvRmDevice::ExportObjectToFd(NvHandle hParent, NvHandle hObject)
{
	NvRmApi &rm = *fDevice.Rm();
	FileDesc outFd(open("/dev/" NVIDIA_CONTROL_DEVICE_NAME, O_RDWR | O_CLOEXEC));
	CheckErrno(outFd.Get());
	NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TO_FD_PARAMS params {
		.object = {
			.type = NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TYPE_RM,
			.data = {
				.rmObject = {
					.hDevice = fDevice.Get(),
					.hParent = hParent,
					.hObject = hObject,
				},
			},
		},
		.fd = outFd.Get()
	};
	rm.Client().Control(NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD, &params, sizeof(params));
	return outFd.Detach();
}

NvRmObject NvRmDevice::ImportObjectFromFd(NvHandle hParent, int inFd)
{
	NvRmApi &rm = *fDevice.Rm();
	NV0000_CTRL_OS_UNIX_IMPORT_OBJECT_FROM_FD_PARAMS params {
		.fd = inFd,
		.object = {
			.type = NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TYPE_RM,
			.data = {
				.rmObject = {
					.hDevice = fDevice.Get(),
					.hParent = hParent,
				},
			},
		},
	};
	rm.Client().Control(NV0000_CTRL_CMD_OS_UNIX_IMPORT_OBJECT_FROM_FD, &params, sizeof(params));
	return NvRmObject(rm, params.object.data.rmObject.hObject);
}
