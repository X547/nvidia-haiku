#pragma once

#include <PCI.h>

#include <util/Vector.h>

#include "Drivers_cpp.h"
#include "DeviceNamesArray.h"

#include "IntrSafePool.h"
#include "WorkQueue.h"


class NvHaikuControlDevice;
class NvHaikuDevice;


class NvHaikuDriver {
private:
	pci_module_info *fPCI {};
	class IntrSafePool fIntrSafePool;
	os_work_queue fWorkQueue;
	bool fWorkQueueInitDone = false;

	bool fInitRmDone = false;

	NvHaikuControlDevice *fControlDevice {};
	Vector<NvHaikuDevice*> fDevices;
	DeviceNamesArray fDeviceNamesArray;

	static NvHaikuDriver sInstance;

public:
	~NvHaikuDriver();
	status_t Init();

	static NvHaikuDriver &Instance() {return sInstance;}
	pci_module_info &PCI() {return *fPCI;}
	class IntrSafePool &IntrSafePool() {return fIntrSafePool;}
	os_work_queue *WorkQueue() {return fWorkQueueInitDone ? &fWorkQueue : nullptr;}

	uint32 DeviceCount() {return fDevices.Count();}
	NvHaikuDevice *DeviceAt(uint32 index) {return fDevices[index];}

	static status_t InitDriver();
	static void UninitDriver();
	static const char **PublishDevices();
	static DevfsNodeInstance FindDevice(const char *name);
};
