#pragma once

#include <NvRmApi.h>
#include <NvRmDevice.h>


// A block of system memory that is RM-allocated, mapped into the GPU's VA
// space, and optionally CPU-mapped. The lifecycle is non-trivial because
// the GPU DMA mapping must be torn down between the CPU mapping and the
// physical/virtual handle frees, so we drive it explicitly in Reset()
// rather than relying on member-destruction order.
class NvGpuMemory {
private:
	NvRmDevice *fDev {};
	NvHandle fPhysHandle {};
	NvHandle fVirtHandle {};
	NvU64 fGpuAddr {};
	NvU64 fSize {};
	NvRmMemoryMapping fCpuMap;

	void Reset();

public:
	NvGpuMemory() = default;
	NvGpuMemory(NvGpuMemory &&other);
	NvGpuMemory &operator=(NvGpuMemory &&other);
	~NvGpuMemory();

	NvGpuMemory(const NvGpuMemory &) = delete;
	NvGpuMemory &operator=(const NvGpuMemory &) = delete;

	// Allocate `size` bytes of cached, coherent system memory, bind it into
	// `hVaSpace` at a fresh GPU virtual address, and (if mapCpu) map it into
	// the calling process for CPU access.
	void AllocSystem(NvRmDevice &dev, NvHandle hVaSpace, NvU64 size, NvU64 align, bool mapCpu);

	// Allocate `size` bytes of GPU-local video memory under the subdevice
	// and bind it into `hVaSpace`. No CPU mapping is produced — vidmem
	// generally is not CPU-accessible on these GPUs, and consumers must
	// drive any access through the channel.
	void AllocLocal(NvRmDevice &dev, NvHandle hVaSpace, NvU64 size, NvU64 align);

	// Wrap an existing page-aligned userland buffer [addr, addr+size) as an
	// OS-descriptor memory object (the kernel locks the backing user pages),
	// and bind it into `hVaSpace` at a fresh GPU virtual address. The caller
	// retains ownership of the CPU memory and keeps using its own pointer for
	// CPU access, so no CPU mapping is produced here; CpuAddr() stays null.
	// `writable` controls whether the GPU may write the pages. Freeing this
	// object (Reset/destruction) unlocks the user pages.
	void WrapUserSystem(NvRmDevice &dev, NvHandle hVaSpace, void *addr, NvU64 size, NvU64 align, bool writable);

	bool IsSet() const {return fSize != 0;}
	NvHandle PhysHandle() const {return fPhysHandle;}
	NvU64 GpuAddr() const {return fGpuAddr;}
	NvU64 Size() const {return fSize;}
	void *CpuAddr() const {return fCpuMap.Address();}
	const NvRmMemoryMapping &CpuMap() const {return fCpuMap;}
};
