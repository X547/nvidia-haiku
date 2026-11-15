#include "NvGpuMemory.h"

#include <utility>

extern "C" {
#include "nvos.h"
#include "class/cl003e.h" // NV01_MEMORY_SYSTEM
#include "class/cl0040.h" // NV01_MEMORY_LOCAL_USER
#include "class/cl50a0.h" // NV50_MEMORY_VIRTUAL
}

#include <ErrorUtils.h>


NvGpuMemory::NvGpuMemory(NvGpuMemory &&other):
	fDev(other.fDev),
	fPhysHandle(other.fPhysHandle),
	fVirtHandle(other.fVirtHandle),
	fGpuAddr(other.fGpuAddr),
	fSize(other.fSize),
	fCpuMap(std::move(other.fCpuMap))
{
	other.fDev = nullptr;
	other.fPhysHandle = 0;
	other.fVirtHandle = 0;
	other.fGpuAddr = 0;
	other.fSize = 0;
}

NvGpuMemory &NvGpuMemory::operator=(NvGpuMemory &&other)
{
	Reset();
	fDev = other.fDev;
	fPhysHandle = other.fPhysHandle;
	fVirtHandle = other.fVirtHandle;
	fGpuAddr = other.fGpuAddr;
	fSize = other.fSize;
	fCpuMap = std::move(other.fCpuMap);
	other.fDev = nullptr;
	other.fPhysHandle = 0;
	other.fVirtHandle = 0;
	other.fGpuAddr = 0;
	other.fSize = 0;
	return *this;
}

NvGpuMemory::~NvGpuMemory()
{
	Reset();
}

void NvGpuMemory::Reset()
{
	if (fDev == nullptr) {
		return;
	}
	NvRmApi *rm = fDev->Device().Rm();

	// CPU mapping must be torn down first; UnmapMemory still needs the
	// physical handle to be live.
	fCpuMap = NvRmMemoryMapping();

	if (fGpuAddr != 0) {
		rm->UnmapMemoryDma(fDev->Device().Get(), fVirtHandle, fPhysHandle, 0, fGpuAddr);
		fGpuAddr = 0;
	}
	if (fVirtHandle != 0) {
		rm->Free(fVirtHandle);
		fVirtHandle = 0;
	}
	if (fPhysHandle != 0) {
		rm->Free(fPhysHandle);
		fPhysHandle = 0;
	}
	fSize = 0;
	fDev = nullptr;
}

void NvGpuMemory::AllocSystem(NvRmDevice &dev, NvHandle hVaSpace, NvU64 size, NvU64 align, bool mapCpu)
{
	Reset();
	NvRmApi &rm = *dev.Device().Rm();

	NV_MEMORY_ALLOCATION_PARAMS physParams {
		.owner = rm.Client().Get(),
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.attr =
			DRF_DEF(OS32, _ATTR, _LOCATION, _PCI) |
			DRF_DEF(OS32, _ATTR, _COHERENCY, _CACHED) |
			DRF_DEF(OS32, _ATTR, _PHYSICALITY, _NONCONTIGUOUS) |
			DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB),
		.size = size,
		.alignment = align,
	};
	NvHandle hPhys = rm.Alloc(NV01_MEMORY_SYSTEM, dev.Device().Get(), &physParams, sizeof(physParams));

	NV_MEMORY_ALLOCATION_PARAMS virtParams {
		.owner = rm.Client().Get(),
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB),
		.size = size,
		.alignment = align,
		.hVASpace = hVaSpace,
	};
	NvHandle hVirt;
	try {
		hVirt = rm.Alloc(NV50_MEMORY_VIRTUAL, dev.Device().Get(), &virtParams, sizeof(virtParams));
	} catch (...) {
		rm.Free(hPhys);
		throw;
	}

	NvU64 gpuAddr = 0;
	try {
		NvU32 mapFlags = FLD_SET_DRF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE, 0U);
		rm.MapMemoryDma(dev.Device().Get(), hVirt, hPhys, 0, size, mapFlags, gpuAddr);
	} catch (...) {
		rm.Free(hVirt);
		rm.Free(hPhys);
		throw;
	}

	NvRmMemoryMapping cpuMap;
	if (mapCpu) {
		try {
			cpuMap = dev.MapMemory(hPhys, true, 0, size, 0);
		} catch (...) {
			rm.UnmapMemoryDma(dev.Device().Get(), hVirt, hPhys, 0, gpuAddr);
			rm.Free(hVirt);
			rm.Free(hPhys);
			throw;
		}
	}

	fDev = &dev;
	fPhysHandle = hPhys;
	fVirtHandle = hVirt;
	fGpuAddr = gpuAddr;
	fSize = size;
	fCpuMap = std::move(cpuMap);
}

void NvGpuMemory::WrapUserSystem(NvRmDevice &dev, NvHandle hVaSpace, void *addr, NvU64 size, NvU64 align, bool writable)
{
	Reset();
	NvRmApi &rm = *dev.Device().Rm();

	// The physical handle is backed by the caller's locked user pages rather
	// than a fresh RM allocation; everything below mirrors AllocSystem.
	NvHandle hPhys = rm.AllocOsDescriptor(dev.Device().Get(), addr, size, writable);

	NV_MEMORY_ALLOCATION_PARAMS virtParams {
		.owner = rm.Client().Get(),
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB),
		.size = size,
		.alignment = align,
		.hVASpace = hVaSpace,
	};
	NvHandle hVirt;
	try {
		hVirt = rm.Alloc(NV50_MEMORY_VIRTUAL, dev.Device().Get(), &virtParams, sizeof(virtParams));
	} catch (...) {
		rm.Free(hPhys);
		throw;
	}

	NvU64 gpuAddr = 0;
	try {
		NvU32 mapFlags = FLD_SET_DRF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE, 0U);
		rm.MapMemoryDma(dev.Device().Get(), hVirt, hPhys, 0, size, mapFlags, gpuAddr);
	} catch (...) {
		rm.Free(hVirt);
		rm.Free(hPhys);
		throw;
	}

	fDev = &dev;
	fPhysHandle = hPhys;
	fVirtHandle = hVirt;
	fGpuAddr = gpuAddr;
	fSize = size;
}

void NvGpuMemory::AllocLocal(NvRmDevice &dev, NvHandle hVaSpace, NvU64 size, NvU64 align, bool mapCpu)
{
	Reset();
	NvRmApi &rm = *dev.Device().Rm();

	// VIDMEM physical allocations are parented by the subdevice (the
	// device owns no FB itself in the multi-subdevice model), and
	// vidmem is naturally uncached from the CPU's perspective.
	NV_MEMORY_ALLOCATION_PARAMS physParams {
		.owner = rm.Client().Get(),
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.attr =
			DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM) |
			DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED) |
			DRF_DEF(OS32, _ATTR, _PHYSICALITY, _CONTIGUOUS) |
			DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _DEFAULT),
		.size = size,
		.alignment = align,
	};
	NvHandle hPhys = rm.Alloc(NV01_MEMORY_LOCAL_USER, dev.Subdevice().Get(), &physParams, sizeof(physParams));

	NV_MEMORY_ALLOCATION_PARAMS virtParams {
		.owner = rm.Client().Get(),
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _DEFAULT),
		.size = size,
		.alignment = align,
		.hVASpace = hVaSpace,
	};
	NvHandle hVirt;
	try {
		hVirt = rm.Alloc(NV50_MEMORY_VIRTUAL, dev.Device().Get(), &virtParams, sizeof(virtParams));
	} catch (...) {
		rm.Free(hPhys);
		throw;
	}

	NvU64 gpuAddr = 0;
	try {
		// CACHE_SNOOP is a no-op for vidmem (the GPU is hitting its own
		// FB) but the flag is silently accepted, so just pass 0 for
		// flags here to avoid signalling a sysmem-only intent.
		rm.MapMemoryDma(dev.Device().Get(), hVirt, hPhys, 0, size, 0, gpuAddr);
	} catch (...) {
		rm.Free(hVirt);
		rm.Free(hPhys);
		throw;
	}

	NvRmMemoryMapping cpuMap;
	if (mapCpu) {
		try {
			cpuMap = dev.MapMemory(hPhys, false, 0, size, 0);
		} catch (...) {
			rm.UnmapMemoryDma(dev.Device().Get(), hVirt, hPhys, 0, gpuAddr);
			rm.Free(hVirt);
			rm.Free(hPhys);
			throw;
		}
	}

	fDev = &dev;
	fPhysHandle = hPhys;
	fVirtHandle = hVirt;
	fGpuAddr = gpuAddr;
	fSize = size;
	fCpuMap = std::move(cpuMap);
}
