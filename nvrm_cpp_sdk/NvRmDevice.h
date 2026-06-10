#pragma once

#include "NvRmApi.h"
#ifdef __HAIKU__
#include <OS.h>
#endif


class NvRmMemoryMapping {
private:
	NvRmApi *fRm;
	NvU32 fHDevice;
	NvU32 fHMemory;
	void *fPLinearAddress;
	NvU32 fFlags;
	void *fAddress;
	NvU64 fSize;
#ifdef __HAIKU__
	area_id fArea;
#endif

	void Delete();

public:
	inline NvRmMemoryMapping();
	inline NvRmMemoryMapping(NvRmApi &rm, NvU32 hDevice, NvU32 hMemory, void *pLinearAddress, NvU32 flags, void *address, NvU64 size
#ifdef __HAIKU__
		, area_id area
#endif
	);
	inline NvRmMemoryMapping(NvRmMemoryMapping &&other);
	inline ~NvRmMemoryMapping();

	inline NvRmMemoryMapping(const NvRmMemoryMapping &other) = delete;
	inline NvRmMemoryMapping &operator=(const NvRmMemoryMapping &other) = delete;

	inline NvRmMemoryMapping &operator=(NvRmMemoryMapping &&other);

	inline bool IsSet() const;
	inline void *Address() const;
#ifdef __HAIKU__
	inline area_id Area() const {return fArea;}
#endif
};


class NvRmDevice {
private:
	NvRmObject fDevice;
	NvRmObject fSubdevice;

public:
	NvRmDevice(NvRmApi &rm, NvU32 deviceId);

	const NvRmObject &Device() const {return fDevice;}
	const NvRmObject &Subdevice() const {return fSubdevice;}

	NvU32 DeviceId() const;
	NvU32 DevFsIndex() const;
	NvU32 GpuId() const;

	NvRmMemoryMapping MapMemory(NvHandle hMemory, bool isSystemMemory, NvU64 offset, NvU64 length, NvU32 flags);
	FileDesc ExportObjectToFd(NvHandle hParent, NvHandle hObject);
	NvRmObject ImportObjectFromFd(NvHandle hParent, int fd);
};


NvRmMemoryMapping::NvRmMemoryMapping():
	fRm(nullptr),
	fHDevice(0),
	fHMemory(0),
	fPLinearAddress(nullptr),
	fFlags(0),
	fAddress(nullptr),
	fSize(0)
#ifdef __HAIKU__
	, fArea(-1)
#endif
{
}

NvRmMemoryMapping::NvRmMemoryMapping(NvRmApi &rm, NvU32 hDevice, NvU32 hMemory, void *pLinearAddress, NvU32 flags, void *address, NvU64 size
#ifdef __HAIKU__
	, area_id area
#endif
):
	fRm(&rm),
	fHDevice(hDevice),
	fHMemory(hMemory),
	fPLinearAddress(pLinearAddress),
	fFlags(flags),
	fAddress(address),
	fSize(size)
#ifdef __HAIKU__
	, fArea(area)
#endif
{
}

NvRmMemoryMapping::NvRmMemoryMapping(NvRmMemoryMapping &&other):
	fRm(other.fRm),
	fHDevice(other.fHDevice),
	fHMemory(other.fHMemory),
	fPLinearAddress(other.fPLinearAddress),
	fFlags(other.fFlags),
	fAddress(other.fAddress),
	fSize(other.fSize)
#ifdef __HAIKU__
	, fArea(other.fArea)
#endif
{
	other.fAddress = nullptr;
}

NvRmMemoryMapping::~NvRmMemoryMapping()
{
	if (IsSet()) {
		Delete();
	}
}

NvRmMemoryMapping &NvRmMemoryMapping::operator=(NvRmMemoryMapping &&other)
{
	if (IsSet()) {
		Delete();
	}
	fRm = other.fRm;
	fHDevice = other.fHDevice;
	fHMemory = other.fHMemory;
	fPLinearAddress = other.fPLinearAddress;
	fFlags = other.fFlags;
	fAddress = other.fAddress;
	fSize = other.fSize;

	other.fAddress = nullptr;
	return *this;
}

bool NvRmMemoryMapping::IsSet() const
{
	return fAddress != nullptr;
}

void *NvRmMemoryMapping::Address() const
{
	return fAddress;
}
