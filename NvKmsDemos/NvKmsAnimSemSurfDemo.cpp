// NvKmsAnimSemSurfDemo
//
// Variant of NvKmsAnimDemo that drives flip completion through the per-layer
// release semaphore instead of NVKMS_EVENT_TYPE_FLIP_OCCURRED.
//
// NV_SEMAPHORE_SURFACE waiter scans only fire when something raises a GPU
// interrupt RM treats as a "scan waiters now" signal — and the display
// engine doesn't have an interface for that. So a CPU waiter registered on
// a slot the display engine writes is inert; the slot reaches the target
// value, but no wakeup ever lands on the fd.
//
// The bridge channel pattern below works around that. One sysmem block
// backs:
//   * an NVKMS NISO surface (display engine writes release.value into it
//     when the flip is displaced),
//   * an NV_SEMAPHORE_SURFACE (so RegisterWaiter can attach an OS event
//     fd to a target value on a slot), and
//   * a GPU mapping in a lightweight copy-engine channel's VA space.
//
// Per flip k the channel pushes:
//     SEM_ACQUIRE_GEQ slot=k+1
//     SEM_RELEASE     slot=k+1   ; no-op write, but emits a non-stall IRQ
//     NON_STALL_INTERRUPT
// When the display engine writes k+1 into the slot, the host SEM_ACQUIRE
// unblocks, the SEM_RELEASE retires, and the non-stall fires. RM then
// scans the channel's bound semsurfs and wakes the fd. That gives us a
// real interrupt-driven CPU wakeup tied to flip displacement, without
// relying on the completion-notifier event path.
//
// Requires NvKmsAllocDeviceReply::supportsIndependentAcqRelSemaphore so
// that NVKMS accepts a release-only configuration (acquire.surface == 0).

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>

extern "C" {
#include "nvkms-ioctl.h"
#include "nvkms-api.h"
#include "nv-haiku.h"

#include "nvos.h"
#include "class/cl003e.h"   // NV01_MEMORY_SYSTEM
#include "class/cl00da.h"   // NV_SEMAPHORE_SURFACE
#include "class/cl50a0.h"   // NV50_MEMORY_VIRTUAL
#include "class/cl2080_notification.h" // NV2080_NOTIFIERS_FIFO_EVENT_MTHD
#include "ctrl/ctrl00da.h"  // NV_SEMAPHORE_SURFACE_CTRL_CMD_*
}

#include "ErrorUtils.h"
#include "FileDesc.h"

#include "NvRmApi.h"
#include "NvRmDevice.h"

#include "NvKmsApi.h"
#include "NvKmsDevice.h"
#include "NvKmsSurface.h"

#include <nvrm-helpers/NvOsEvent.h>
#include <nvrm-helpers/NvChannel.h>
#include <nvrm-helpers/NvCommandBuffer.h>

#include "NvUtils.h"
#include "NvKmsBitmap.h"


struct ColorRgba {
	NvU8 r;
	NvU8 g;
	NvU8 b;
	NvU8 a;
};


static const NvKmsNIsoFormat kSemFormat = NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY;
static const NvU64 kSemMemSize = 4096;
static const NvU32 kNumBuffers = 2;


// Bundles together everything the bridge pattern needs to talk about a
// single semaphore slot:
//   * the underlying sysmem (NVKMS-NISO-tagged so the display engine's
//     write lands coherently),
//   * a GPU VA mapping into the bridge channel's address space (so host
//     SEM_ACQUIRE/RELEASE can target it),
//   * an NV_SEMAPHORE_SURFACE (so RegisterWaiter ties an fd to a slot
//     value), and
//   * the NvKmsSurface registration (so NVKMS can program
//     SET_CONTEXT_DMA_SEMAPHORE on the display channel).
//
// We use a single slot at offset 0 — the bridge writes the same value the
// display engine wrote, so a one-slot configuration is enough.
class BridgeSemSurf {
private:
	NvRmDevice *fRmDev {};
	NvKmsApi *fKms {};
	NvKmsDeviceHandle fKmsDeviceHandle {};
	NvU64 fSize {};
	NvHandle fHVaSpace {};

	NvRmObject fPhysMem;
	NvRmMemoryMapping fCpuMap;
	NvHandle fVirtMem {};
	NvU64 fGpuAddr {};
	NvRmObject fSemSurf;
	NvKmsSurface fKmsSurface;

	void TearDownVaMapping()
	{
		if (fRmDev == nullptr) {
			return;
		}
		NvRmApi &rm = *fRmDev->Device().Rm();
		if (fGpuAddr != 0) {
			rm.UnmapMemoryDma(fRmDev->Device().Get(), fVirtMem, fPhysMem.Get(), 0, fGpuAddr);
			fGpuAddr = 0;
		}
		if (fVirtMem != 0) {
			rm.Free(fVirtMem);
			fVirtMem = 0;
		}
	}

public:
	BridgeSemSurf() = default;
	BridgeSemSurf(const BridgeSemSurf &) = delete;
	BridgeSemSurf &operator=(const BridgeSemSurf &) = delete;

	BridgeSemSurf(BridgeSemSurf &&other) noexcept:
		fRmDev(other.fRmDev),
		fKms(other.fKms),
		fKmsDeviceHandle(other.fKmsDeviceHandle),
		fSize(other.fSize),
		fHVaSpace(other.fHVaSpace),
		fPhysMem(std::move(other.fPhysMem)),
		fCpuMap(std::move(other.fCpuMap)),
		fVirtMem(other.fVirtMem),
		fGpuAddr(other.fGpuAddr),
		fSemSurf(std::move(other.fSemSurf)),
		fKmsSurface(std::move(other.fKmsSurface))
	{
		other.fRmDev = nullptr;
		other.fKms = nullptr;
		other.fVirtMem = 0;
		other.fGpuAddr = 0;
	}

	BridgeSemSurf &operator=(BridgeSemSurf &&other) noexcept
	{
		if (this != &other) {
			TearDownVaMapping();
			fRmDev = other.fRmDev;
			fKms = other.fKms;
			fKmsDeviceHandle = other.fKmsDeviceHandle;
			fSize = other.fSize;
			fHVaSpace = other.fHVaSpace;
			fPhysMem = std::move(other.fPhysMem);
			fCpuMap = std::move(other.fCpuMap);
			fVirtMem = other.fVirtMem;
			fGpuAddr = other.fGpuAddr;
			fSemSurf = std::move(other.fSemSurf);
			fKmsSurface = std::move(other.fKmsSurface);
			other.fRmDev = nullptr;
			other.fKms = nullptr;
			other.fVirtMem = 0;
			other.fGpuAddr = 0;
		}
		return *this;
	}

	~BridgeSemSurf() { TearDownVaMapping(); }

	BridgeSemSurf(NvRmDevice &rmDev, NvKmsApi &kms, NvKmsDevice &kmsDev, NvHandle hVaSpace):
		fRmDev(&rmDev),
		fKms(&kms),
		fKmsDeviceHandle(kmsDev.Get()),
		fSize(kSemMemSize),
		fHVaSpace(hVaSpace)
	{
		// 1. Sysmem with NISO_DISPLAY attrs (so the display engine's
		//    release write lands in a CPU-readable form).
		NvU8 compressible = 0;
		nvKmsKapiAllocateSystemMemory(rmDev, kmsDev, fPhysMem,
			NvKmsSurfaceMemoryLayoutPitch, fSize,
			NVKMS_KAPI_ALLOCATION_TYPE_NOTIFIER, &compressible);

		fCpuMap = rmDev.MapMemory(fPhysMem.Get(), true, 0, fSize, 0);

		// Zero the slot so the first display-engine release (value 1) is a
		// clean monotonic step.
		memset(fCpuMap.Address(), 0, (size_t)fSize);

		// 2. Bind the same memory into the channel's VA space. Bridge
		//    channel methods will hit it via GpuAddr() below.
		NvRmApi &rm = *rmDev.Device().Rm();
		NV_MEMORY_ALLOCATION_PARAMS virtParams {
			.owner = rm.Client().Get(),
			.type = NVOS32_TYPE_IMAGE,
			.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
			.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB),
			.size = fSize,
			.alignment = 0x1000,
			.hVASpace = fHVaSpace,
		};
		fVirtMem = rm.Alloc(NV50_MEMORY_VIRTUAL, rmDev.Device().Get(), &virtParams, sizeof(virtParams));

		NvU32 mapFlags = FLD_SET_DRF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE, 0U);
		rm.MapMemoryDma(rmDev.Device().Get(), fVirtMem, fPhysMem.Get(), 0, fSize, mapFlags, fGpuAddr);

		// 3. RM-side semsurf wrapping the same memory.
		NV_SEMAPHORE_SURFACE_ALLOC_PARAMETERS semParams {
			.hSemaphoreMem = fPhysMem.Get(),
		};
		fSemSurf = rmDev.Subdevice().Alloc(NV_SEMAPHORE_SURFACE, &semParams, sizeof(semParams));

		// 4. NVKMS NISO registration so SET_CONTEXT_DMA_SEMAPHORE has
		//    something to point at.
		FileDesc memoryFd = rmDev.ExportObjectToFd(rmDev.Device().Get(), fPhysMem.Get());

		NvKmsRegisterSurfaceParams params {};
		params.request.deviceHandle = fKmsDeviceHandle;
		params.request.useFd = true;
		params.request.planes[0].u.fd = memoryFd.Get();
		params.request.planes[0].pitch = (NvU32)fSize;
		params.request.planes[0].rmObjectSizeInBytes = fSize;
		params.request.widthInPixels = (NvU32)fSize;
		params.request.heightInPixels = 1;
		params.request.layout = NvKmsSurfaceMemoryLayoutPitch;
		params.request.format = NvKmsSurfaceMemoryFormatI8;
		params.request.isoType = NVKMS_MEMORY_NISO;
		fKms->Control(NVKMS_IOCTL_REGISTER_SURFACE, &params, sizeof(params));
		fKmsSurface = NvKmsSurface(kmsDev, params.reply.surfaceHandle);
	}

	NvHandle SemSurfHandle() const { return fSemSurf.Get(); }
	NvKmsSurfaceHandle KmsSurfaceHandle() const { return fKmsSurface.Get(); }
	NvU32 SlotOffsetInWords() const { return 0; }
	NvU64 GpuAddr() const { return fGpuAddr; }

	// 32-bit read at slot 0. Display HW writes a 32-bit release value;
	// upper 32 bits remain 0 from the initial memset, so a plain 32-bit
	// load is the right read for both Is64Bit and legacy slot semantics
	// at the values this demo uses (counters far below 2^32).
	NvU32 ReadValue() const
	{
		volatile NvU32 *p = (volatile NvU32 *)fCpuMap.Address();
		return __atomic_load_n(p, __ATOMIC_ACQUIRE);
	}

	void RegisterWaiter(NvU64 waitValue, NvU64 notificationHandle)
	{
		NV_SEMAPHORE_SURFACE_CTRL_REGISTER_WAITER_PARAMS p {
			.index = 0,
			.waitValue = waitValue,
			.newValue = 0,
			.notificationHandle = notificationHandle,
		};
		fSemSurf.Control(NV_SEMAPHORE_SURFACE_CTRL_CMD_REGISTER_WAITER, &p, sizeof(p));
	}

	void UnregisterWaiter(NvU64 waitValue, NvU64 notificationHandle)
	{
		NV_SEMAPHORE_SURFACE_CTRL_UNREGISTER_WAITER_PARAMS p {
			.index = 0,
			.waitValue = waitValue,
			.notificationHandle = notificationHandle,
		};
		fSemSurf.Control(NV_SEMAPHORE_SURFACE_CTRL_CMD_UNREGISTER_WAITER, &p, sizeof(p));
	}

	void BindChannel(NvHandle hChannel)
	{
		NV_SEMAPHORE_SURFACE_CTRL_BIND_CHANNEL_PARAMS p {
			.hChannel = hChannel,
			.numNotifyIndices = 1,
		};
		p.notifyIndices[0] = NV2080_NOTIFIERS_FIFO_EVENT_MTHD;
		fSemSurf.Control(NV_SEMAPHORE_SURFACE_CTRL_CMD_BIND_CHANNEL, &p, sizeof(p));
	}
};


class NvKmsAnimSemSurfDemo {
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

	// Bridge channel: cheap copy-DMA engine. The only thing it ever does
	// is host SEM_ACQUIRE / SEM_RELEASE / NON_STALL, so no engine-class
	// binding is required. Channel must be declared before fSemSurf so
	// its VA space is alive when fSemSurf maps into it.
	NvChannel fChannel;
	NvCommandBuffer fCmdBuf;
	BridgeSemSurf fSemSurf;
	NvOsEvent fOsEvent;

	NvKmsBitmap fFramebuffer[kNumBuffers];
	NvU32 fQueuedBuffer;
	NvU32 fPhase;
	// Monotonic flip index. SetMode is 0, each Flip ioctl is the next int.
	NvU64 fFlipCounter;

	bool QueryConnected(NVDpyId dpyId);
	bool QueryPreferredMode(NVDpyId dpyId, NvKmsMode &outMode);
	bool PickConnectedDpy(NVDpyId &outDpyId, NvKmsMode &outMode);

	void FillReleaseSemaphore(NvKmsChannelSyncObjects &sync, NvU64 releaseValue);
	void RenderFrame(NvKmsBitmap &bitmap, NvU32 phase);
	void SubmitFlip(NvU32 buffer);
	void PushBridge(NvU64 value);
	void WaitForFlipDisplaced(NvU64 waitValue);

public:
	NvKmsAnimSemSurfDemo();

	void SetupDisplay();
	void RunEventLoop();
};


NvKmsAnimSemSurfDemo::NvKmsAnimSemSurfDemo():
	fDevFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC)),
	fRmDev(fRm, 0),
	fKmsDev(fKms, 0),
	fHead(0),
	fWidth(0),
	fHeight(0),
	// COPY0 is the lightest engine that still routes host SEM_*/non-stall
	// methods through the GPFIFO scheduler — graphics would also work but
	// allocates a 3D context for no reason.
	fChannel(fRmDev, /*engineType=*/NV2080_ENGINE_TYPE_COPY0),
	fCmdBuf(fChannel),
	fOsEvent(fRm),
	fQueuedBuffer(0),
	fPhase(0),
	fFlipCounter(0)
{
	CheckErrno(fDevFd.Get());

	if (fKmsDev.Info().numDisps == 0) {
		RaiseErrno(ENODEV);
	}
	fDeviceHandle = fKmsDev.Info().deviceHandle;
	fDispHandle = fKmsDev.Info().dispHandles[0];

	// With supportsIndependentAcqRelSemaphore = FALSE the C37E HAL asserts
	// acquireSurface == releaseSurface and uses ACQ_MODE _EQ, which makes
	// any acquire.value the demo could pick deadlock after the first flip.
	// Bail out rather than ship a misleading demo on legacy HW.
	if (!fKmsDev.Info().supportsIndependentAcqRelSemaphore) {
		fprintf(stderr, "[!] supportsIndependentAcqRelSemaphore = FALSE on this device\n");
		RaiseErrno(ENOTSUP);
	}

	{
		NvKmsGrabOwnershipParams params {};
		params.request.deviceHandle = fDeviceHandle;
		fKms.Control(NVKMS_IOCTL_GRAB_OWNERSHIP, &params, sizeof(params));
	}

	fSemSurf = BridgeSemSurf(fRmDev, fKms, fKmsDev, fChannel.VaSpace());

	// Tell RM to scan this semsurf's waiters whenever the channel raises
	// FIFO_EVENT_MTHD (i.e. the non-stall interrupt the bridge batch
	// emits). Without this the registered waiter never fires.
	fSemSurf.BindChannel(fChannel.Handle());
}


bool NvKmsAnimSemSurfDemo::QueryConnected(NVDpyId dpyId)
{
	NvKmsQueryDpyDynamicDataParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.dispHandle = fDispHandle;
	params.request.dpyId = dpyId;
	fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params));
	return params.reply.connected;
}

bool NvKmsAnimSemSurfDemo::QueryPreferredMode(NVDpyId dpyId, NvKmsMode &outMode)
{
	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = fDeviceHandle;
		params.request.dispHandle = fDispHandle;
		params.request.dpyId = dpyId;
		params.request.modeIndex = i;
		fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params));
		if (params.reply.end) {
			break;
		}
		if (params.reply.mode.timings.hVisible == 2560 && params.reply.mode.timings.vVisible == 1600 && params.reply.mode.timings.RRx1k == 119999) {
			outMode = params.reply.mode;
			return true;
		}
	}

	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = fDeviceHandle;
		params.request.dispHandle = fDispHandle;
		params.request.dpyId = dpyId;
		params.request.modeIndex = i;
		fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params));
		if (params.reply.end) {
			return false;
		}
		if (params.reply.preferredMode) {
			outMode = params.reply.mode;
			return true;
		}
	}
}

bool NvKmsAnimSemSurfDemo::PickConnectedDpy(NVDpyId &outDpyId, NvKmsMode &outMode)
{
	NVDpyIdList validDpys;
	{
		NvKmsQueryDispParams params {};
		params.request.deviceHandle = fDeviceHandle;
		params.request.dispHandle = fDispHandle;
		fKms.Control(NVKMS_IOCTL_QUERY_DISP, &params, sizeof(params));
		validDpys = params.reply.validDpys;
	}

	NVDpyId cur = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(), validDpys);
	while (!nvDpyIdIsInvalid(cur)) {
		if (QueryConnected(cur) && QueryPreferredMode(cur, outMode)) {
			outDpyId = cur;
			return true;
		}
		cur = nvNextDpyIdInDpyIdListUnsorted(cur, validDpys);
	}
	return false;
}


void NvKmsAnimSemSurfDemo::FillReleaseSemaphore(NvKmsChannelSyncObjects &sync, NvU64 releaseValue)
{
	sync.useSyncpt = false;
	sync.u.semaphores.acquire = {};
	sync.u.semaphores.release.surface.surfaceHandle = fSemSurf.KmsSurfaceHandle();
	sync.u.semaphores.release.surface.format = kSemFormat;
	sync.u.semaphores.release.surface.offsetInWords = fSemSurf.SlotOffsetInWords();
	sync.u.semaphores.release.value = (NvU32)releaseValue;
}


void NvKmsAnimSemSurfDemo::RenderFrame(NvKmsBitmap &bitmap, NvU32 phase)
{
	auto line = (NvU8*)bitmap.Bits();
	NvU32 stride = bitmap.BytesPerRow();
	for (NvU32 y = 0; y < bitmap.Height(); y++) {
		auto color = (ColorRgba*)line;
		for (NvU32 x = 0; x < bitmap.Width(); x++) {
			color->r = (NvU8)((x + phase) & 0xff);
			color->g = (NvU8)((y + phase) & 0xff);
			color->b = 0x80;
			color->a = 0xff;
			color++;
		}
		line += stride;
	}
}


void NvKmsAnimSemSurfDemo::SetupDisplay()
{
	NvKmsMode mode {};
	if (!PickConnectedDpy(fDpyId, mode)) {
		fprintf(stderr, "[!] no connected dpy with a preferred mode\n");
		RaiseErrno(ENODEV);
	}
	fHead = 0;
	fWidth = mode.timings.hVisible;
	fHeight = mode.timings.vVisible;
	printf("dpyId %" PRIu32 ", head %" PRIu32 ", mode %s (%" PRIu32 "x%" PRIu32 " @ %" PRIu32 ".%03" PRIu32 " Hz)\n",
		nvDpyIdToIndex(fDpyId), fHead, mode.name, fWidth, fHeight,
		mode.timings.RRx1k / 1000, mode.timings.RRx1k % 1000);

	for (NvU32 i = 0; i < kNumBuffers; i++) {
		fFramebuffer[i] = NvKmsBitmap(fRmDev, fKmsDev, fWidth, fHeight, B_RGB32);
	}

	fPhase = 0;
	RenderFrame(fFramebuffer[0], fPhase);

	NvKmsSetModeParams params {};
	params.request.deviceHandle = fDeviceHandle;
	params.request.commit = true;
	params.request.requestedDispsBitMask = 1U << 0;
	params.request.disp[0].requestedHeadsBitMask = 1U << fHead;

	auto &headReq = params.request.disp[0].head[fHead];
	headReq.dpyIdList = nvAddDpyIdToEmptyDpyIdList(fDpyId);
	headReq.mode = mode;
	headReq.viewPortOut = {.x = 0, .y = 0, .width = fWidth, .height = fHeight};
	headReq.viewPortSizeIn = {.width = fWidth, .height = fHeight};

	auto &layer = headReq.flip.layer[NVKMS_MAIN_LAYER];
	layer.surface.handle[0] = fFramebuffer[0].Surface().Get();
	layer.surface.specified = true;
	layer.sizeIn.val = {.width = fWidth, .height = fHeight};
	layer.sizeIn.specified = true;
	layer.sizeOut.val = {.width = fWidth, .height = fHeight};
	layer.sizeOut.specified = true;

	// SetMode is flip 0; its release.value lands in the slot when the
	// first Flip ioctl displaces it.
	FillReleaseSemaphore(layer.syncObjects.val, /*releaseValue=*/fFlipCounter + 1);
	layer.syncObjects.specified = true;
	fFlipCounter++;

	try {
		fKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params));
	} catch (const std::system_error &ex) {
		printf("[!] SetMode failed: %s\n", ex.what());
		printf("    status=%d disp[0].status=%d head[%" PRIu32 "].status=%d\n",
			params.reply.status,
			params.reply.disp[0].status,
			fHead,
			params.reply.disp[0].head[fHead].status);
		throw;
	}
}


void NvKmsAnimSemSurfDemo::SubmitFlip(NvU32 buffer)
{
	NvKmsFlipRequestOneHead flipHead {};
	auto &layer = flipHead.flip.layer[NVKMS_MAIN_LAYER];
	layer.surface.handle[0] = fFramebuffer[buffer].Surface().Get();
	layer.surface.specified = true;
	FillReleaseSemaphore(layer.syncObjects.val, /*releaseValue=*/fFlipCounter + 1);
	layer.syncObjects.specified = true;

	NvKmsFlipParams flipParams {};
	flipParams.request.deviceHandle = fDeviceHandle;
	flipParams.request.pFlipHead = nvKmsPointerToNvU64(&flipHead);
	flipParams.request.numFlipHeads = 1;
	flipParams.request.commit = true;
	fKms.Control(NVKMS_IOCTL_FLIP, &flipParams, sizeof(flipParams));

	fQueuedBuffer = buffer;
	fFlipCounter++;
}


void NvKmsAnimSemSurfDemo::PushBridge(NvU64 value)
{
	// Host SEM_ACQUIRE_STRICT_GEQ blocks the channel until the slot
	// reaches `value`; the display engine's release for the previous flip
	// is what eventually writes that value. The release that follows is a
	// no-op write (slot already at `value`) but still triggers a non-stall
	// interrupt via the next method.
	const NvU64 gpuAddr = fSemSurf.GpuAddr();
	if (value <= 0xFFFFFFFFull) {
		WriteSemAcquire32(fCmdBuf, gpuAddr, (NvU32)value);
		WriteSemRelease32(fCmdBuf, gpuAddr, (NvU32)value, /*waitForIdle=*/false);
	} else {
		WriteSemAcquire64(fCmdBuf, gpuAddr, value);
		WriteSemRelease64(fCmdBuf, gpuAddr, value, /*waitForIdle=*/false);
	}
	WriteNonStallInterrupt(fCmdBuf);
	fCmdBuf.SubmitTo(fChannel);
}


void NvKmsAnimSemSurfDemo::WaitForFlipDisplaced(NvU64 waitValue)
{
	fSemSurf.RegisterWaiter(waitValue, (NvU64)fOsEvent.Fd());

	// TODO: call fSemSurf.UnregisterWaiter on error

	// Robust against:
	//   * spurious poll() wakeups (recheck the slot),
	//   * the slot already being at/past waitValue at register time
	//     (RegisterWaiter fires immediately on registration in that case),
	//   * a dropped non-stall (the 1s timeout caps the worst-case latency
	//     without making it the steady-state rate). A bridge channel kill
	//     would also keep the slot below waitValue forever, so CheckError
	//     surfaces that as an exception instead of an indefinite hang.
	while (fSemSurf.ReadValue() < waitValue) {
		fChannel.CheckError();
		struct pollfd pfd {
			.fd = fOsEvent.Fd(),
			.events = POLLIN | POLLPRI,
		};
		int pr = poll(&pfd, 1, 1000);
		if (pr < 0) {
			CheckErrno(pr);
		}
	}
}


void NvKmsAnimSemSurfDemo::RunEventLoop()
{
	for (;;) {
		NvU32 nextBuffer = (fQueuedBuffer + 1) % kNumBuffers;
		fPhase = (fPhase + 1) & 0xff;
		RenderFrame(fFramebuffer[nextBuffer], fPhase);

		// fFlipCounter is the index this flip will receive; after it's
		// submitted with release.value = fFlipCounter + 1, the slot will
		// reach fFlipCounter + 1 only when the *next* flip displaces this
		// one. To know when this flip itself is on-screen, we wait for
		// the slot to reach the previous flip's release.value, which is
		// fFlipCounter (this flip's index).
		const NvU64 waitValue = fFlipCounter;
		SubmitFlip(nextBuffer);
		PushBridge(waitValue);
		WaitForFlipDisplaced(waitValue);
	}
}


int main()
{
	try {
		NvKmsAnimSemSurfDemo demo;
		demo.SetupDisplay();
		demo.RunEventLoop();
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvKmsAnimSemSurfDemo: %s\n", ex.what());
		return 1;
	}
	return 0;
}
