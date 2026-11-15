#pragma once

#include <atomic>
#include <OS.h>
#include <AutoDeleterOS.h>
#include <lock.h>

#include "Drivers_cpp.h"
#include "ContainerOf.h"

#include "nv-include.h"
#include "nv-haiku.h"

struct select_sync_pool;


// RM's private data for system memory is either of the two structures below,
// and some paths get either: both start with the kind, to tell them apart.
enum nv_sysmem_kind : uint32 {
	NV_SYSMEM_ALLOC = 'nvAl',
	NV_SYSMEM_USER_PAGES = 'nvUp',
};


// System memory the driver allocated (nv_alloc_pages()).
struct nv_alloc_t {
	nv_sysmem_kind kind = NV_SYSMEM_ALLOC;
	AreaDeleter area;
	NvU32 cacheType {};
	phys_addr_t physBase {};
};


// Userland pages backing an OS-descriptor memory object. They must lie in one
// B_FULL_LOCK area of the calling team, which is cloned into the kernel: the
// clone keeps the pages wired however the team goes away (it deletes its
// areas before closing its file descriptors), so the GPU never reaches pages
// that were freed. lock_memory_etc() is not used, since deleting an area with
// wired ranges at team exit can hang or panic. Created by os_lock_user_pages(),
// passed through nv_register_user_pages()/nv_unregister_user_pages()
// unchanged, and released by os_unlock_user_pages().
struct nv_user_pages_t {
	nv_sysmem_kind kind = NV_SYSMEM_USER_PAGES;
	AreaDeleter clone;
	addr_t kernelBase {};	// the first page, in the clone
	NvU64 pageCount {};
};


// The private data of system memory as one or the other, or nullptr.
static inline nv_alloc_t *nv_sysmem_as_alloc(void *priv)
{
	auto alloc = static_cast<nv_alloc_t*>(priv);
	return alloc != nullptr && alloc->kind == NV_SYSMEM_ALLOC ? alloc : nullptr;
}

static inline nv_user_pages_t *nv_sysmem_as_user_pages(void *priv)
{
	auto pages = static_cast<nv_user_pages_t*>(priv);
	return pages != nullptr && pages->kind == NV_SYSMEM_USER_PAGES ? pages : nullptr;
}


const char *nv_cache_type_str(NvU32 cache_type);
uint32 to_haiku_cache_type(NvU32 cache_type);


class NvHaikuBaseDevice {
protected:
	friend class NvHaikuBaseDeviceHandle;
	nv_state_t fNvState {};

public:
	NvHaikuBaseDevice();
	~NvHaikuBaseDevice();

	static inline NvHaikuBaseDevice *FromNv(nv_state_t *nv) {return static_cast<NvHaikuBaseDevice*>(nv->os_state);}
	inline nv_state_t *Nv() {return &fNvState;}
};


class NvHaikuBaseDeviceHandle {
private:
	NvHaikuBaseDevice *fDevice {};

protected:
	nv_file_private_t fNvfp {};
	std::atomic<bool> fClosed = false;

	mutex fSelectPoolMutex = MUTEX_INITIALIZER("NvHaikuSelectPool");
	select_sync_pool* fSelectPool {};
	std::atomic<bool> fDatalessEventPending {};

	nv_alloc_mapping_context_t fMmapContext {};

public:
	NvHaikuBaseDeviceHandle(NvHaikuBaseDevice *device);
	~NvHaikuBaseDeviceHandle();

	static NvHaikuBaseDeviceHandle *FromFd(int fd);
	static inline NvHaikuBaseDeviceHandle *FromNvfp(nv_file_private_t *nvfp) {return &ContainerOf(*nvfp, &NvHaikuBaseDeviceHandle::fNvfp);}
	inline nv_file_private_t &Nvfp() {return fNvfp;}
	inline nv_alloc_mapping_context_t &MmapContext() {return fMmapContext;}
	NvHaikuBaseDevice *GetBaseDevice() const {return fDevice;}

	inline void SetDatalessEventPending();
	void NotifySelectPool();

	status_t Close();
	status_t Control(uint32 op, void *data, size_t len);
	status_t Select(uint8 event, uint32 ref, selectsync *sync);
	status_t Deselect(uint8 event, selectsync *sync);
};


void NvHaikuBaseDeviceHandle::SetDatalessEventPending()
{
	fDatalessEventPending = true;
}
