// NvRmChannelErrorDemo
//
// Intentionally faults a GPFIFO channel and exercises the error-handling
// contract added to NvChannel:
//
//   1. Allocates a channel and binds two NV01_EVENT_OS_EVENT objects to
//      the same osEvent fd:
//        * NV2080_NOTIFIERS_FIFO_EVENT_MTHD  - normal non-stall interrupt
//          (same wakeup used by NvRmChannelDemo)
//        * NV2080_NOTIFIERS_RC_ERROR         - robust-channel kill wakeup
//      Both events write to the same pipe; one poll() wakes on either.
//
//   2. Submits a known-good push buffer and waits for the sem release, to
//      confirm the channel is alive before we break it.
//
//   3. Submits a push buffer whose semaphore release targets an unmapped
//      GPU VA. The host engine MMU-faults on the write, which lands as a
//      PBDMA exception and RM tears the channel down (typically
//      ROBUST_CHANNEL_FIFO_ERROR_MMU_ERR_FLT). poll() then wakes on the
//      RC_ERROR event.
//
//   4. Calls channel.CheckError(), which throws std::system_error carrying
//      the NV_STATUS and a decoded ROBUST_CHANNEL_* type / engine id from
//      the channel's error notifier.
//
//   5. Confirms a follow-up Submit() refuses to run because Submit() calls
//      CheckError() at its top; we catch the throw and report success.
//
// A successful run exits 0 once both the RC catch and the dead-channel
// guard fire. Any other path returns non-zero.

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
		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		NvChannel channel(rmDev);
		NvCommandBuffer cmdBuf(channel);

		NvGpuMemory data;
		data.AllocSystem(rmDev, channel.VaSpace(), 0x1000, 0x1000, true);
		auto dataValue = (volatile NvU64*)data.CpuAddr();
		*dataValue = 0;

		NvOsEvent osEvent(rm);
		NvRmObject hCompletionEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_FIFO_EVENT_MTHD, /*nonStallIntr=*/true);
		NvRmObject hErrorEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_RC_ERROR,        /*nonStallIntr=*/false);

		printf("channel:       %#" PRIx32 "\n", channel.Handle());
		printf("data gpu addr: %#" PRIx64 "\n", data.GpuAddr());

		// ---- Phase 1: sanity submission ----
		{
			WriteSemRelease64(cmdBuf, data.GpuAddr(), 1, /*waitForIdle=*/true);
			WriteNonStallInterrupt(cmdBuf);

			cmdBuf.SubmitTo(channel);

			while (*dataValue != 1) {
				channel.CheckError();
				struct pollfd pfd { .fd = osEvent.Fd(), .events = POLLIN | POLLPRI };
				int pr = poll(&pfd, 1, 1000);
				if (pr < 0) {
					CheckErrno(pr);
				}
			}
			printf("[ok ] sanity submit completed: *data = %" PRIu64 "\n",
				(NvU64)*dataValue);
		}

		// ---- Phase 2: faulting submission ----
		//
		// 0xdead'0000'0000 lives in the upper bits, well above anything
		// FERMI_VASPACE_A hands out, so the channel's VA space has no PTE
		// for it. The host engine traps trying to write the semaphore.
		const NvU64 unmappedGpuAddr = 0xdead000000ull;
		{
			WriteSemRelease64(cmdBuf, unmappedGpuAddr, 0xbad, /*waitForIdle=*/true);
			WriteNonStallInterrupt(cmdBuf);

			cmdBuf.SubmitTo(channel);
			printf("[--] faulting push buffer submitted (sem release to %#" PRIx64
				"), waiting for RC...\n", unmappedGpuAddr);
		}

		// ---- Phase 3: wait for RC, then decode ----
		const int kMaxWaitSeconds = 5;
		bool sawError = false;
		for (int attempt = 0; attempt < kMaxWaitSeconds && !sawError; attempt++) {
			if (channel.HasError()) {
				sawError = true;
				break;
			}
			struct pollfd pfd { .fd = osEvent.Fd(), .events = POLLIN | POLLPRI };
			int pr = poll(&pfd, 1, 1000);
			if (pr < 0) {
				CheckErrno(pr);
			}
		}

		if (!sawError) {
			fprintf(stderr, "[!] no RC error observed within %d seconds\n", kMaxWaitSeconds);
			return 2;
		}

		try {
			channel.CheckError();
			fprintf(stderr, "[!] HasError() said yes but CheckError() did not throw\n");
			return 3;
		} catch (const std::system_error &ex) {
			printf("[ok ] caught expected RC: %s\n", ex.what());
		}

		// ---- Phase 4: confirm dead channel refuses further work ----
		try {
			WriteSemRelease64(cmdBuf, data.GpuAddr(), 2, /*waitForIdle=*/true);
			WriteNonStallInterrupt(cmdBuf);
			cmdBuf.SubmitTo(channel);
			fprintf(stderr, "[!] Submit() on dead channel unexpectedly succeeded\n");
			return 4;
		} catch (const std::system_error &ex) {
			printf("[ok ] follow-up Submit() refused: %s\n", ex.what());
		}

		return 0;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmChannelErrorDemo: %s\n", ex.what());
		return 1;
	}
}
