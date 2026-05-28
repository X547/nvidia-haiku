extern "C" {
#include <nvidia-modeset-os-interface.h>
}

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <new>

#include <OS.h>
#include <KernelExport.h>
#include <kernel.h>
#include <thread.h>
#include <DPC.h>

#include <AutoDeleterOS.h>
#include <util/AutoLock.h>

#include "nvidia/nv-haiku-kernel.h"

#include "ContainerOf.h"
#include "nv-kref.h"

#include "KmsDriver.h"
#include "KmsDevice.h"


#define NVKMS_LOG_PREFIX "nvidia-modeset: "


NvBool nvkms_test_fail_alloc_core_channel(enum FailAllocCoreChannelMethod method)
{
	return NV_FALSE;
}

NvBool nvkms_output_rounding_fix(void)
{
	return NV_FALSE;
}

NvBool nvkms_disable_hdmi_frl(void)
{
	return NV_FALSE;
}

NvBool nvkms_disable_vrr_memclk_switch(void)
{
	return NV_FALSE;
}

NvBool nvkms_hdmi_deepcolor(void)
{
	return NV_FALSE;
}

NvBool nvkms_vblank_sem_control(void)
{
	return NV_FALSE;
}

NvBool nvkms_opportunistic_display_sync(void)
{
	return NV_FALSE;
}

enum NvKmsDebugForceColorSpace nvkms_debug_force_color_space(void)
{
	return NVKMS_DEBUG_FORCE_COLOR_SPACE_NONE;
}

NvBool nvkms_enable_overlay_layers(void)
{
	return NV_TRUE;
}


// #pragma mark - libc

void* nvkms_alloc(size_t size, NvBool zero)
{
	return zero ? calloc(size, 1) : malloc(size);
}

void nvkms_free(void *ptr, size_t size)
{
	(void)size;
	free(ptr);
}

void* nvkms_memset(void *ptr, NvU8 c, size_t size)
{
	return memset(ptr, c, size);
}

void* nvkms_memcpy(void *dest, const void *src, size_t n)
{
	return memcpy(dest, src, n);
}

void* nvkms_memmove(void *dest, const void *src, size_t n)
{
	return memmove(dest, src, n);
}

int nvkms_memcmp(const void *s1, const void *s2, size_t n)
{
	return memcmp(s1, s2, n);
}

size_t nvkms_strlen(const char *s)
{
	return strlen(s);
}

int nvkms_strcmp(const char *s1, const char *s2)
{
	return strcmp(s1, s2);
}

char* nvkms_strncpy(char *dest, const char *src, size_t n)
{
	return strncpy(dest, src, n);
}

int nvkms_snprintf(char *str, size_t size, const char *format, ...)
{
	int ret;
	va_list ap;

	va_start(ap, format);
	ret = vsnprintf(str, size, format, ap);
	va_end(ap);

	return ret;
}

int nvkms_vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
	return vsnprintf(str, size, format, ap);
}


// # pragma mark -

void nvkms_usleep(NvU64 usec)
{
	snooze(usec);
}

NvU64 nvkms_get_usec(void)
{
	return system_time();
}

int nvkms_copyin(void *kptr, NvU64 uaddr, size_t n)
{
	if (!(IS_KERNEL_ADDRESS(kptr) && IS_USER_ADDRESS(uaddr))) {
		return B_BAD_VALUE;
	}
	return user_memcpy(kptr, (const void*)uaddr, n);
}

int nvkms_copyout(NvU64 uaddr, const void *kptr, size_t n)
{
	if (!(IS_USER_ADDRESS(uaddr) && IS_KERNEL_ADDRESS(kptr))) {
		return B_BAD_VALUE;
	}
	return user_memcpy((void*)uaddr, kptr, n);
}

void nvkms_yield(void)
{
	thread_yield();
}

void nvkms_dump_stack(void)
{
}

NvBool nvkms_syncpt_op(enum NvKmsSyncPtOp op, NvKmsSyncPtOpParams *params)
{
	// unused
	return NV_FALSE;
}

void nvkms_log(const int level, const char *gpuPrefix, const char *msg)
{
	const char *levelPrefix;

	switch (level) {
	default:
	case NVKMS_LOG_LEVEL_INFO:
		levelPrefix = "";
		break;
	case NVKMS_LOG_LEVEL_WARN:
		levelPrefix = "WARNING: ";
		break;
	case NVKMS_LOG_LEVEL_ERROR:
		levelPrefix = "ERROR: ";
		break;
	}

	dprintf("%s%s%s%s\n",
	        NVKMS_LOG_PREFIX, levelPrefix, gpuPrefix, msg);
}


// #pragma mark - ref_ptr

struct nvkms_ref_ptr {
    nv_kref_t refcnt;
    // Access to ptr is guarded by the nvkms_lock.
    void *ptr;
};

struct nvkms_ref_ptr* nvkms_alloc_ref_ptr(void *ptr)
{
    struct nvkms_ref_ptr *ref_ptr = (struct nvkms_ref_ptr *)nvkms_alloc(sizeof(*ref_ptr), NV_FALSE);
    if (ref_ptr) {
        // The ref_ptr owner counts as a reference on the ref_ptr itself.
        nv_kref_init(&ref_ptr->refcnt);
        ref_ptr->ptr = ptr;
    }
    return ref_ptr;
}

void nvkms_free_ref_ptr(struct nvkms_ref_ptr *ref_ptr)
{
    if (ref_ptr) {
        ref_ptr->ptr = NULL;
        // Release the owner's reference of the ref_ptr.
        nvkms_dec_ref(ref_ptr);
    }
}

void nvkms_inc_ref(struct nvkms_ref_ptr *ref_ptr)
{
    nv_kref_get(&ref_ptr->refcnt);
}

static void ref_ptr_free(nv_kref_t *ref)
{
    struct nvkms_ref_ptr *ref_ptr = &ContainerOf(*ref, &nvkms_ref_ptr::refcnt);
    nvkms_free(ref_ptr, sizeof(*ref_ptr));
}

void* nvkms_dec_ref(struct nvkms_ref_ptr *ref_ptr)
{
    void *ptr = ref_ptr->ptr;
    nv_kref_put(&ref_ptr->refcnt, ref_ptr_free);
    return ptr;
}


// #pragma mark - timer queue

#define NV_TIMER_QUEUE_ENABLED 1

nvkms_timer_handle_t* nvkms_alloc_timer(nvkms_timer_proc_t *proc, void *dataPtr, NvU32 dataU32, NvU64 usec)
{
#if NV_TIMER_QUEUE_ENABLED
	return NvHaikuKmsDriver::Instance().TimerQueue().Alloc(proc, dataPtr, dataU32, usec);
#else
	return nullptr;
#endif
}

NvBool nvkms_alloc_timer_with_ref_ptr(nvkms_timer_proc_t *proc, struct nvkms_ref_ptr *ref_ptr, NvU32 dataU32, NvU64 usec)
{
#if NV_TIMER_QUEUE_ENABLED
	return NvHaikuKmsDriver::Instance().TimerQueue().AllocWithRefPtr(proc, ref_ptr, dataU32, usec);
#else
	return NV_FALSE;
#endif
}

void nvkms_free_timer(nvkms_timer_handle_t *handle)
{
#if NV_TIMER_QUEUE_ENABLED
	NvHaikuKmsDriver::Instance().TimerQueue().Free(handle);
#endif
}


// #pragma mark -

void nvkms_event_queue_changed(nvkms_per_open_handle_t *pOpenKernel, NvBool eventsAvailable)
{
	NvHaikuKmsDeviceHandle::FromNvKmsHandle(pOpenKernel)->EventQueueChanged(eventsAvailable);
}

void* nvkms_get_per_open_data(int fd)
{
	return NULL;
}


// #pragma mark - nvidia module calls

void nvkms_call_rm(void *ops)
{
	NvHaikuKmsDriver::Instance().NvidiaModule().op(ops);
}

NvBool nvkms_open_gpu(NvU32 gpuId)
{
	return NvHaikuKmsDriver::Instance().NvidiaModule().open_gpu(gpuId);
}

void nvkms_close_gpu(NvU32 gpuId)
{
	return NvHaikuKmsDriver::Instance().NvidiaModule().close_gpu(gpuId);
}

NvU32 nvkms_enumerate_gpus(nv_gpu_info_t *gpu_info)
{
	return NvHaikuKmsDriver::Instance().NvidiaModule().enumerate_gpus(gpu_info);
}

NvBool nvkms_allow_write_combining(void)
{
	return NvHaikuKmsDriver::Instance().NvidiaModule().system_info.allow_write_combining;
}


// #pragma mark -

NvBool nvkms_kernel_supports_syncpts(void)
{
	return NV_FALSE;
}

NvBool nvkms_fd_is_nvidia_chardev(int fd)
{
	// TODO: implement
	return NV_TRUE;
}


// #pragma mark - kapi (not used)

struct nvkms_per_open* nvkms_open_from_kapi(struct NvKmsKapiDevice *device)
{
	return NULL;
}

void nvkms_close_from_kapi(struct nvkms_per_open *popen)
{
}

NvBool nvkms_ioctl_from_kapi(struct nvkms_per_open *popen, NvU32 cmd, void *params_address, const size_t params_size)
{
	return NV_FALSE;
}

NvBool nvkms_ioctl_from_kapi_try_pmlock(struct nvkms_per_open *popen, NvU32 cmd, void *params_address, const size_t params_size)
{
	return NV_FALSE;
}


// #pragma mark - semaphore

struct nvkms_sema_t {
    SemDeleter sem;
};

nvkms_sema_handle_t *nvkms_sema_alloc(void)
{
	ObjectDeleter<nvkms_sema_t> sema = new(std::nothrow) nvkms_sema_t();
	if (!sema.IsSet()) {
		return NULL;
	}
	sema->sem.SetTo(create_sem(0, "nvkms_sema_t"));
	if (!sema->sem.IsSet()) {
		return NULL;
	}
	return sema.Detach();
}

void nvkms_sema_free(nvkms_sema_handle_t *sema)
{
	delete(sema);
}

void nvkms_sema_down(nvkms_sema_handle_t *sema)
{
	release_sem_etc(sema->sem.Get(), 1, B_DO_NOT_RESCHEDULE);
}

void nvkms_sema_up(nvkms_sema_handle_t *sema)
{
	acquire_sem(sema->sem.Get());
}


// #pragma mark -

struct nvkms_backlight_device* nvkms_register_backlight(NvU32 gpu_id, NvU32 display_id, void *drv_priv, NvU32 current_brightness)
{
	return NULL;
}

void nvkms_unregister_backlight(struct nvkms_backlight_device *nvkms_bd)
{
}
