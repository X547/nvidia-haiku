#include "NvOsEvent.h"

#include <fcntl.h>
#include <unistd.h>

#include <utility>

extern "C" {
#include "nv.h"
#include "nv_escape.h"
#include "nv-ioctl.h"
#include "nv-haiku.h"
#include "nvos.h"                          // NV01_EVENT_*, DRF_NUM
#include "class/cl0005.h"                  // NV0005_ALLOC_PARAMETERS
#include "ctrl/ctrl2080/ctrl2080event.h"   // NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION
}

#include <ErrorUtils.h>


// NV_ESC_ALLOC_OS_EVENT must be issued on the *same* fd that's being
// registered as the OS event (the kernel matches the ioctl fd against the
// payload fd), so this class owns a dedicated control-device fd rather than
// sharing the NvRmApi's main fd. Same constraint applies to NV_ESC_FREE_OS_EVENT.
NvOsEvent::NvOsEvent(NvRmApi &rm):
	fRm(&rm),
	fFd(open("/dev/" NVIDIA_CONTROL_DEVICE_NAME, O_RDWR | O_CLOEXEC))
{
	CheckErrno(fFd.Get());

	nv_ioctl_alloc_os_event_t p {
		.hClient = rm.Client().Get(),
		.fd = (NvU32)fFd.Get(),
	};
	int ret = NvRmIoctl(fFd.Get(), NV_ESC_ALLOC_OS_EVENT, &p, sizeof(p));
	CheckErrno(ret);
	CheckNvStatus(p.Status);
}

NvOsEvent::NvOsEvent(NvOsEvent &&other):
	fRm(other.fRm),
	fFd(std::move(other.fFd))
{
	other.fRm = nullptr;
}

NvOsEvent &NvOsEvent::operator=(NvOsEvent &&other)
{
	Reset();
	fRm = other.fRm;
	fFd = std::move(other.fFd);
	other.fRm = nullptr;
	return *this;
}

NvOsEvent::~NvOsEvent()
{
	Reset();
}

void NvOsEvent::Reset()
{
	if (fRm == nullptr || !fFd.IsSet()) {
		return;
	}
	nv_ioctl_free_os_event_t p {
		.hClient = fRm->Client().Get(),
		.fd = (NvU32)fFd.Get(),
	};
	NvRmIoctl(fFd.Get(), NV_ESC_FREE_OS_EVENT, &p, sizeof(p));
	fFd = FileDesc();
	fRm = nullptr;
}


NvRmObject AllocSubdeviceOsEvent(
	NvRmDevice &rmDev,
	const NvOsEvent &osEvent,
	NvU32 notifyIndex,
	bool nonStallIntr)
{
	NvRmApi &rm = *rmDev.Subdevice().Rm();
	NV0005_ALLOC_PARAMETERS p {
		.hParentClient = rm.Client().Get(),
		.hSrcResource = rmDev.Subdevice().Get(),
		.hClass = NV01_EVENT_OS_EVENT,
		.notifyIndex =
			notifyIndex |
			(nonStallIntr ? NV01_EVENT_NONSTALL_INTR : 0) |
			NV01_EVENT_WITHOUT_EVENT_DATA |
			NV01_EVENT_SUBDEVICE_SPECIFIC |
			DRF_NUM(0005, _NOTIFY_INDEX, _SUBDEVICE, 0),
		.data = (NvP64)(uintptr_t)osEvent.Fd(),
	};
	NvRmObject event = rmDev.Subdevice().Alloc(NV01_EVENT_OS_EVENT, &p, sizeof(p));

	// Non-stall-interrupt events sit on a separate per-engine list that
	// fires unconditionally; everything else (RC_ERROR, etc.) is dispatched
	// via gpuNotifySubDeviceEvent, which silently skips notifyIndex values
	// whose subdevice notifyActions[] entry is still DISABLE (the default).
	// Arm REPEAT so the event keeps firing across the channel's lifetime.
	if (!nonStallIntr) {
		NV2080_CTRL_EVENT_SET_NOTIFICATION_PARAMS arm {
			.event = notifyIndex,
			.action = NV2080_CTRL_EVENT_SET_NOTIFICATION_ACTION_REPEAT,
		};
		rmDev.Subdevice().Control(NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION,
			&arm, sizeof(arm));
	}

	return event;
}
