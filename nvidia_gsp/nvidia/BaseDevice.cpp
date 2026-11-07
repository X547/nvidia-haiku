#include "BaseDevice.h"
#include "Device.h"
#include "Driver.h"

#include <new>

#include <sys/ioccom.h>

#include <KernelExport.h>
#include <device/graphic_driver.h>

#include <StackOrHeapArray.h>
#include <util/AutoLock.h>
#include <team.h>
#include <vm/vm.h>
//#include <fs/fd.h>
#include <fs/select_sync_pool.h>

extern status_t user_fd_kernel_ioctl(int fd, unsigned op, void *buffer,
	size_t length);

#include <kernel.h>

#include <AutoDeleter.h>
#include <AutoDeleterOS.h>

#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


NvHaikuBaseDevice::NvHaikuBaseDevice()
{
	dprintf("+NvHaikuBaseDevice()\n");

	fNvState.os_state = this;
}

NvHaikuBaseDevice::~NvHaikuBaseDevice()
{
	dprintf("-NvHaikuBaseDevice()\n");
}


NvHaikuBaseDeviceHandle *NvHaikuBaseDeviceHandle::FromFd(int fd)
{
	// TODO: check that FD is Nvidia device

	NvHaikuBaseDeviceHandle *handle {};
	if (user_fd_kernel_ioctl(fd, NV_HAIKU_BASE + NV_HAIKU_GET_COOKIE, &handle, sizeof(handle)) < 0) {
		return nullptr;
	}
	return handle;
}

NvHaikuBaseDeviceHandle::NvHaikuBaseDeviceHandle(NvHaikuBaseDevice *device):
	fDevice(device)
{
	//dprintf("+NvHaikuBaseDeviceHandle\n");

	fNvfp.ctl_nvfp_priv = this;
}

NvHaikuBaseDeviceHandle::~NvHaikuBaseDeviceHandle()
{
	//dprintf("-NvHaikuBaseDeviceHandle\n");

	rm_cleanup_file_private(nullptr, &fDevice->fNvState, &fNvfp);

    if (fMmapContext.valid)
    {
        if (fMmapContext.page_array != nullptr)
        {
            free(fMmapContext.page_array);
        }
        if (fMmapContext.memArea.pRanges != nullptr)
        {
            free(fMmapContext.memArea.pRanges);
        }
    }
}

status_t NvHaikuBaseDeviceHandle::Close()
{
	//dprintf("NvHaikuBaseDeviceHandle::Close()\n");

	fClosed.store(true);

	return B_OK;
}

status_t NvHaikuBaseDeviceHandle::Control(uint32 op, void *data, size_t len)
{
	if (fClosed.load())
		return EINVAL;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE: {
			if (NV_IS_CTL_DEVICE(fDevice->Nv())) {
				return EINVAL;
			}

			CHECK_RET(user_strlcpy((char*)data, "nvidia_gsp.accelerant", len));
			return B_OK;
		}
		default:
			break;
	}

	if (op < NV_HAIKU_BASE)
		return B_DEV_INVALID_IOCTL;

	uint32 cmd = op - NV_HAIKU_BASE;
	//dprintf("NvHaikuBaseDeviceHandle::Control(cmd: %" B_PRIu32 ", len: %" B_PRIu32 ")\n", cmd, len);

	switch (cmd) {
		case NV_HAIKU_GET_COOKIE: {
			if (!IS_KERNEL_ADDRESS(data)) {
				dprintf("[!] !IS_KERNEL_ADDRESS(data)\n");
				return B_DEV_INVALID_IOCTL;
			}
			if (len != sizeof(void*)) {
				dprintf("[!] len != sizeof(void*)\n");
				return EINVAL;
			}
			*(void**)data = this;
			return B_OK;
		}
		case NV_HAIKU_MAP: {
			nv_haiku_map_params params;
			if (len != sizeof(params)) {
				dprintf("[!] len != sizeof(params): %" B_PRIu32 ", %" B_PRIuSIZE "\n", len, sizeof(params));
				return EINVAL;
			}
			if (!IS_USER_ADDRESS(data)) {
				dprintf("[!] !IS_USER_ADDRESS(data)\n");
				return B_DEV_INVALID_IOCTL;
			}
			CHECK_RET(user_memcpy(&params, data, sizeof(params)));

			if (strnlen(params.name, B_OS_NAME_LENGTH) == B_OS_NAME_LENGTH) {
				dprintf("[!] strnlen(params.name, B_OS_NAME_LENGTH) == B_OS_NAME_LENGTH\n");
				return EINVAL;
			}

			if (!fMmapContext.valid) {
				dprintf("[!] !fMmapContext.valid\n");
				return EINVAL;
			}

			AreaDeleter area;
			void *baseAddress {};
			if (NV_IS_CTL_DEVICE(fDevice->Nv())) {
				auto alloc = static_cast<nv_alloc_t*>(fMmapContext.alloc);
				area_id srcArea = alloc->area.Get();
				area.SetTo(vm_clone_area(
					team_get_current_team_id(),
					params.name,
					&baseAddress,
					params.addressSpec,
					params.protection & (B_USER_PROTECTION | B_CLONEABLE_AREA),
					REGION_NO_PRIVATE_MAP,
					srcArea,
					true
				));
				CHECK_RET(area.Get());
				params.address = (uint8*)baseAddress + fMmapContext.page_index * B_PAGE_SIZE;
				uint32 haikuCacheType = to_haiku_cache_type(alloc->cacheType);
				vm_set_area_memory_type(area.Get(), alloc->physBase, haikuCacheType);
			} else {
				if (fMmapContext.memArea.numRanges == 0 || fMmapContext.memArea.numRanges > (uint64)UINT32_MAX) {
					dprintf("[!] fMmapContext.memArea is invalid, numRanges: %" B_PRIu64 "\n", fMmapContext.memArea.numRanges);
					return EINVAL;
				}
				if (fMmapContext.memArea.numRanges == 1) {
					area.SetTo(vm_map_physical_memory(
						team_get_current_team_id(),
						params.name,
						&baseAddress,
						params.addressSpec,
						fMmapContext.memArea.pRanges[0].size,
						params.protection & (B_USER_PROTECTION | B_CLONEABLE_AREA),
						fMmapContext.memArea.pRanges[0].start,
						false
					));
				} else {
					area.SetTo(vm_map_physical_memory_vecs(
						team_get_current_team_id(),
						params.name,
						&baseAddress,
						params.addressSpec,
						nullptr,
						params.protection & (B_USER_PROTECTION | B_CLONEABLE_AREA),
						(generic_io_vec*)fMmapContext.memArea.pRanges,
						(uint32)fMmapContext.memArea.numRanges
					));
				}
				CHECK_RET(area.Get());
				params.address = baseAddress;
				phys_addr_t basePhysAddr = fMmapContext.memArea.pRanges[0].start;
				uint32 haikuCacheType = to_haiku_cache_type(fMmapContext.caching);
				vm_set_area_memory_type(area.Get(), basePhysAddr, haikuCacheType);
			}


			CHECK_RET(user_memcpy(data, &params, sizeof(params)));

			return area.Detach();
		}

		case NV_ESC_CHECK_VERSION_STR: {
			// Handled by the OS layer on Linux (nvidia_ioctl), not by
			// rm_ioctl(), so it must be answered here. NVK's RM backend sends
			// it at startup to check that it was built for this RM release.
			if (!NV_IS_CTL_DEVICE(fDevice->Nv()))
				return EINVAL;
			nv_ioctl_rm_api_version_t params;
			if (len != sizeof(params))
				return EINVAL;
			if (!IS_USER_ADDRESS(data))
				return B_BAD_ADDRESS;
			CHECK_RET(user_memcpy(&params, data, sizeof(params)));
			NV_STATUS status = rm_perform_version_check(nullptr, &params, sizeof(params));
			// Like Linux, copy the reply back even on a mismatch: it carries
			// RM's version string for the client's error message.
			CHECK_RET(user_memcpy(data, &params, sizeof(params)));
			return status == NV_OK ? B_OK : EINVAL;
		}

		case NV_ESC_CARD_INFO: {
			if (!NV_IS_CTL_DEVICE(fDevice->Nv())) {
				return EINVAL;
			}

			size_t numArgDevices = len / sizeof(nv_ioctl_card_info_t);
			uint32 numDevices = NvHaikuDriver::Instance().DeviceCount();

			CHECK_RET(user_memset(data, 0, len));

			if (numArgDevices < numDevices) {
				return EINVAL;
			}

			for (uint32 i = 0; i < numDevices; i++) {
				auto dev = NvHaikuDriver::Instance().DeviceAt(i);
				nv_state_t *nv = dev->Nv();

		        if ((nv->flags & NV_FLAG_EXCLUDE) != 0)
		            continue;

				nv_ioctl_card_info_t ci {};
		        ci.valid              = NV_TRUE;
		        ci.pci_info.domain    = nv->pci_info.domain;
		        ci.pci_info.bus       = nv->pci_info.bus;
		        ci.pci_info.slot      = nv->pci_info.slot;
		        ci.pci_info.vendor_id = nv->pci_info.vendor_id;
		        ci.pci_info.device_id = nv->pci_info.device_id;
		        ci.gpu_id             = nv->gpu_id;
		        ci.interrupt_line     = nv->interrupt_line;
		        ci.reg_address        = nv->regs->cpu_address;
		        ci.reg_size           = nv->regs->size;
		        ci.minor_number       = dev->DeviceIndex();
	            ci.fb_address         = nv->fb->cpu_address;
	            ci.fb_size            = nv->fb->size;

	            CHECK_RET(user_memcpy(static_cast<nv_ioctl_card_info_t*>(data) + i, &ci, sizeof(nv_ioctl_card_info_t)));
			}

			return B_OK;
		}
	}

	BStackOrHeapArray<uint8, 32> kernelData(len);
	if (!kernelData.IsValid()) {
		return B_NO_MEMORY;
	}
	CHECK_RET(user_memcpy(&kernelData[0], data, len));

	if (rm_ioctl(nullptr, &fDevice->fNvState, &fNvfp, cmd, &kernelData[0], len) != NV_OK)
		return EINVAL;

	CHECK_RET(user_memcpy(data, &kernelData[0], len));

	return B_OK;
}

void NvHaikuBaseDeviceHandle::NotifySelectPool()
{
	MutexLocker _(&fSelectPoolMutex);
	notify_select_event_pool(fSelectPool, B_SELECT_READ);
}

status_t NvHaikuBaseDeviceHandle::Select(uint8 event, uint32 ref, selectsync *sync)
{
	//dprintf("NvHaikuBaseDeviceHandle::Select()\n");

	MutexLocker _(&fSelectPoolMutex);
	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	CHECK_RET(add_select_sync_pool_entry(&fSelectPool, sync, event));

	if (fDatalessEventPending.exchange(false)) {
		notify_select_event(sync, event);
	}

	return B_OK;
}

status_t NvHaikuBaseDeviceHandle::Deselect(uint8 event, selectsync *sync)
{
	//dprintf("NvHaikuBaseDeviceHandle::Deselect()\n");

	MutexLocker _(&fSelectPoolMutex);

	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	CHECK_RET(remove_select_sync_pool_entry(&fSelectPool, sync, event));

	return B_OK;
}
