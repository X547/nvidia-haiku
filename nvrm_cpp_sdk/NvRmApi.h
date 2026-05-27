#pragma once

#include <utility>

extern "C" {
#include "nvtypes.h"
#include "nv-ioctl.h"
}

#include "FileDesc.h"


class NvRmApi;

class NvRmObject {
private:
	NvRmApi *fRm;
	NvHandle fHObject;

public:
	inline NvRmObject();
	inline NvRmObject(NvRmApi &rm, NvHandle hObject);
	inline NvRmObject(NvRmObject &&other);
	inline ~NvRmObject();

	inline NvRmObject &operator=(NvRmObject &&other);

	NvRmObject(const NvRmObject &other) = delete;
	NvRmObject &operator=(const NvRmObject &other) = delete;

	inline NvRmApi *Rm() const {return fRm;}
	inline NvHandle Get() const;
	inline NvHandle Detach();

	inline NvRmObject Alloc(NvV32 hClass, void *params, NvU32 paramsSize = 0) const;
	inline void Control(NvV32 cmd, void *params, NvU32 paramsSize, NvU32 flags = 0) const;
};


int NvRmIoctl(int fd, NvU32 cmd, void *pParams, NvU32 paramsSize);


class NvRmApi {
private:
	FileDesc fFd;
	NvRmObject fClient;

public:
	NvRmApi();

	NvRmApi(const NvRmApi &other) = delete;
	NvRmApi &operator=(const NvRmApi &other) = delete;

	const NvRmObject &Client() const {return fClient;}

	NvHandle Alloc(NvV32 hClass, NvHandle hParent, void *params, NvU32 paramsSize = 0);

	// Wrap an existing page-aligned userland buffer [address, address+size) as
	// an OS-descriptor memory object via the VID_HEAP_CONTROL ioctl
	// (NVOS32_FUNCTION_ALLOC_OS_DESCRIPTOR). The kernel locks the backing user
	// pages for the lifetime of the returned handle. `address` must be
	// page-aligned; the memory is treated as write-back cacheable,
	// non-contiguous system memory. `writable` controls whether the GPU may
	// write the pages. RM generates and returns the memory handle.
	NvHandle AllocOsDescriptor(NvHandle hParent, void *address, NvU64 size, bool writable);

	void Free(NvHandle hObject);
	void Control(NvHandle hObject, NvV32 cmd, void *params, NvU32 paramsSize, NvU32 flags = 0);
	void MapMemory(NvU32 hDevice, NvU32 hMemory, NvU64 offset, NvU64 length, void *&pLinearAddress, NvU32 flags, int memFd);
	void UnmapMemory(NvU32 hDevice, NvU32 hMemory, void *pLinearAddress, NvU32 flags);
	void MapMemoryDma(NvU32 hDevice, NvU32 hDma, NvU32 hMemory, NvU64 offset, NvU64 length, NvU32 flags, NvU64 &dmaOffset);
	void UnmapMemoryDma(NvU32 hDevice, NvU32 hDma, NvU32 hMemory, NvU32 flags, NvU64 dmaOffset);

	void AllocOsEvent(int fd);
	void FreeOsEvent(int fd);

	void CardInfo(nv_ioctl_card_info_t *infos, NvU32 count);
};


NvRmObject::NvRmObject():
	fRm(nullptr),
	fHObject(0)
{
}

NvRmObject::NvRmObject(NvRmApi &rm, NvHandle hObject):
	fRm(&rm),
	fHObject(hObject)
{
}

NvRmObject::NvRmObject(NvRmObject &&other):
	fRm(other.fRm)
{
	fHObject = other.fHObject;
	other.fHObject = 0;
}

NvRmObject::~NvRmObject()
{
	if (fHObject != 0) {
		fRm->Free(fHObject);
	}
}

NvRmObject &NvRmObject::operator=(NvRmObject &&other)
{
	if (fHObject != 0) {
		fRm->Free(fHObject);
	}
	fRm = other.fRm;
	fHObject = other.fHObject;
	other.fHObject = 0;
	return *this;
}

NvHandle NvRmObject::Get() const
{
	return fHObject;
}

NvHandle NvRmObject::Detach()
{
	NvHandle res = fHObject;
	fHObject = 0;
	return res;
}

NvRmObject NvRmObject::Alloc(NvV32 hClass, void *params, NvU32 paramsSize) const
{
	return NvRmObject(*fRm, fRm->Alloc(hClass, fHObject, params, paramsSize));
}

void NvRmObject::Control(NvV32 cmd, void *params, NvU32 paramsSize, NvU32 flags) const
{
	fRm->Control(fHObject, cmd, params, paramsSize, flags);
}
