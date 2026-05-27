// NvRmChannelSemSurfDemo
//
// Same shape as NvRmChannelDemo, but the completion path uses an
// NV_SEMAPHORE_SURFACE instead of an NV01_EVENT_OS_EVENT attached to a
// subdevice notifier. For each iteration:
//   * register the OS event fd as a waiter on slot 0 of the semsurf with a
//     target value of `wSeq`;
//   * submit a pushbuffer that does a 64-bit semaphore release writing
//     `wSeq` into the semsurf slot, followed by a non-stall interrupt;
//   * poll() the fd until RM signals it once the slot reaches `wSeq`.
//
// Unlike the subdevice-OS-event path, the wakeup is value-targeted: RM
// only fires the fd when the registered waitValue is reached, so the poll
// loop doesn't need to re-check the in-memory value to filter spurious
// wakeups.

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <stdio.h>
#include <inttypes.h>

#include <system_error>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
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
#include "NvSemSurf.h"


int main()
{
	try {
		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		NvChannel channel(rmDev);
		NvCommandBuffer cmdBuf(channel);

		NvSemSurf semSurf(rmDev, channel.VaSpace(), 1);

		// The channel signals the semsurf slot via SEM_RELEASE + non-stall
		// interrupt, so it needs to be bound with the FIFO_EVENT_MTHD
		// notifier index — that's the one RM scans on each non-stall to
		// look for registered waiters whose wait value has been reached.
		semSurf.BindChannel(channel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});

		NvOsEvent osEvent(rm);
		// The semsurf path uses RegisterWaiter to wake the fd on the
		// completion side; we add an RC_ERROR binding so a channel kill
		// also wakes poll() instead of waiting on the 1s timeout.
		NvRmObject hErrorEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_RC_ERROR, /*nonStallIntr=*/false);

		printf("channel:         %#" PRIx32 "\n", channel.Handle());
		printf("semsurf:         %#" PRIx32 "\n", semSurf.Handle());
		printf("semsurf gpu adr: %#" PRIx64 "\n", semSurf.GpuAddr(0));
		printf("layout.size:     %" PRIu64 "\n", (NvU64)semSurf.Layout().size);
		printf("layout.caps:     %#" PRIx32 " (64bit=%d, monfence=%d)\n",
			(NvU32)semSurf.Layout().caps,
			(int)!!(semSurf.Layout().caps & NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_CAPS_64BIT_SEMAPHORES_SUPPORTED),
			(int)!!(semSurf.Layout().caps & NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_CAPS_MONITORED_FENCE_SUPPORTED));

		const NvU64 iterations = 10;
		for (NvU64 wSeq = 1; wSeq <= iterations; wSeq++) {
			// On non-64-bit hardware, publish the upper half of the
			// target value CPU-side before either the kernel or the GPU
			// can observe a state where the on-GPU 32-bit value has
			// advanced past the previous max-submitted. Must happen
			// before RegisterWaiter and before Submit.
			semSurf.RecordSubmission(0, wSeq);

			// Register the waiter next so it's in place before the GPU
			// could possibly reach the target value.
			semSurf.RegisterWaiter(0, wSeq, 0, (NvU64)osEvent.Fd());

			semSurf.WriteRelease(cmdBuf, 0, wSeq, /*waitForIdle=*/true);
			WriteNonStallInterrupt(cmdBuf);

			cmdBuf.SubmitTo(channel);

			// poll() may still return early; the authoritative completion
			// signal remains the synthesized semaphore value. CheckError()
			// on each pass turns a channel kill into a thrown exception
			// rather than a silent hang (a dead channel never reaches the
			// release, so ReadValue() would stay below wSeq forever).
			while (semSurf.ReadValue(0) < wSeq) {
				channel.CheckError();
				struct pollfd pfd {
					.fd = osEvent.Fd(),
					.events = POLLIN | POLLPRI,
				};
				int pr = poll(&pfd, 1, 1000);
				if (pr < 0) {
					CheckErrno(pr);
				}
			}

			printf("iter %3" PRIu64 ": sem = %" PRIu64 "\n", wSeq, (NvU64)semSurf.ReadValue(0));
		}

		semSurf.UnbindChannel(channel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});

		return 0;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmChannelSemSurfDemo: %s\n", ex.what());
		return 1;
	}
}
