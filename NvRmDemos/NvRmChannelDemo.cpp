// NvRmChannelDemo
//
// Allocates a GPFIFO channel, then in a loop:
//   * builds a pushbuffer that does a 64-bit semaphore release writing an
//     incrementing value into a small system-memory block, followed by a
//     non-stall interrupt;
//   * submits it via the channel;
//   * waits for completion by poll()ing an OS-event fd attached to a
//     subdevice NV01_EVENT_OS_EVENT bound to NV2080_NOTIFIERS_FIFO_EVENT_MTHD;
//   * reads the value the GPU just wrote and prints it.

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


int main()
{
	try {
		// Keep the device node open for the lifetime of the test: it backs
		// the per-process RM state inspected by various ioctls.
		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		NvChannel channel(rmDev);
		NvCommandBuffer cmdBuf(channel);

		// Data block the GPU writes into via the semaphore release.
		NvGpuMemory data;
		data.AllocSystem(rmDev, channel.VaSpace(), 0x1000, 0x1000, true);
		auto dataValue = (volatile NvU64*)data.CpuAddr();
		*dataValue = 0;

		// OS event fd that becomes pollable when either subdevice notifier
		// fires. Both events share the fd (kernel refcounts the
		// registration); a single poll() wakes on either.
		NvOsEvent osEvent(rm);
		NvRmObject hCompletionEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_FIFO_EVENT_MTHD, /*nonStallIntr=*/true);
		NvRmObject hErrorEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_RC_ERROR,        /*nonStallIntr=*/false);

		printf("channel:       %#" PRIx32 "\n", channel.Handle());
		printf("data gpu addr: %#" PRIx64 "\n", data.GpuAddr());

		const NvU64 iterations = 10;
		for (NvU64 wSeq = 1; wSeq <= iterations; wSeq++) {
			WriteSemRelease64(cmdBuf, data.GpuAddr(), wSeq, /*waitForIdle=*/true);
			WriteNonStallInterrupt(cmdBuf);

			cmdBuf.SubmitTo(channel);

			// Wait for the non-stall interrupt to wake the OS event, then
			// re-check the value in memory. poll() may return early or
			// spuriously; the semaphore value in `*dataValue` is the
			// authoritative completion signal. CheckError() on each pass
			// turns a channel kill into a thrown exception rather than a
			// silent hang.
			while (*dataValue != wSeq) {
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

			printf("iter %3" PRIu64 ": *data = %" PRIu64 "\n", wSeq, (NvU64)*dataValue);
		}

		return 0;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmChannelDemo: %s\n", ex.what());
		return 1;
	}
}
