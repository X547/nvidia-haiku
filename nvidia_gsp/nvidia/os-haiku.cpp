#include "nv-include.h"
extern "C" {
#include <os-interface.h>
#include "nvlink/interface/nvlink_os.h"
#include <nvrm_registry.h>
}
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <AutoDeleter.h>
#include <AutoDeleterOS.h>
#include <util/AutoLock.h>

#include <OS.h>
#include <FindDirectory.h>
#include <KernelExport.h>
#include <kernel.h>
#include <lock.h>
#include <interrupts.h>
#include <vm/vm.h>
#include <condition_variable.h>

#include <PCI.h>

#include <heap.h>
#include <private/kernel/boot_item.h>
#include <private/kernel/frame_buffer_console.h>

#include "Driver.h"
#include "ControlDevice.h"
#include "IntrSafePool.h"

#if 0
#define TRACE(...) dprintf(__VA_ARGS__)
#else
#define TRACE(...)
#endif


struct os_wait_queue {ConditionVariable cv;};
typedef spinlock os_spinlock_t;
typedef mutex os_mutex_t;
typedef rw_lock os_rwlock_t;
struct os_semaphore_t {SemDeleter sem;};


NvU32 os_page_size  = B_PAGE_SIZE;
NvU64 os_page_mask  = ~((uint64)(B_PAGE_SIZE - 1));
NvU8  os_page_shift = 12; // !!!
NvU32 os_sev_status = 0;
NvBool os_sev_enabled = 0;
NvBool os_cc_sev_snp_enabled = 0;
NvBool os_cc_snp_vtom_enabled = 0;
NvBool os_cc_enabled = 0;
NvBool os_cc_tdx_enabled = 0;

#define MAX_ERROR_STRING 528
static char nv_error_string[MAX_ERROR_STRING];
static spinlock nv_error_string_lock = B_SPINLOCK_INITIALIZER;



void NV_API_CALL out_string(const char *str)
{
	dprintf("out_string()\n");
	dprintf("%s", str);
}

int NV_API_CALL nv_printf(NvU32 debuglevel, const char *printf_format, ...)
{
	TRACE("nv_printf()\n");
	va_list arglist;

	InterruptsSpinLocker lock(&nv_error_string_lock);

	va_start(arglist, printf_format);
	vsnprintf(nv_error_string, MAX_ERROR_STRING, printf_format, arglist);
	nv_error_string[MAX_ERROR_STRING - 1] = 0;
	dprintf(nv_error_string);
	va_end(arglist);

	return 0;
}

NvS32 NV_API_CALL os_snprintf(char *buf, NvU32 size, const char *fmt, ...)
{
	TRACE("os_snprintf()\n");
	va_list arglist;
	int chars_written;

	va_start(arglist, fmt);
	chars_written = vsnprintf(buf, size, fmt, arglist);
	va_end(arglist);

	return chars_written;
}

void
nvlink_print
(
    const char *file,
    int         line,
    const char *function,
    int         log_level,
    const char *fmt,
    ...
)
{
    va_list arglist;
    char    nv_string[1024];
    const char *sys_log_level;

    switch (log_level) {
    case NVLINK_DBG_LEVEL_INFO:
        sys_log_level = "[INFO] ";
        break;
    case NVLINK_DBG_LEVEL_SETUP:
        sys_log_level = "[DEBUG] ";
        break;
    case NVLINK_DBG_LEVEL_USERERRORS:
        sys_log_level = "[NOTICE] ";
        break;
    case NVLINK_DBG_LEVEL_WARNINGS:
        sys_log_level = "[WARNING] ";
        break;
    case NVLINK_DBG_LEVEL_ERRORS:
        sys_log_level = "[ERROR] ";
        break;
    default:
        sys_log_level = "[INFO] ";
        break;
    }

    va_start(arglist, fmt);
    vsnprintf(nv_string, sizeof(nv_string), fmt, arglist);
    va_end(arglist);

    nv_string[sizeof(nv_string) - 1] = '\0';
    dprintf("%snvidia-nvlink: %s", sys_log_level, nv_string);
}

void NV_API_CALL os_bug_check(NvU32 bugCode, const char *bugCodeStr)
{
    dprintf("[!] panic: %s\n", bugCodeStr);
    abort();
}

void NV_API_CALL os_dbg_breakpoint(void)
{
    dprintf("[!] os_dbg_breakpoint\n");
    //abort();
}

void NV_API_CALL os_dbg_init(void)
{
    TRACE("os_dbg_init()\n");
}

void NV_API_CALL os_add_record_for_crashLog(void *pbuffer, NvU32 size)
{
    TRACE("os_add_record_for_crashLog()\n");
}

void NV_API_CALL os_delete_record_for_crashLog(void *pbuffer)
{
    TRACE("os_add_record_for_crashLog()\n");
}

NV_STATUS NV_API_CALL nv_log_error(
    nv_state_t *nv,
    NvU32       error_number,
    const char *format,
    va_list    ap
)
{
	dprintf("nv_log_error(%#" B_PRIx32 ")\n", error_number);
	InterruptsSpinLocker lock(&nv_error_string_lock);

	vsnprintf(nv_error_string, MAX_ERROR_STRING, format, ap);
	nv_error_string[MAX_ERROR_STRING - 1] = 0;
	dprintf("%s\n", nv_error_string);

	return NV_OK;
}


//#pragma mark - memory


NV_STATUS NV_API_CALL os_alloc_mem(
    void **address,
    NvU64 size
) {
	TRACE("os_alloc_mem()\n");

	if (are_interrupts_enabled()) {
		*address = calloc(1, size);
	} else {
		*address = NvHaikuDriver::Instance().IntrSafePool().Alloc(size);
	}

	return ((*address != nullptr) ? NV_OK : NV_ERR_NO_MEMORY);
}

void NV_API_CALL os_free_mem(void *address)
{
	TRACE("os_free_mem()\n");
	if (are_interrupts_enabled()) {
		free(address);
	} else {
		// deferred_free is the kernel's IRQ-safe primitive: it reuses the
		// freed block as a list node and lets a kernel daemon do the real
		// free() once it's safe.
		deferred_free(address);
	}
}

void *NV_API_CALL os_mem_copy(
    void       *dst,
    const void *src,
    NvU32       length
) {
    TRACE("os_mem_copy()\n");
    return memcpy(dst, src, length);
}

void* NV_API_CALL os_mem_set(
    void  *dst,
    NvU8   c,
    NvU32  length
) {
    TRACE("os_mem_set()\n");
    return memset(dst, (int)c, length);
}

NvS32 NV_API_CALL os_mem_cmp(
    const NvU8 *buf0,
    const NvU8* buf1,
    NvU32 length
)
{
    TRACE("os_mem_cmp()\n");
    return memcmp(buf0, buf1, length);
}

// String helpers. RM calls these from ordinary paths (the version check
// behind NV_ESC_CHECK_VERSION_STR uses os_string_copy, registry parsing the
// others), so they must not be panic stubs.
char *NV_API_CALL os_string_copy(char *dst, const char *src)
{
	return strcpy(dst, src);
}

NvU32 NV_API_CALL os_string_length(const char *str)
{
	return (NvU32)strlen(str);
}

NvS32 NV_API_CALL os_string_compare(const char *str1, const char *str2)
{
	return strcmp(str1, str2);
}

// Same contract as strtoul(); written out so the driver doesn't depend on
// the kernel exporting it. Base 0 picks 16 for "0x", 8 for a leading 0.
NvU32 NV_API_CALL os_strtoul(const char *str, char **endp, NvU32 base)
{
	const char *p = str;
	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	if (*p == '+')
		p++;
	if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')
		&& isxdigit((unsigned char)p[2])) {
		p += 2;
		base = 16;
	} else if (base == 0) {
		base = (p[0] == '0') ? 8 : 10;
	}

	NvU32 value = 0;
	const char *start = p;
	for (;; p++) {
		NvU32 digit;
		if (*p >= '0' && *p <= '9')
			digit = *p - '0';
		else if (*p >= 'a' && *p <= 'z')
			digit = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'Z')
			digit = *p - 'A' + 10;
		else
			break;
		if (digit >= base)
			break;
		value = value * base + digit;
	}
	if (endp != nullptr)
		*endp = (char *)(p == start ? str : p);
	return value;
}

NV_STATUS NV_API_CALL os_memcpy_to_user(
    void       *to,
    const void *from,
    NvU32       n
) {
	TRACE("os_memcpy_to_user()\n");
	if (!IS_USER_ADDRESS(to) || !IS_KERNEL_ADDRESS(from))
		return NV_ERR_INVALID_ADDRESS;

	return user_memcpy(to, from, n) < B_OK ? NV_ERR_INVALID_ADDRESS : NV_OK;
}

NV_STATUS NV_API_CALL os_memcpy_from_user(
    void       *to,
    const void *from,
    NvU32       n
) {
	TRACE("os_memcpy_from_user()\n");
	if (!IS_KERNEL_ADDRESS(to) || !IS_USER_ADDRESS(from))
		return NV_ERR_INVALID_ADDRESS;

	return user_memcpy(to, from, n) < B_OK ? NV_ERR_INVALID_ADDRESS : NV_OK;
}


void * nvlink_malloc(NvLength size)
{
   return calloc(1, size);
}

void nvlink_free(void *ptr)
{
    return free(ptr);
}

void *nvlink_memset(void *dest, int value, NvLength size)
{
     return memset(dest, value, size);
}

void *nvlink_memcpy(void *dest, const void *src, NvLength size)
{
    return memcpy(dest, src, size);
}

int nvlink_memcmp(const void *s1, const void *s2, NvLength size)
{
    return memcmp(s1, s2, size);
}

char * nvlink_strcpy(char *dest, const char *src)
{
    return strcpy(dest, src);
}

int nvlink_strcmp(const char *dest, const char *src)
{
    return strcmp(dest, src);
}

NvLength nvlink_strlen(const char *s)
{
    return strlen(s);
}


//#pragma mark - memory mapping

const char *nv_cache_type_str(NvU32 cache_type)
{
	switch (cache_type) {
		case NV_MEMORY_CACHED: return "C";
		case NV_MEMORY_UNCACHED: return "UC";
		case NV_MEMORY_WRITECOMBINED: return "WC";
		case NV_MEMORY_WRITEBACK: return "WB";
		case NV_MEMORY_DEFAULT: return "-";
		case NV_MEMORY_UNCACHED_WEAK: return "UCW";
		default: return "?";
	}
}

uint32 to_haiku_cache_type(NvU32 cache_type)
{
	uint32 haikuCacheType = 0;
	switch (cache_type) {
		case NV_MEMORY_CACHED:
			haikuCacheType |= 0;
			break;
		case NV_MEMORY_WRITECOMBINED:
			haikuCacheType |= B_WRITE_COMBINING_MEMORY;
			break;
		case NV_MEMORY_UNCACHED:
		case NV_MEMORY_DEFAULT:
			haikuCacheType |= B_UNCACHED_MEMORY;
			break;
	}
	return haikuCacheType;
}

void* NV_API_CALL os_map_kernel_space(
    NvU64 start,
    NvU64 size_bytes,
    NvU32 mode
)
{
	TRACE("os_map_kernel_space(%#" PRIx64 ", %#" PRIx64 ", %#" PRIx32 ")\n", start, size_bytes, mode);
	void *address = nullptr;
	area_id area = map_physical_memory("nvidia_gsp MMIO",
		start, size_bytes,
		B_ANY_KERNEL_ADDRESS | to_haiku_cache_type(mode),
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&address
	);
	TRACE(" -> area: %" B_PRId32 ", address: %p\n", area, address);
	if (area < 0) {
		dprintf("map_physical_memory failed: %#" B_PRIx32 "(%s)\n", area, strerror(area));
		return nullptr;
	}

	return address;
}

void NV_API_CALL os_unmap_kernel_space(
    void *addr,
    NvU64 size_bytes
)
{
    TRACE("os_unmap_kernel_space()\n");

    area_id area = area_for(addr);
    if (area >= B_OK)
    	delete_area(area);
}

NvBool NV_API_CALL nv_requires_dma_remap(
    nv_state_t *nv
)
{
    return NV_FALSE;
}

void NV_API_CALL
nv_set_dma_address_size(
    nv_state_t  *nv,
    NvU32       phys_addr_bits
)
{
    TRACE("nv_set_dma_address_size()\n");
    // TODO
}

NV_STATUS NV_API_CALL nv_alloc_pages(
    nv_state_t *nv,
    NvU32       page_count,
    NvU64       page_size,
    NvBool      contiguous,
    NvU32       cache_type,
    NvBool      zeroed,
    NvBool      unencrypted,
    NvS32       node_id,
    NvU64      *pte_array,
    void      **priv_data
)
{
	TRACE("nv_alloc_pages(page_count: %" PRIu32 ", page_size: %" PRIu64 ", cache_type: %" PRIu32 ")\n", page_count, page_size, cache_type);

	ObjectDeleter<nv_alloc_t> alloc(new(std::nothrow) nv_alloc_t());
	if (!alloc.IsSet()) {
		return NV_ERR_NO_MEMORY;
	}

	alloc->cacheType = cache_type;

	void *address = nullptr;
	alloc->area.SetTo(create_area(
		"DMA Buffer",
		&address, B_ANY_KERNEL_ADDRESS, (uint64)page_count * B_PAGE_SIZE,
		contiguous ? B_CONTIGUOUS : B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA
	));

	if (!alloc->area.IsSet()) {
		TRACE("create_area failed: %#" B_PRIx32 "(%s)\n", area, strerror(area));
		return NV_ERR_NO_MEMORY;
	}
	TRACE("  area: %" B_PRId32 "\n", alloc->area.Get());

	for (uint32 page = 0; page < ((contiguous) ? 1 : page_count); page++) {
		physical_entry table;
		if (get_memory_map((uint8*)address + (uint64)page * B_PAGE_SIZE, B_PAGE_SIZE, &table, 1) < B_OK)
			abort();

		uint64 physAdr = table.address;

		pte_array[page] = physAdr;
		// dprintf("  pte_array[%" B_PRIu32 "]: %#" B_PRIx64 "\n", page, physAdr);
	}
	TRACE("  pte_array[0]: %#" B_PRIx64 "\n", pte_array[0]);
	alloc->physBase = pte_array[0];
#if 1
	uint32 haikuCacheType = to_haiku_cache_type(cache_type);
	vm_set_area_memory_type(alloc->area.Get(), pte_array[0], haikuCacheType);
#endif

	*priv_data = alloc.Detach();
	return NV_OK;
}

NV_STATUS NV_API_CALL nv_free_pages(
    nv_state_t *nv,
    NvU32 page_count,
    NvBool contiguous,
    NvU32 cache_type,
    void *priv_data
)
{
	TRACE("nv_free_pages(area: %" B_PRId32 ")\n", area);
	delete (nv_alloc_t*)priv_data;
	return NV_OK;
}

void* NV_API_CALL nv_alloc_kernel_mapping(
    nv_state_t *nv,
    void       *pAllocPrivate,
    NvU64       pageIndex,
    NvU32       pageOffset,
    NvU64       size,
    void      **pPrivate
)
{
	TRACE("nv_alloc_kernel_mapping(%" PRIu64 ", %" PRIu32 ", %" PRIu64 ")\n", pageIndex, pageOffset, size);
	auto alloc = static_cast<nv_alloc_t*>(pAllocPrivate);
	area_id area = alloc->area.Get();

	area_info info {};
	if (get_area_info(area, &info) < B_OK)
		return nullptr;

	void *address = info.address;

	*pPrivate = (void*)(addr_t)area;
	return (uint8*)address + (uint64)pageIndex * B_PAGE_SIZE + pageOffset;
}

NV_STATUS NV_API_CALL nv_free_kernel_mapping(
    nv_state_t *nv,
    void       *pAllocPrivate,
    void       *address,
    void       *pPrivate
)
{
	TRACE("nv_free_kernel_mapping()\n");
	return NV_OK;
}

NvU64 NV_API_CALL nv_get_dma_start_address(
    nv_state_t *nv
)
{
	TRACE("nv_get_dma_start_address()\n");
	return 0;
}

NvU64 NV_API_CALL os_get_max_user_va(void)
{
	TRACE("os_get_max_user_va()\n");
	return 0xa000000000000000LL;
}

NV_STATUS NV_API_CALL os_flush_cpu_cache_all(void)
{
	TRACE("os_flush_cpu_write_combine_buffer()\n");
	// not needed for x86
	return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL os_flush_cpu_write_combine_buffer(void)
{
	TRACE("os_flush_cpu_write_combine_buffer()\n");
	// TODO
}

NV_STATUS NV_API_CALL nv_get_usermap_access_params(
	nv_state_t *nv,
	nv_usermap_access_params_t *nvuap
)
{
	TRACE("nv_get_usermap_access_params()\n");

	nvuap->remap_prot_extra = 0;

	return NV_OK;
}

NV_STATUS NV_API_CALL nv_add_mapping_context_to_file(
	nv_state_t *nv,
	nv_usermap_access_params_t *nvuap,
	NvU32       prot,
	void       *pAllocPriv,
	NvU64       pageIndex,
	NvU32       fd
)
{
#if 0
	dprintf("nv_add_mapping_context_to_file(pAllocPriv: %p, pageIndex: %#" B_PRIx64 ", fd: %" B_PRIu32 ")\n", pAllocPriv, pageIndex, fd);

	dprintf("  nvuap:\n");
	dprintf("    addr: %#" B_PRIx64 "\n", nvuap->addr);
	dprintf("    size: %#" B_PRIx64 "\n", nvuap->size);
	dprintf("    offset: %#" B_PRIx64 "\n", nvuap->offset);
	dprintf("    page_array: %p\n", nvuap->page_array);
	dprintf("    num_pages: %#" B_PRIx64 "\n", nvuap->num_pages);
	for (NvU64 i = 0; i < nvuap->memArea.numRanges; i++) {
		const MemoryRange &range = nvuap->memArea.pRanges[i];
		dprintf("    nvuap.memArea[%" B_PRIu64 "]: (%#" B_PRIx64 ", %#" B_PRIx64 ")\n", i, range.start, range.size);
	}
	dprintf("    access_start: %#" B_PRIx64 "\n", nvuap->access_start);
	dprintf("    access_size: %#" B_PRIx64 "\n", nvuap->access_size);
	dprintf("    remap_prot_extra: %#" B_PRIx64 "\n", nvuap->remap_prot_extra);
	dprintf("    contig: %d\n", nvuap->contig);
	dprintf("    caching: %s\n", nv_cache_type_str(nvuap->caching));
#endif

    NV_STATUS status = NV_OK;
    nv_alloc_mapping_context_t *nvamc = nullptr;
    nv_file_private_t *nvfp = nullptr;
    NvHaikuBaseDeviceHandle *nvlfp = nullptr;
    void *priv = nullptr;

    nvfp = nv_get_file_private(fd, NV_IS_CTL_DEVICE(nv), &priv);
    if (nvfp == nullptr) {
    	dprintf("[!] nvfp == nullptr\n");
        return NV_ERR_INVALID_ARGUMENT;
    }

    nvlfp = NvHaikuBaseDeviceHandle::FromNvfp(nvfp);

    nvamc = &nvlfp->MmapContext();

    if (nvamc->valid)
    {
        status = NV_ERR_STATE_IN_USE;
        goto done;
    }

    os_mem_set((void*) nvamc, 0, sizeof(nv_alloc_mapping_context_t));

    if (NV_IS_CTL_DEVICE(nv))
    {
        nvamc->alloc = pAllocPriv;
        nvamc->page_index = pageIndex;
    }
    else
    {
        if (nvlfp->GetBaseDevice()->Nv() != nv)
        {
    		dprintf("[!] nvlfp->GetBaseDevice()->Nv() != nv\n");
            status = NV_ERR_INVALID_ARGUMENT;
            goto done;
        }

        status = os_alloc_mem((void**) &nvamc->memArea.pRanges,
            sizeof(MemoryRange) * nvuap->memArea.numRanges);

        if (status != NV_OK)
        {
            nvamc->memArea.pRanges = nullptr;
            goto done;
        }
        nvamc->memArea.numRanges = nvuap->memArea.numRanges;
        os_mem_copy(nvamc->memArea.pRanges, nvuap->memArea.pRanges,
            sizeof(MemoryRange) * nvuap->memArea.numRanges);

#if 0
        if (nv_get_numa_status(nvl) == NV_NUMA_STATUS_ONLINE)
        {
            nvamc->page_array = nvuap->page_array;
            nvamc->num_pages = nvuap->num_pages;
        }
#endif
        nvamc->access_start = nvuap->access_start;
        nvamc->access_size = nvuap->access_size;
        nvamc->remap_prot_extra = nvuap->remap_prot_extra;
    }

    nvamc->prot = prot;
    nvamc->valid = NV_TRUE;
    nvamc->caching = nvuap->caching;

done:
    nv_put_file_private(priv);

    return status;
}

NV_STATUS NV_API_CALL nv_alloc_user_mapping(
	nv_state_t *nv,
	void       *pAllocPrivate,
	NvU64       pageIndex,
	NvU32       pageOffset,
	NvU64       size,
	NvU32       protect,
	NvU64      *pUserAddress,
	void      **ppPrivate
)
{
	TRACE("nv_alloc_user_mapping(pAllocPriv: %p, pageIndex: %#" B_PRIx64 ", pageOffset: %#" B_PRIx32 ", size: %#" B_PRIx64 ")\n", pAllocPrivate, pageIndex, pageOffset, size);

	auto alloc = static_cast<nv_alloc_t*>(pAllocPrivate);
	area_id area = alloc->area.Get();

	area_info info {};
	if (get_area_info(area, &info) < B_OK)
		return NV_ERR_GENERIC;

	void *address = info.address;

	physical_entry table;
	if (get_memory_map((uint8*)address + (uint64)pageIndex * B_PAGE_SIZE, B_PAGE_SIZE, &table, 1) < B_OK)
		return NV_ERR_GENERIC;

	*pUserAddress = table.address + pageOffset;
//	dprintf("  *pUserAddress: %#" B_PRIx64 "\n", *pUserAddress);

	return NV_OK;
}

NV_STATUS NV_API_CALL nv_free_user_mapping(
	nv_state_t *nv,
	void       *pAllocPrivate,
	NvU64       userAddress,
	void       *pPrivate
)
{
	return NV_OK;
}

NV_STATUS NV_API_CALL os_match_mmap_offset(
	void  *pAllocPrivate,
	NvU64  offset,
	NvU64 *pPageIndex
)
{
	TRACE("os_match_mmap_offset(pAllocPriv: %p, offset: %#" B_PRIx64 ")\n", pAllocPrivate, offset);

	*pPageIndex = 0;

	// return NV_ERR_OBJECT_NOT_FOUND;
	return NV_OK;
}


//#pragma mark - spinlock

NV_STATUS NV_API_CALL os_alloc_spinlock(void **ppSpinlock)
{
    TRACE("os_alloc_spinlock()\n");

    NV_STATUS rmStatus;
    os_spinlock_t *os_spinlock;

    rmStatus = os_alloc_mem(ppSpinlock, sizeof(os_spinlock_t));
    if (rmStatus != NV_OK)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate spinlock!\n");
        return rmStatus;
    }

    os_spinlock = (os_spinlock_t *)*ppSpinlock;
    *os_spinlock = B_SPINLOCK_INITIALIZER;

    return NV_OK;
}

void NV_API_CALL os_free_spinlock(void *pSpinlock)
{
    TRACE("os_free_spinlock()\n");

    os_free_mem(pSpinlock);
}

NvU64 NV_API_CALL os_acquire_spinlock(void *pSpinlock)
{
    TRACE("os_acquire_spinlock()\n");

    os_spinlock_t *os_spinlock = (os_spinlock_t *)pSpinlock;

    NvU64 res = disable_interrupts();
    acquire_spinlock(os_spinlock);

    return res;
}

void NV_API_CALL os_release_spinlock(void *pSpinlock, NvU64 oldIrql)
{
    TRACE("os_release_spinlock()\n");

    os_spinlock_t *os_spinlock = (os_spinlock_t *)pSpinlock;

    release_spinlock(os_spinlock);
    restore_interrupts(oldIrql);
    if (oldIrql != 0) {
    	NvHaikuDriver::Instance().IntrSafePool().Maintain();
    }
}


//#pragma mark - mutex

NV_STATUS NV_API_CALL os_alloc_mutex(void **ppMutex)
{
	TRACE("os_alloc_mutex()\n");

	NV_STATUS rmStatus;
	os_mutex_t *os_mutex;

	rmStatus = os_alloc_mem(ppMutex, sizeof(os_mutex_t));
	if (rmStatus != NV_OK)
	{
		nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate mutex!\n");
		return rmStatus;
	}
	os_mutex = (os_mutex_t *)*ppMutex;
	mutex_init(os_mutex, "os_mutex_t");

	return NV_OK;
}

void NV_API_CALL os_free_mutex(void *pMutex)
{
	TRACE("os_free_mutex()\n");

	os_mutex_t *os_mutex = (os_mutex_t *)pMutex;

	if (os_mutex != nullptr)
	{
		mutex_destroy(os_mutex);
		os_free_mem(pMutex);
	}
}

NV_STATUS NV_API_CALL os_acquire_mutex(void *pMutex)
{
    TRACE("os_acquire_mutex()\n");

    os_mutex_t *os_mutex = (os_mutex_t *)pMutex;

    mutex_lock(os_mutex);

    return NV_OK;
}

NV_STATUS NV_API_CALL os_cond_acquire_mutex(void *pMutex)
{
    TRACE("os_cond_acquire_mutex()\n");

    os_mutex_t *os_mutex = (os_mutex_t *)pMutex;

    if (mutex_trylock(os_mutex) < B_OK)
    	return NV_ERR_TIMEOUT_RETRY;

    return NV_OK;
}

void NV_API_CALL os_release_mutex(void *pMutex)
{
    TRACE("os_release_mutex()\n");

    os_mutex_t *os_mutex = (os_mutex_t *)pMutex;

    mutex_unlock(os_mutex);
}


//#pragma mark - RW lock

void* NV_API_CALL os_alloc_rwlock(void)
{
    os_rwlock_t *os_rwlock = nullptr;

    NV_STATUS rmStatus = os_alloc_mem((void **)&os_rwlock, sizeof(os_rwlock_t));
    if (rmStatus != NV_OK)
    {
        nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate rw_semaphore!\n");
        return nullptr;
    }

    rw_lock_init(os_rwlock, "os_rwlock_t");

    return os_rwlock;
}

void NV_API_CALL os_free_rwlock(void *pRwLock)
{
    os_rwlock_t *os_rwlock = (os_rwlock_t *)pRwLock;
    rw_lock_destroy(os_rwlock);
    os_free_mem(os_rwlock);
}

NV_STATUS NV_API_CALL os_acquire_rwlock_read(void *pRwLock)
{
    os_rwlock_t *os_rwlock = (os_rwlock_t *)pRwLock;

    rw_lock_read_lock(os_rwlock);

    return NV_OK;
}

NV_STATUS NV_API_CALL os_acquire_rwlock_write(void *pRwLock)
{
    os_rwlock_t *os_rwlock = (os_rwlock_t *)pRwLock;

    rw_lock_write_lock(os_rwlock);

    return NV_OK;
}

NV_STATUS NV_API_CALL os_cond_acquire_rwlock_read(void *pRwLock)
{
    os_rwlock_t *os_rwlock = (os_rwlock_t *)pRwLock;

    panic("[!] os_cond_acquire_rwlock_read: not implemented\n");

    return NV_OK;
}

NV_STATUS NV_API_CALL os_cond_acquire_rwlock_write(void *pRwLock)
{
    os_rwlock_t *os_rwlock = (os_rwlock_t *)pRwLock;

    panic("[!] os_cond_acquire_rwlock_write: not implemented\n");

    return NV_OK;
}

void NV_API_CALL os_release_rwlock_read(void *pRwLock)
{
    os_rwlock_t *os_rwlock = (os_rwlock_t *)pRwLock;

    rw_lock_read_unlock(os_rwlock);
}

void NV_API_CALL os_release_rwlock_write(void *pRwLock)
{
    os_rwlock_t *os_rwlock = (os_rwlock_t *)pRwLock;

    rw_lock_write_unlock(os_rwlock);
}


//#pragma mark - semaphore

void* NV_API_CALL os_alloc_semaphore(NvU32 initialValue)
{
	NV_STATUS rmStatus;
	ObjectDeleter<os_semaphore_t> os_sema(new(std::nothrow) os_semaphore_t);
	if (os_sema.IsSet()) {
		os_sema->sem.SetTo(create_sem(initialValue, "os_semaphore_t"));
		if (os_sema->sem.IsSet()) {
			return os_sema.Detach();
		}
	}

	nv_printf(NV_DBG_ERRORS, "NVRM: failed to allocate semaphore!\n");
	return nullptr;
}

void NV_API_CALL os_free_semaphore(void *pSema)
{
	os_semaphore_t *os_sema = (os_semaphore_t *)pSema;

	delete os_sema;
}

NV_STATUS NV_API_CALL os_acquire_semaphore(void *pSema)
{
	os_semaphore_t *os_sema = (os_semaphore_t *)pSema;
	acquire_sem(os_sema->sem.Get());
	return NV_OK;
}

NV_STATUS NV_API_CALL os_cond_acquire_semaphore(void *pSema)
{
	os_semaphore_t *os_sema = (os_semaphore_t *)pSema;
	if (acquire_sem_etc(os_sema->sem.Get(), 1, B_RELATIVE_TIMEOUT, 0) < B_OK)
		return NV_ERR_TIMEOUT_RETRY;

	return NV_OK;
}

NV_STATUS NV_API_CALL os_release_semaphore(void *pSema)
{
    os_semaphore_t *os_sema = (os_semaphore_t *)pSema;
    release_sem_etc(os_sema->sem.Get(), 1, B_DO_NOT_RESCHEDULE);
    return NV_OK;
}

NvBool NV_API_CALL os_semaphore_may_sleep(void)
{
    return are_interrupts_enabled();
}


//#pragma mark - wait queue

NV_STATUS NV_API_CALL os_alloc_wait_queue(os_wait_queue **wq)
{
	*wq = new(std::nothrow) os_wait_queue();
	if (*wq == nullptr)
		return NV_ERR_NO_MEMORY;

	(*wq)->cv.Init(*wq, "os_wait_queue");

	return NV_OK;
}

void NV_API_CALL os_free_wait_queue(os_wait_queue *wq)
{
	delete wq;
}

void NV_API_CALL os_wait_uninterruptible(os_wait_queue *wq)
{
	ConditionVariableEntry entry;
	wq->cv.Add(&entry);
	entry.Wait();
}

void NV_API_CALL os_wait_interruptible(os_wait_queue *wq)
{
	ConditionVariableEntry entry;
	wq->cv.Add(&entry);
	entry.Wait(B_CAN_INTERRUPT);
}

void NV_API_CALL os_wake_up(os_wait_queue *wq)
{
	wq->cv.NotifyOne();
}


//#pragma mark - events

void NV_API_CALL nv_post_event(
    nv_event_t *event,
    NvHandle    handle,
    NvU32       index,
    NvU32       info32,
    NvU16       info16,
    NvBool      data_valid
)
{
	TRACE("nv_post_event(%p, %#" B_PRIx32 ", %#" B_PRIx32 ", %#" B_PRIx32 ", %#" B_PRIx16 ", %u)\n",
		event, handle, index, info32, info16, data_valid
	);

	nv_file_private_t *nvfp = event->nvfp;
	auto nvhfp = NvHaikuBaseDeviceHandle::FromNvfp(nvfp);

	if (data_valid) {
		// TODO
	} else {
		nvhfp->SetDatalessEventPending();
	}

	nvhfp->NotifySelectPool();
}


//#pragma mark -

void nvlink_assert(int cond)
{
	if (cond == 0)
		abort();
}

void nvlink_sleep(unsigned int ms)
{
    snooze((bigtime_t)1000 * ms);
}


//#pragma mark -

NvBool NV_API_CALL os_allow_priority_override(void)
{
    return false;
}


//#pragma mark -

NvU64 NV_API_CALL os_get_num_phys_pages(void)
{
    TRACE("os_get_num_phys_pages()\n");
    // TODO
    return 1024*1024*1024;
}


//#pragma mark -

NV_STATUS NV_API_CALL os_get_current_time(
    NvU32 *seconds,
    NvU32 *useconds
)
{
    TRACE("os_get_current_time()\n");

    bigtime_t time = real_time_clock_usecs();

    *seconds = time / 1000000;
    *useconds = time % 1000000;

    return NV_OK;
}


//#pragma mark -

NV_STATUS NV_API_CALL os_get_is_openrm(NvBool *bIsOpenRm)
{
    *bIsOpenRm = NV_TRUE;
    return NV_OK;
}

NvBool NV_API_CALL nv_is_chassis_notebook(void)
{
    TRACE("nv_is_chassis_notebook()\n");
    return false;
}

NV_STATUS NV_API_CALL nv_get_device_memory_config(
    nv_state_t *nv,
    NvU64 *compr_addr_sys_phys,
    NvU64 *addr_guest_phys,
    NvU64 *rsvd_phys,
    NvU32 *addr_width,
    NvS32 *node_id
)
{
	TRACE("nv_get_device_memory_config()\n");
	// NUMA not supported
	return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL os_device_vm_present(void)
{
	return NV_ERR_NOT_SUPPORTED;
}

NvBool NV_API_CALL os_is_efi_enabled(void)
{
    TRACE("os_is_efi_enabled()\n");
    return NV_TRUE;
}

NV_STATUS NV_API_CALL os_get_acpi_rsdp_from_uefi(NvU32 *pRsdpAddr)
{
  TRACE("os_get_acpi_rsdp_from_uefi()\n");
	*pRsdpAddr = 0;
	return NV_ERR_OPERATING_SYSTEM;
}

NvBool NV_API_CALL os_is_nvswitch_present(void)
{
    TRACE("os_is_nvswitch_present()\n");
/*
    struct pci_device_id nvswitch_pci_table[] = {
        {
            PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID),
            .class      = PCI_CLASS_BRIDGE_OTHER << 8,
            .class_mask = PCI_ANY_ID
        },
        {0}
    };

    return !!pci_dev_present(nvswitch_pci_table);
*/
    return NV_FALSE;
}

extern "C" NvlStatus
nvswitch_os_read_registery_binary
(
    void *os_handle,
    const char *name,
    NvU8 *data,
    NvU32 length
)
{
    TRACE("nvswitch_os_read_registery_binary()\n");
    return -NVL_ERR_NOT_SUPPORTED;
}

NvBool NV_API_CALL os_is_vgx_hyper(void)
{
    TRACE("os_is_vgx_hyper()\n");
    return NV_FALSE;
}

NvBool NV_API_CALL os_pat_supported(void)
{
    TRACE("os_pat_supported()\n");
    return NV_FALSE;
}

NvU32 NV_API_CALL nv_get_dev_minor(nv_state_t *nv)
{
    TRACE("nv_get_dev_minor()\n");
    return 1; // !!!
}

void NV_API_CALL nv_get_screen_info(
    nv_state_t  *nv,
    NvU64       *pPhysicalAddress,
    NvU32       *pFbWidth,
    NvU32       *pFbHeight,
    NvU32       *pFbDepth,
    NvU32       *pFbPitch,
    NvU64       *pFbSize
)
{
	TRACE("nv_get_screen_info()\n");

	frame_buffer_boot_info* bufferInfo = (frame_buffer_boot_info*)get_boot_item(FRAME_BUFFER_BOOT_INFO, nullptr);

	if (bufferInfo == nullptr) {
		*pPhysicalAddress = 0;
		*pFbWidth = 0;
		*pFbHeight = 0;
		*pFbDepth = 0;
		*pFbPitch = 0;
		*pFbSize = 0;
		return;
	}

	*pPhysicalAddress = bufferInfo->physical_frame_buffer;
	*pFbWidth = bufferInfo->width;
	*pFbHeight = bufferInfo->height;
	*pFbDepth = bufferInfo->depth;
	*pFbPitch = bufferInfo->bytes_per_row;
	*pFbSize = bufferInfo->width * bufferInfo->bytes_per_row;
}

void NV_API_CALL os_disable_console_access(void)
{
	dprintf("os_disable_console_access()\n");
  // TODO
}

void NV_API_CALL os_enable_console_access(void)
{
	dprintf("os_enable_console_access()\n");
  // TODO
}

NvU64 NV_API_CALL os_get_cpu_frequency(void)
{
	return 1000000000ULL;
}

NvU32 NV_API_CALL os_get_cpu_count()
{
    TRACE("os_get_cpu_count()\n");
    return 1; // TODO
}

NvU32 NV_API_CALL os_get_cpu_number()
{
    TRACE("os_get_cpu_number()\n");
    return 0; // TODO
}

NvU64 NV_API_CALL os_get_tick_resolution(void)
{
    TRACE("os_get_tick_resolution()\n");
    return 1000; // 1 us
}

NvU64 NV_API_CALL os_get_current_tick_hr(void)
{
    TRACE("os_get_current_tick_hr()\n");
    return system_time() * 1000LL;
}

NvU64 NV_API_CALL os_get_current_tick(void)
{
    TRACE("os_get_current_tick()\n");
    return system_time() * 1000LL;
}

void nv_get_disp_smmu_stream_ids
(
    nv_state_t *nv,
    NvU32 *dispIsoStreamId,
    NvU32 *dispNisoStreamId)
{
    TRACE("nv_get_disp_smmu_stream_ids()\n");
    *dispIsoStreamId = nv->iommus.dispIsoStreamId;
    *dispNisoStreamId = nv->iommus.dispNisoStreamId;
}

NvBool NV_API_CALL os_is_grid_supported(void)
{
	TRACE("os_is_grid_supported()\n");
	return NV_FALSE;
}

NvU32 NV_API_CALL os_get_grid_csp_support(void)
{
	TRACE("os_is_grid_supported()\n");
	return 0;
}

nv_state_t* NV_API_CALL nv_get_ctl_state(void)
{
	TRACE("nv_get_ctl_state()\n");
	return gNvHaikuControlDeviceInst->Nv();
}

nv_file_private_t* NV_API_CALL nv_get_file_private(
    NvS32 fd,
    NvBool ctl,
    void **os_private
)
{
	// TODO: correctly handle case when FD is closed from other thread
	NvHaikuBaseDeviceHandle *handle = NvHaikuBaseDeviceHandle::FromFd(fd);
	if (handle == nullptr) {
		return nullptr;
	}
	*os_private = handle;
    return &handle->Nvfp();
}

void NV_API_CALL nv_put_file_private(void *os_private)
{
	TRACE("nv_put_file_private()\n");
}

NvBool NV_API_CALL nv_is_gpu_accessible(nv_state_t *nv)
{
	TRACE("nv_is_gpu_accessible()\n");
	return NV_TRUE;
}

NvBool NV_API_CALL os_check_access(RsAccessRight accessRight)
{
    switch (accessRight)
    {
        case RS_ACCESS_PERFMON:
        case RS_ACCESS_NICE:
        {
            return os_is_administrator();
        }
        default:
        {
            return NV_FALSE;
        }
    }
}

NvBool NV_API_CALL os_is_init_ns(void)
{
    return NV_FALSE;
}

NV_STATUS nv_get_syncpoint_aperture
(
    NvU32 syncpointId,
    NvU64 *physAddr,
    NvU64 *limit,
    NvU32 *offset
)
{
    return NV_ERR_NOT_SUPPORTED;
}

NvBool NV_API_CALL os_is_queue_flush_ongoing(struct os_work_queue *queue)
{
    return NV_FALSE;
}

NV_STATUS NV_API_CALL nv_set_primary_vga_status(
    nv_state_t *nv
)
{
    return NV_ERR_NOT_SUPPORTED;
}

void NV_API_CALL nv_audio_dynamic_power(
    nv_state_t *nv
)
{
}


//#pragma mark - Processes and threads

NvU32 NV_API_CALL os_get_current_process(void)
{
    TRACE("os_get_current_process()\n");
    // TODO: get client process ID, not server
    return getpid();
}

void NV_API_CALL os_get_current_process_name(char *buf, NvU32 len)
{
	team_info info {};
	team_id team = os_get_current_process();
	if (get_team_info(team, &info) < B_OK) {
		strcpy(info.name, "");
	}

	strncpy(buf, info.name, len - 1);
	buf[len - 1] = '\0';
}


NV_STATUS NV_API_CALL os_get_euid(NvU32 *pSecToken)
{
    TRACE("os_get_euid()\n");
    *pSecToken = geteuid();
    return NV_OK;
}

NV_STATUS NV_API_CALL os_get_current_thread(NvU64 *threadId)
{
    TRACE("os_get_current_thread()\n");
    *threadId = find_thread(nullptr);
    return NV_OK;
}

void* NV_API_CALL os_get_pid_info(void)
{
	TRACE("os_get_pid_info()\n");
	return malloc(1);
}

void NV_API_CALL os_put_pid_info(void *pid_info)
{
    TRACE("os_put_pid_info()\n");
    free(pid_info);
}

NV_STATUS NV_API_CALL os_find_ns_pid(void *pid_info, NvU32 *ns_pid)
{
    TRACE("os_find_ns_pid()\n");
    if ((pid_info == nullptr) || (ns_pid == nullptr))
        return NV_ERR_INVALID_ARGUMENT;

    *ns_pid = getpid();

    if (*ns_pid == 0)
        return NV_ERR_OBJECT_NOT_FOUND;

    return NV_OK;
}

NvBool NV_API_CALL os_is_administrator(void)
{
	TRACE("os_is_administrator()\n");
	return getuid() == 0 || geteuid() == 0;
}

NV_STATUS NV_API_CALL os_schedule(void)
{
	TRACE("os_delay_us()\n");
	snooze(1);
	return NV_OK;
}

NV_STATUS NV_API_CALL os_delay_us(NvU32 MicroSeconds)
{
    TRACE("os_delay_us()\n");
    snooze(MicroSeconds);
    return NV_OK;
}

NV_STATUS NV_API_CALL os_delay(NvU32 MilliSeconds)
{
    TRACE("os_delay()\n");
    snooze((bigtime_t)MilliSeconds * 1000);
    return NV_OK;
}

NvBool NV_API_CALL os_is_isr(void)
{
    TRACE("os_is_isr()\n");
    return !are_interrupts_enabled();
}

void NV_API_CALL
nv_schedule_uvm_isr(nv_state_t *nv)
{
    dprintf("nv_schedule_uvm_isr()\n");
}


//#pragma mark - Timers

int NV_API_CALL nv_start_rc_timer(
    nv_state_t *nv
)
{
    TRACE("nv_start_rc_timer()\n");
    // TODO
    return 0;
}

int NV_API_CALL nv_stop_rc_timer(
    nv_state_t *nv
)
{
    TRACE("nv_stop_rc_timer()\n");
    // TODO
    return 0;
}


//#pragma mark - PCI bus

void* NV_API_CALL os_pci_init_handle(
	NvU32 domain,
	NvU8  bus,
	NvU8  slot,
	NvU8  function,
	NvU16 *vendor,
	NvU16 *device
)
{
	TRACE("os_pci_init_handle()\n");
	if (domain != 0) {
		return nullptr;
	}

	pci_info pciInfo {};
	for (uint32 i = 0; NvHaikuDriver::Instance().PCI().get_nth_pci_info(i, &pciInfo) >= B_OK; i++) {
		if (pciInfo.bus == bus && pciInfo.device == slot && pciInfo.function == function) {
			pci_info *handle = (pci_info*)malloc(sizeof(pci_info));
			if (handle == nullptr)
				return nullptr;

			memcpy(handle, &pciInfo, sizeof(pci_info));
			if (vendor != nullptr)
				*vendor = pciInfo.vendor_id;
			if (device != nullptr)
				*device = pciInfo.device_id;
			return handle;
		}
	}

	return nullptr;
}

void NV_API_CALL os_pci_remove(void *handle)
{
	pci_info *pciInfo = (pci_info*)handle;
	free(pciInfo);
}

NV_STATUS NV_API_CALL os_pci_read_byte(
	void *handle,
	NvU32 offset,
	NvU8 *pReturnValue
)
{
	pci_info *pciInfo = (pci_info*)handle;
	*pReturnValue = NvHaikuDriver::Instance().PCI().read_pci_config(pciInfo->bus, pciInfo->device, pciInfo->function, offset, 1);
	return NV_OK;
}

NV_STATUS NV_API_CALL os_pci_read_word(
	void *handle,
	NvU32 offset,
	NvU16 *pReturnValue
)
{
	pci_info *pciInfo = (pci_info*)handle;
	*pReturnValue = NvHaikuDriver::Instance().PCI().read_pci_config(pciInfo->bus, pciInfo->device, pciInfo->function, offset, 2);
	return NV_OK;
}

NV_STATUS NV_API_CALL os_pci_read_dword(
	void *handle,
	NvU32 offset,
	NvU32 *pReturnValue
)
{
	pci_info *pciInfo = (pci_info*)handle;
	*pReturnValue = NvHaikuDriver::Instance().PCI().read_pci_config(pciInfo->bus, pciInfo->device, pciInfo->function, offset, 4);
	return NV_OK;
}

NV_STATUS NV_API_CALL os_pci_write_byte(
	void *handle,
	NvU32 offset,
	NvU8 value
)
{
	pci_info *pciInfo = (pci_info*)handle;
	NvHaikuDriver::Instance().PCI().write_pci_config(pciInfo->bus, pciInfo->device, pciInfo->function, offset, 1, value);
	return NV_OK;
}

NV_STATUS NV_API_CALL os_pci_write_word(
	void *handle,
	NvU32 offset,
	NvU16 value
)
{
	pci_info *pciInfo = (pci_info*)handle;
	NvHaikuDriver::Instance().PCI().write_pci_config(pciInfo->bus, pciInfo->device, pciInfo->function, offset, 2, value);
	return NV_OK;
}

NV_STATUS NV_API_CALL os_pci_write_dword(
	void *handle,
	NvU32 offset,
	NvU32 value
)
{
	pci_info *pciInfo = (pci_info*)handle;
	NvHaikuDriver::Instance().PCI().write_pci_config(pciInfo->bus, pciInfo->device, pciInfo->function, offset, 4, value);
	return NV_OK;
}


//#pragma mark - ACPI


void NV_API_CALL nv_acpi_methods_init(NvU32 *handlePresent)
{
    *handlePresent = 0;
}

void NV_API_CALL nv_acpi_methods_uninit(void)
{
    return;
}

NV_STATUS NV_API_CALL nv_acpi_method(
    NvU32 acpi_method,
    NvU32 function,
    NvU32 subFunction,
    void  *inParams,
    NvU16 inParamSize,
    NvU32 *outStatus,
    void  *outData,
    NvU16 *outDataSize
)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_acpi_dsm_method(
    nv_state_t *nv,
    NvU8  *pAcpiDsmGuid,
    NvU32 acpiDsmRev,
    NvBool acpiNvpcfDsmFunction,
    NvU32 acpiDsmSubFunction,
    void  *pInParams,
    NvU16 inParamSize,
    NvU32 *outStatus,
    void  *pOutData,
    NvU16 *pSize
)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_acpi_ddc_method(
    nv_state_t *nv,
    void *pEdidBuffer,
    NvU32 *pSize,
    NvBool bReadMultiBlock
)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_acpi_rom_method(
    nv_state_t *nv,
    NvU32 *pInData,
    NvU32 *pOutData
)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_acpi_dod_method(
    nv_state_t *nv,
    NvU32      *pOutData,
    NvU32      *pSize
)
{
    return NV_ERR_NOT_SUPPORTED;
}

NvBool nv_acpi_power_resource_method_present(
    struct pci_dev *pdev
)
{
    return NV_FALSE;
}

NV_STATUS NV_API_CALL nv_acpi_get_powersource(NvU32 *ac_plugged)
{
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS NV_API_CALL nv_acpi_mux_method(
    nv_state_t *nv,
    NvU32 *pInOut,
    NvU32 muxAcpiId,
    const char *pMethodName
)
{
    return NV_ERR_NOT_SUPPORTED;
}

NvBool NV_API_CALL nv_acpi_is_battery_present(void)
{
    return NV_FALSE;
}


//#pragma mark - I2C

void* NV_API_CALL nv_i2c_add_adapter(nv_state_t *nv, NvU32 port)
{
    return nullptr;
}

void NV_API_CALL nv_i2c_del_adapter(nv_state_t *nv, void *data)
{
}


//#pragma mark - firmware

#define NV_FIRMWARE_PATH_FOR_FILENAME(name) "/firmware/nvidia/570.86.16/" name

static void HexDump(uint8 *data, uint32 size)
{
	char line[256];
	char *lineEnd = line;
	for (uint32 offset = 0; offset < size; offset++) {
		if (offset != 0 && offset % 0x10 == 0) {
			dprintf("%s\n", line);
			lineEnd = line;
			lineEnd += sprintf(lineEnd, "%08" B_PRIx32 " ", offset);
		}
		lineEnd += sprintf(lineEnd, " %02" B_PRIx8, data[offset]);
	}
	dprintf("%s\n", line);
}

static inline const char *nv_firmware_path(
    nv_firmware_type_t fw_type,
    nv_firmware_chip_family_t fw_chip_family
)
{
    if (fw_type == NV_FIRMWARE_TYPE_GSP)
    {
        switch (fw_chip_family)
        {
            case NV_FIRMWARE_CHIP_FAMILY_GH100:  // fall through
            case NV_FIRMWARE_CHIP_FAMILY_AD10X:  // fall through
            case NV_FIRMWARE_CHIP_FAMILY_GA10X:
                return NV_FIRMWARE_PATH_FOR_FILENAME("gsp_ga10x.bin");

            case NV_FIRMWARE_CHIP_FAMILY_GA100:  // fall through
            case NV_FIRMWARE_CHIP_FAMILY_TU11X:  // fall through
            case NV_FIRMWARE_CHIP_FAMILY_TU10X:
                return NV_FIRMWARE_PATH_FOR_FILENAME("gsp_tu10x.bin");

            case NV_FIRMWARE_CHIP_FAMILY_END:  // fall through
            case NV_FIRMWARE_CHIP_FAMILY_NULL:
                return "";

            default:
                return "";
        }
    }
    else if (fw_type == NV_FIRMWARE_TYPE_GSP_LOG)
    {
        switch (fw_chip_family)
        {
            case NV_FIRMWARE_CHIP_FAMILY_GH100:  // fall through
            case NV_FIRMWARE_CHIP_FAMILY_AD10X:  // fall through
            case NV_FIRMWARE_CHIP_FAMILY_GA10X:
                return NV_FIRMWARE_PATH_FOR_FILENAME("gsp_log_ga10x.bin");

            case NV_FIRMWARE_CHIP_FAMILY_GA100:  // fall through
            case NV_FIRMWARE_CHIP_FAMILY_TU11X:  // fall through
            case NV_FIRMWARE_CHIP_FAMILY_TU10X:
                return NV_FIRMWARE_PATH_FOR_FILENAME("gsp_log_tu10x.bin");

            case NV_FIRMWARE_CHIP_FAMILY_END:  // fall through
            case NV_FIRMWARE_CHIP_FAMILY_NULL:
                return "";

            default:
                return "";
        }
    }

    return "";
}

const void* NV_API_CALL nv_get_firmware(
    nv_state_t *nv,
    nv_firmware_type_t fw_type,
    nv_firmware_chip_family_t fw_chip_family,
    const void **fw_buf,
    NvU32 *fw_size
)
{
	TRACE("nv_get_firmware()\n");

	static const directory_which dataDirectories[] = {
		B_USER_NONPACKAGED_DATA_DIRECTORY,
		B_USER_DATA_DIRECTORY,
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY,
	};

	const char *suffix = nv_firmware_path(fw_type, fw_chip_family);
	if (suffix == nullptr) {
		return nullptr;
	}

	FileDescriptorCloser file;
	for (uint i = 0; i < sizeof(dataDirectories) / sizeof(directory_which); i++) {
		char path[B_PATH_NAME_LENGTH];
		if (find_directory(dataDirectories[i], -1, false, (char*)&path, B_PATH_NAME_LENGTH) < B_OK) {
			continue;
		}

		if (strlen(path) + strlen(suffix) > B_PATH_NAME_LENGTH - 1) {
			continue;
		}
		strcat(path, nv_firmware_path(fw_type, fw_chip_family));

		file.SetTo(open(path, O_RDONLY));
		if (file.IsSet()) {
			break;
		}
	}

	if (!file.IsSet())
		return nullptr;

	off_t size = lseek(file.Get(), 0, SEEK_END);
	lseek(file.Get(), 0, SEEK_SET);

	ArrayDeleter<uint8> data(new(std::nothrow) uint8[size]);
	memset(&data[0], 0, size);
	if (!data.IsSet()) {
		dprintf("[!] no memory\n");
		return nullptr;
	}

	off_t readSize = read_pos(file.Get(), 0, &data[0], size);
	if (readSize == -1 || readSize < size) {
		dprintf("[!] can't read firmware\n");
		return nullptr;
	}

	*fw_buf = &data[0];
	*fw_size = size;

	return data.Detach();
}

void NV_API_CALL nv_put_firmware(
    const void *fw_handle
)
{
	TRACE("nv_put_firmware()\n");
	const uint8 *data = static_cast<const uint8*>(fw_handle);
	delete[] data;
}

//#pragma mark -

nv_cap_t* NV_API_CALL os_nv_cap_init(const char *path)
{
    TRACE("os_nv_cap_init()\n");
    return nullptr;
}

nv_cap_t* NV_API_CALL os_nv_cap_create_dir_entry
(
    nv_cap_t *parent_cap,
    const char *name,
    int mode
)
{
    TRACE("os_nv_cap_create_dir_entry()\n");
    return (nv_cap_t*)malloc(1);
}

nv_cap_t* NV_API_CALL os_nv_cap_create_file_entry
(
    nv_cap_t *parent_cap,
    const char *name,
    int mode
)
{
    TRACE("os_nv_cap_create_file_entry()\n");
    return (nv_cap_t*)malloc(1);
}

void NV_API_CALL os_nv_cap_destroy_entry(nv_cap_t *cap)
{
    TRACE("os_nv_cap_destroy_entry()\n");
    free(cap);
}

int NV_API_CALL os_nv_cap_validate_and_dup_fd
(
    const nv_cap_t *cap,
    int fd
)
{
    TRACE("os_nv_cap_validate_and_dup_fd()\n");
    return dup(fd);
}

void NV_API_CALL os_nv_cap_close_fd(int fd)
{
    TRACE("os_nv_cap_close_fd()\n");
}

NV_STATUS NV_API_CALL nv_acquire_fabric_mgmt_cap(int fd, int *duped_fd)
{
	TRACE("nv_acquire_fabric_mgmt_cap()\n");
	*duped_fd = dup(fd);
	return NV_OK;
}


//#pragma mark -

NV_STATUS NV_API_CALL os_registry_init(void)
{
    TRACE("os_registry_init()\n");

	// Handle nonstall interrupts in DPC instead of interrupt handler directly.
	rm_write_registry_dword(nullptr, nullptr, NV_REG_PROCESS_NONSTALL_INTR_IN_LOCKLESS_ISR, NV_REG_PROCESS_NONSTALL_INTR_IN_LOCKLESS_ISR_DISABLE);

    return NV_OK;
}
