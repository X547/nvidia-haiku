#pragma once

#include <cstdint>

extern "C" {
#include "nvtypes.h"
#include "nvkms-api.h"
}

#include "FileDesc.h"

#include "NvRmApi.h"
#include "NvRmDevice.h"

#include "NvKmsApi.h"
#include "NvKmsDevice.h"
#include "NvKmsSurface.h"

#include <nvrm-helpers/NvOsEvent.h>
#include <nvrm-helpers/NvChannel.h>
#include <nvrm-helpers/NvCommandBuffer.h>
#include <nvrm-helpers/NvSemSurf.h>

#include "NvKmsBitmap.h"


// Owns the NVRM/NVKMS device pair, the copy-engine channel used for
// GART -> VRAM transfers, the double-buffered VRAM scanout surfaces, and the
// per-flip completion notifier slots. Construction opens the devices and
// grabs NVKMS ownership; SetupDisplay() picks a dpy/mode and performs the
// initial SetMode. Destruction releases ownership.
class NvKmsDisplay {
private:
	FileDesc fDevFd;
	NvRmApi fRm;
	NvRmDevice fRmDev;
	NvKmsApi fKms;
	NvKmsDevice fKmsDev;

	NvKmsDeviceHandle fDeviceHandle;
	NvKmsDispHandle fDispHandle;
	NVDpyId fDpyId;
	NvU32 fHead;
	NvU32 fWidth;
	NvU32 fHeight;
	NvU32 fRefreshRateMilliHz;

	// Graphics-engine channel: needed to drive FERMI_TWOD_A. Both the
	// 2D blits (GART -> VRAM scale) and any future engine-class work
	// (copy, compute) ride this same channel via SET_OBJECT.
	NvChannel fChannel;
	NvCommandBuffer fCmdBuf;
	NvRmObject fTwodObj;

	NvSemSurf fBlitSem;
	NvOsEvent fBlitEvent;
	NvRmObject fBlitErrorEvent;
	NvU64 fBlitSeq;

	NvRmObject fNotifierMem;
	NvRmMemoryMapping fNotifierMap;
	NvKmsSurface fNotifierSurface;

	NvKmsBitmap fFramebuffer[2];
	NvRmObject fFramebufferVirt[2];
	NvU64 fFramebufferGpuAddr[2];
	NvU32 fQueuedBuffer;
	NvU64 fFlipCounter;
	bool fOwnershipHeld;

	bool QueryConnected(NVDpyId dpyId);
	bool QueryPreferredMode(NVDpyId dpyId, NvKmsMode &outMode);
	bool PickConnectedDpy(NVDpyId &outDpyId, NvKmsMode &outMode);

	void AllocateNotifier();
	void FillFlipCompletionNotifier(NvKmsCompletionNotifierDescription &cn, NvU32 slot);
	void SubmitInitialSetMode(const NvKmsMode &mode);

	void SubmitFlip(NvU32 buffer);

public:
	NvKmsDisplay();
	~NvKmsDisplay();
	NvKmsDisplay(const NvKmsDisplay &) = delete;
	NvKmsDisplay &operator=(const NvKmsDisplay &) = delete;

	// Pick a connected dpy + its preferred mode, allocate VRAM framebuffers
	// matching the mode, and submit the initial SetMode. After this returns,
	// Flip() can be called.
	void SetupDisplay();

	NvU32 Width() const {return fWidth;}
	NvU32 Height() const {return fHeight;}
	NvU32 RefreshRateMilliHz() const {return fRefreshRateMilliHz;}
	int KmsFd() const {return fKms.Fd();}

	// Exposed so GartBufferGroup can allocate GART blocks in the same
	// channel VA space the 2D engine reads from.
	NvRmDevice &RmDevice() {return fRmDev;}
	NvChannel &BlitChannel() {return fChannel;}

	// Scale-copy an A8R8G8B8 source of `(srcWidth x srcHeight)` from GART
	// at `srcGpuAddr`/`srcPitch` into the next VRAM scanout via
	// FERMI_TWOD_A bilinear blit. The blit is centered, preserving aspect
	// ratio — display pixels outside the active rect keep whatever they
	// held last (zero-initialized in SetupDisplay), producing letterbox
	// or pillarbox bars. Blocks on blit completion, then submits an
	// NVKMS flip for the just-written buffer.
	void BlitAndFlip(NvU64 srcGpuAddr, NvU32 srcWidth, NvU32 srcHeight, NvU32 srcPitch);

	// Block on (or fail-fast poll) the kms fd for a FLIP_OCCURRED event.
	// On success returns true and writes the PTIMER timestamp (in ns) of
	// the flip that just completed into outPtimerNs. Returns false if no
	// event was available within `timeoutMs` milliseconds (0 == poll only).
	bool PollFlipNotifierTimestamp(NvU64 &outPtimerNs, int timeoutMs);
};
