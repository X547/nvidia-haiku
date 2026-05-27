#pragma once

#include <NvRmApi.h>
#include <NvRmDevice.h>
#include <FileDesc.h>


// RAII wrapper around a poll()-able OS event fd allocated against the
// NVRM control device. Each NvOsEvent owns its own file descriptor, which
// is what becomes signalled when the GPU triggers the bound NV01_EVENT.
class NvOsEvent {
private:
	NvRmApi *fRm {};
	FileDesc fFd;

	void Reset();

public:
	NvOsEvent() = default;
	NvOsEvent(NvRmApi &rm);
	NvOsEvent(NvOsEvent &&other);
	NvOsEvent &operator=(NvOsEvent &&other);
	~NvOsEvent();

	NvOsEvent(const NvOsEvent &) = delete;
	NvOsEvent &operator=(const NvOsEvent &) = delete;

	bool IsSet() const {return fFd.IsSet();}
	int Fd() const {return fFd.Get();}
};


// Bind an NV01_EVENT_OS_EVENT to `rmDev`'s subdevice that posts to
// `osEvent`'s fd when the given NV2080_NOTIFIERS_* index fires. The fd
// can back multiple such events (the kernel refcounts the registration);
// dropping the returned handle unregisters this one.
//
// `nonStallIntr` must be true for events delivered via the non-stall
// interrupt path (e.g. NV2080_NOTIFIERS_FIFO_EVENT_MTHD) and false for
// events delivered through gpuNotifySubDeviceEvent (e.g.
// NV2080_NOTIFIERS_RC_ERROR). With the wrong flag the event is filed
// into the wrong notification list and never wakes the fd.
//
// When `nonStallIntr` is false the helper additionally arms the
// subdevice's notifyActions[notifyIndex] to REPEAT — without it
// gpuNotifySubDeviceEvent silently skips the event (the action defaults
// to DISABLE, and unlike the non-stall path nothing else flips it on).
NvRmObject AllocSubdeviceOsEvent(
	NvRmDevice &rmDev,
	const NvOsEvent &osEvent,
	NvU32 notifyIndex,
	bool nonStallIntr);
