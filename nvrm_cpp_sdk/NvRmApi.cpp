#include "NvRmApi.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <utility>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

extern "C" {
#include "nv.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "nv-ioctl.h"
#include "nvstatus.h"
#include "nvos.h"

#include "ctrl/ctrl0000/ctrl0000unix.h"
}
#ifdef __HAIKU__
#include "nv-haiku.h"
#endif

#include "ErrorUtils.h"


int NvRmIoctl(int fd, NvU32 cmd, void *pParams, NvU32 paramsSize)
{
	int res;
	do {
#ifdef __HAIKU__
		res = ioctl(fd, cmd + NV_HAIKU_BASE, pParams, paramsSize);
#else
		res = ioctl(fd, _IOC(IOC_INOUT, NV_IOCTL_MAGIC, cmd, paramsSize), pParams);
#endif
	} while (res < 0 && (errno == EINTR || errno == EAGAIN));
	return res;
}


NvRmApi::NvRmApi()
{
	fFd = FileDesc(open("/dev/" NVIDIA_CONTROL_DEVICE_NAME, O_RDWR | O_CLOEXEC));
	assert(fFd.IsSet());
	fClient = NvRmObject(*this, Alloc(NV01_ROOT_CLIENT, 0, NULL));
}

NvHandle NvRmApi::Alloc(NvV32 hClass, NvHandle hParent, void *params, NvU32 paramsSize)
{
	NVOS21_PARAMETERS p {
		.hRoot = fClient.Get(),
		.hObjectParent = hParent,
		.hClass = hClass,
		.pAllocParms = params,
		.paramsSize = paramsSize
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_ALLOC, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
	return p.hObjectNew;
}

NvHandle NvRmApi::AllocOsDescriptor(NvHandle hParent, void *address, NvU64 size, bool writable)
{
	NVOS32_PARAMETERS p {
		.hRoot = fClient.Get(),
		.hObjectParent = hParent,
		.function = NVOS32_FUNCTION_ALLOC_OS_DESCRIPTOR,
	};
	p.data.AllocOsDesc.type = NVOS32_TYPE_IMAGE;
	// MEMORY_HANDLE_PROVIDED is intentionally not set, so RM generates the
	// handle and returns it in hMemory below.
	p.data.AllocOsDesc.flags = NVOS32_ALLOC_FLAGS_MAP_NOT_REQUIRED;
	// Anonymous user memory is write-back cacheable and cannot be guaranteed
	// contiguous, so the OS-descriptor path requires exactly these attrs.
	p.data.AllocOsDesc.attr =
		DRF_DEF(OS32, _ATTR, _LOCATION, _PCI) |
		DRF_DEF(OS32, _ATTR, _COHERENCY, _WRITE_BACK) |
		DRF_DEF(OS32, _ATTR, _PHYSICALITY, _NONCONTIGUOUS) |
		DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB);
	p.data.AllocOsDesc.attr2 =
		DRF_DEF(OS32, _ATTR2, _GPU_CACHEABLE, _NO) |
		(writable
			? DRF_DEF(OS32, _ATTR2, _PROTECTION_USER, _READ_WRITE)
			: DRF_DEF(OS32, _ATTR2, _PROTECTION_USER, _READ_ONLY));
	p.data.AllocOsDesc.descriptor = (NvP64)(NvUPtr)address;
	p.data.AllocOsDesc.limit = size - 1;
	p.data.AllocOsDesc.descriptorType = NVOS32_DESCRIPTOR_TYPE_VIRTUAL_ADDRESS;

	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_VID_HEAP_CONTROL, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
	return p.data.AllocOsDesc.hMemory;
}

void NvRmApi::Free(NvHandle hObject)
{
	NVOS00_PARAMETERS p {
		.hRoot = fClient.Get(),
		.hObjectOld = hObject
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_FREE, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
}

void NvRmApi::Control(NvHandle hObject, NvV32 cmd, void *params, NvU32 paramsSize, NvU32 flags)
{
	NVOS54_PARAMETERS p {
		.hClient = fClient.Get(),
		.hObject = hObject,
		.cmd = cmd,
		.flags = flags,
		.params = params,
		.paramsSize = paramsSize
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_CONTROL, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
}

void NvRmApi::MapMemory(NvU32 hDevice, NvU32 hMemory, NvU64 offset, NvU64 length, void *&pLinearAddress, NvU32 flags, int memFd)
{
	nv_ioctl_nvos33_parameters_with_fd p {
		.params = {
			.hClient = fClient.Get(),
			.hDevice = hDevice,
			.hMemory = hMemory,
			.offset = offset,
			.length = length,
			.pLinearAddress = pLinearAddress,
			.flags = flags
		},
		.fd = memFd
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_MAP_MEMORY, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.params.status);
	pLinearAddress = p.params.pLinearAddress;
}

void NvRmApi::UnmapMemory(NvU32 hDevice, NvU32 hMemory, void *pLinearAddress, NvU32 flags)
{
	NVOS34_PARAMETERS p {
		.hClient = fClient.Get(),
		.hDevice = hDevice,
		.hMemory = hMemory,
		.pLinearAddress = pLinearAddress,
		.status = 0,
		.flags = flags,
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_UNMAP_MEMORY, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
}

void NvRmApi::MapMemoryDma(NvU32 hDevice, NvU32 hDma, NvU32 hMemory, NvU64 offset, NvU64 length, NvU32 flags, NvU64 &dmaOffset)
{
	NVOS46_PARAMETERS p {
		.hClient = fClient.Get(),
		.hDevice = hDevice,
		.hDma = hDma,
		.hMemory = hMemory,
		.offset = offset,
		.length = length,
		.flags = (NvV32)flags,
		.dmaOffset = dmaOffset,
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_MAP_MEMORY_DMA, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
	dmaOffset = p.dmaOffset;
}

void NvRmApi::UnmapMemoryDma(NvU32 hDevice, NvU32 hDma, NvU32 hMemory, NvU32 flags, NvU64 dmaOffset)
{
	NVOS47_PARAMETERS p {
		.hClient = fClient.Get(),
		.hDevice = hDevice,
		.hDma = hDma,
		.hMemory = hMemory,
		.flags = (NvV32)flags,
		.dmaOffset = dmaOffset,
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_RM_UNMAP_MEMORY_DMA, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.status);
}

void NvRmApi::AllocOsEvent(int fd)
{
	nv_ioctl_alloc_os_event_t p {
		.hClient = fClient.Get(),
		.fd = (NvU32)fd
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_ALLOC_OS_EVENT, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.Status);
}

void NvRmApi::FreeOsEvent(int fd)
{
	nv_ioctl_free_os_event_t p {
		.hClient = fClient.Get(),
		.fd = (NvU32)fd
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_FREE_OS_EVENT, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.Status);
}

void NvRmApi::CardInfo(nv_ioctl_card_info_t *infos, NvU32 count)
{
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_CARD_INFO, infos, sizeof(nv_ioctl_card_info_t)*count);
	CheckErrno(ret);
}
