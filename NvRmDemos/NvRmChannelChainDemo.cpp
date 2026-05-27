// NvRmChannelChainDemo
//
// Chains ten independent GPFIFO channels into a single GPU-side dependency
// graph using one shared semaphore — no CPU involvement between links. Each
// channel's host:
//   * (all but the first) ACQ_STRICT_GEQ-waits the shared semaphore for the
//     value released by the channel ahead of it, and
//   * releases the next value in the sequence once it runs.
//
// So channel 0 releases 1, channel 1 waits for >=1 then releases 2, ...,
// channel 9 waits for >=9 then releases 10. The acquires serialize the
// channels strictly on the GPU even though all ten submissions are queued
// up front with no waits between them; the only CPU wait is at the very end,
// blocking on an OS event — woken by a non-stall interrupt the tail channel
// raises after its release — until the shared semaphore reaches the final
// value (proof the last channel's release, and therefore the whole chain,
// has landed).
//
// The wrinkle is that every NvChannel owns a private VA space, so a single
// NvGpuMemory (which binds to one VA space) can't be the shared signal. The
// SharedSemaphore helper below allocates one physical sysmem block and maps it
// into all ten VA spaces, handing each channel the GPU address at which it
// sees the same bytes.

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include <vector>
#include <system_error>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
#include "nvos.h"
#include "class/cl003e.h" // NV01_MEMORY_SYSTEM
#include "class/cl50a0.h" // NV50_MEMORY_VIRTUAL
#include "class/cl2080_notification.h" // NV2080_NOTIFIERS_FIFO_EVENT_MTHD, NV2080_NOTIFIERS_RC_ERROR
}

#include <ErrorUtils.h>
#include <FileDesc.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvOsEvent.h"
#include "NvGpuMemory.h"
#include "NvChannel.h"
#include "NvCommandBuffer.h"


static const NvU32 kNumChannels = 10;


// One block of CPU+GPU-visible system memory mapped into several channel VA
// spaces at once, so a GPU semaphore released by a channel in one VA space can
// be acquired by a channel in another. NvGpuMemory binds a block to a single
// VA space; this binds one physical allocation into many, mirroring its
// teardown order (CPU map, then each DMA mapping, then the physical handle).
class SharedSemaphore {
private:
	NvRmDevice *fDev {};
	NvHandle fPhys {};
	NvU64 fSize {};
	NvRmMemoryMapping fCpuMap;
	std::vector<NvHandle> fVirt;    // one virtual alloc per VA space
	std::vector<NvU64> fGpuAddr;    // matching GPU VA per VA space

	void Reset()
	{
		if (fDev == nullptr) {
			return;
		}
		NvRmApi &rm = *fDev->Device().Rm();

		fCpuMap = NvRmMemoryMapping();
		for (std::size_t i = 0; i < fVirt.size(); i++) {
			rm.UnmapMemoryDma(fDev->Device().Get(), fVirt[i], fPhys, 0, fGpuAddr[i]);
			rm.Free(fVirt[i]);
		}
		if (fPhys != 0) {
			rm.Free(fPhys);
		}
		fVirt.clear();
		fGpuAddr.clear();
		fPhys = 0;
		fSize = 0;
		fDev = nullptr;
	}

public:
	SharedSemaphore() = default;
	~SharedSemaphore() { Reset(); }
	SharedSemaphore(const SharedSemaphore &) = delete;
	SharedSemaphore &operator=(const SharedSemaphore &) = delete;

	// Allocate `size` bytes of cached coherent sysmem and map it into every VA
	// space in `vaSpaces`, in order. GpuAddr(i) is the address visible to the
	// channel that owns vaSpaces[i].
	SharedSemaphore(NvRmDevice &dev, const std::vector<NvHandle> &vaSpaces, NvU64 size):
		fDev(&dev),
		fSize(size)
	{
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
			.alignment = 0x1000,
		};
		fPhys = rm.Alloc(NV01_MEMORY_SYSTEM, dev.Device().Get(), &physParams, sizeof(physParams));

		fCpuMap = dev.MapMemory(fPhys, true, 0, size, 0);
		memset((void*)fCpuMap.Address(), 0, size);

		for (NvHandle hVaSpace : vaSpaces) {
			NV_MEMORY_ALLOCATION_PARAMS virtParams {
				.owner = rm.Client().Get(),
				.type = NVOS32_TYPE_IMAGE,
				.flags = NVOS32_ALLOC_FLAGS_VIRTUAL | NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
				.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB),
				.size = size,
				.alignment = 0x1000,
				.hVASpace = hVaSpace,
			};
			NvHandle hVirt = rm.Alloc(NV50_MEMORY_VIRTUAL, dev.Device().Get(), &virtParams, sizeof(virtParams));

			NvU64 gpuAddr = 0;
			NvU32 mapFlags = FLD_SET_DRF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE, 0U);
			rm.MapMemoryDma(dev.Device().Get(), hVirt, fPhys, 0, size, mapFlags, gpuAddr);

			fVirt.push_back(hVirt);
			fGpuAddr.push_back(gpuAddr);
		}
	}

	volatile NvU64 *CpuPtr() const { return (volatile NvU64*)fCpuMap.Address(); }
	NvU64 GpuAddr(std::size_t i) const { return fGpuAddr[i]; }
};


int main()
{
	try {
		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		// Channels first, with their addresses pinned (reserve()) so the
		// command buffers — which hold a back-pointer to their channel — stay
		// valid as the vector fills.
		std::vector<NvChannel> channels;
		channels.reserve(kNumChannels);
		for (NvU32 i = 0; i < kNumChannels; i++) {
			channels.emplace_back(rmDev);
		}

		std::vector<NvCommandBuffer> cmdBufs;
		cmdBufs.reserve(kNumChannels);
		for (NvU32 i = 0; i < kNumChannels; i++) {
			cmdBufs.emplace_back(channels[i]);
		}

		std::vector<NvHandle> vaSpaces;
		vaSpaces.reserve(kNumChannels);
		for (NvChannel &ch : channels) {
			vaSpaces.push_back(ch.VaSpace());
		}
		SharedSemaphore sem(rmDev, vaSpaces, 0x1000);

		// Completion wakeup. The OS event is bound to the subdevice, so the
		// non-stall interrupt the tail channel raises after its release fires
		// the fd regardless of which channel raised it; the RC_ERROR binding
		// wakes poll() on a channel kill instead of riding out the timeout.
		NvOsEvent osEvent(rm);
		NvRmObject hCompletionEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_FIFO_EVENT_MTHD, /*nonStallIntr=*/true);
		NvRmObject hErrorEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_RC_ERROR,        /*nonStallIntr=*/false);

		printf("channels: %" PRIu32 "\n", kNumChannels);
		for (NvU32 i = 0; i < kNumChannels; i++) {
			printf("  ch %2" PRIu32 ": handle %#" PRIx32 "  sem gpu addr %#" PRIx64 "\n",
				i, channels[i].Handle(), sem.GpuAddr(i));
		}
		puts("");

		// Queue the whole chain back-to-back. Each link (except the head)
		// host-acquires the value its predecessor releases before releasing
		// its own, so the GPU serializes the ten channels strictly even though
		// the CPU never waits between submissions. WFI is off: there is no
		// engine work to drain, only host semaphore traffic. The tail channel
		// raises a non-stall interrupt after its release so the CPU wait below
		// is woken rather than polled.
		for (NvU32 i = 0; i < kNumChannels; i++) {
			if (i != 0) {
				WriteSemAcquire64(cmdBufs[i], sem.GpuAddr(i), i);
			}
			WriteSemRelease64(cmdBufs[i], sem.GpuAddr(i), i + 1, /*waitForIdle=*/false);
			if (i == kNumChannels - 1) {
				WriteNonStallInterrupt(cmdBufs[i]);
			}
			cmdBufs[i].SubmitTo(channels[i]);
		}

		printf("queued %" PRIu32 " chained submissions; waiting for the tail\n", kNumChannels);

		// The single CPU wait: block on the OS event until the shared
		// semaphore reaches the final value. poll() may wake early or
		// spuriously, so the in-memory value stays the authoritative
		// completion check; CheckError() on every channel each pass turns any
		// link faulting (which would strand every link behind it) into a
		// thrown exception rather than a silent hang.
		volatile NvU64 *semVal = sem.CpuPtr();
		while (*semVal < kNumChannels) {
			for (NvChannel &ch : channels) {
				ch.CheckError();
			}
			struct pollfd pfd {
				.fd = osEvent.Fd(),
				.events = POLLIN | POLLPRI,
			};
			int pr = poll(&pfd, 1, 1000);
			if (pr < 0) {
				CheckErrno(pr);
			}
		}

		printf("chain complete: semaphore = %" PRIu64 " (expected %" PRIu32 ")\n",
			(NvU64)*semVal, kNumChannels);

		// Drain each channel's host so its command buffers are safe to free.
		for (NvChannel &ch : channels) {
			ch.WaitIdle();
		}

		if (*semVal != kNumChannels) {
			fprintf(stderr, "[!] unexpected final semaphore value\n");
			return 2;
		}
		return 0;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmChannelChainDemo: %s\n", ex.what());
		return 1;
	}
}
