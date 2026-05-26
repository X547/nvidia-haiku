#include "Driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <AutoDeleter.h>

#include "nv-include.h"
#include "nv-haiku.h"
#include "nv-haiku-kernel.h"

#include "ControlDevice.h"
#include "Device.h"


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


NvHaikuDriver NvHaikuDriver::sInstance;


static bool ParseUint32(const char *str, char *&strEnd, uint32 &res)
{
	// do not allow leading spaces or zeroes
	if (!(strcmp(str, "0") == 0 || (str[0] >= '1' && str[0] <= '9'))) {
		return false;
	}
	long val = strtol(str, &strEnd, 10);
	if (strEnd == str) {
		return false;
	}
	if ((val == LONG_MIN || val == LONG_MAX) && errno == ERANGE) {
		return false;
	}
	if (val < 0 || val > UINT32_MAX) {
		return false;
	}
	res = val;
	return true;
}

static bool ParseDeviceName(const char *name, uint32 &index)
{
	const char *deviceNamePrefix = NVIDIA_DEVIVE_NAME;
	size_t deviceNamePrefixLen = strlen(deviceNamePrefix);
	if (strncmp(deviceNamePrefix, name, deviceNamePrefixLen) != 0) {
		return false;
	}

	char *strEnd {};
	if (!ParseUint32(name + deviceNamePrefixLen, strEnd, index)) {
		return false;
	}
	if (*strEnd != '\0') {
		return false;
	}
	return true;
}


NvHaikuDriver::~NvHaikuDriver()
{
	dprintf("-NvHaikuDriver\n");

	for (int32 i = 0; i < fDevices.Count(); i++) {
		delete fDevices[i];
	}

	if (fControlDevice != nullptr) {
		delete fControlDevice;
	}

	if (fInitRmDone) {
		rm_shutdown_rm(nullptr);
	}

	if (fWorkQueueInitDone) {
		fWorkQueue.Fini();
		fWorkQueueInitDone = false;
	}

	fIntrSafePool.ReclaimAll();

	if (fPCI != nullptr) {
		put_module(B_PCI_MODULE_NAME);
	}
}

status_t NvHaikuDriver::Init()
{
	dprintf("+NvHaikuDriver\n");

	CHECK_RET(get_module(B_PCI_MODULE_NAME, (module_info**)&fPCI));

	fIntrSafePool.Maintain();

	CHECK_RET(fWorkQueue.Init());
	fWorkQueueInitDone = true;

	if (!rm_init_rm(nullptr)) {
		return B_ERROR;
	}
	fInitRmDone = true;

	fControlDevice = new(std::nothrow) NvHaikuControlDevice();
	if (fControlDevice == nullptr) {
		return B_NO_MEMORY;
	}

	dprintf("Scan PCI:\n");
	pci_info pciInfo {};
	for (uint32 i = 0; fPCI->get_nth_pci_info(i, &pciInfo) >= B_OK; i++) {
		ObjectDeleter<NvHaikuDevice> device;
		if (NvHaikuDevice::Probe(device, pciInfo) < B_OK)
			continue;

		uint32 deviceIndex = fDevices.Count();
		CHECK_RET(fDevices.Add(device.Get()));
		device->SetDeviceIndex(deviceIndex);
		device.Detach();
	}

	CHECK_RET(fDeviceNamesArray.Init(1 + fDevices.Count()));
	fDeviceNamesArray.SetName(0, NVIDIA_CONTROL_DEVIVE_NAME);
	for (int32 i = 0; i < fDevices.Count(); i++) {
		char name[128];
		sprintf(name, NVIDIA_DEVIVE_NAME "%" B_PRId32, i);
		fDeviceNamesArray.SetName(1 + i, name);
	}

	return B_OK;
}

status_t NvHaikuDriver::InitDriver()
{
	dprintf("nvidia_gsp: InitDriver\n");
	module_info *moduleInfo;
	return get_module(NV_HAIKU_MODULE_NAME, &moduleInfo);
}

void NvHaikuDriver::UninitDriver()
{
	dprintf("nvidia_gsp: UninitDriver\n");
	put_module(NV_HAIKU_MODULE_NAME);
}

const char **NvHaikuDriver::PublishDevices()
{
	return Instance().fDeviceNamesArray.Names();
}

DevfsNodeInstance NvHaikuDriver::FindDevice(const char *name)
{
	if (strcmp(name, NVIDIA_CONTROL_DEVIVE_NAME) == 0) {
		return DevfsNodeInstance(&gNvHaikuControlDeviceClass, Instance().fControlDevice);
	}
	uint32 index;
	if (ParseDeviceName(name, index)) {
		if (index >= (uint32)Instance().fDevices.Count()) {
			return DevfsNodeInstance();
		}
		return DevfsNodeInstance(&gNvHaikuDeviceClass, Instance().fDevices[index]);
	}
	return DevfsNodeInstance();
}


DRIVER_ENTRY_POINTS(NvHaikuDriver)
