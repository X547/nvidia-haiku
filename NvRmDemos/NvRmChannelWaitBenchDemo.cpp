// NvRmChannelWaitBenchDemo
//
// Throughput micro-benchmark of the two per-submission CPU-wait paths this
// repo supports, run back-to-back on identical work so the numbers are
// directly comparable:
//
//   * event wait   — a subdevice NV01_EVENT_OS_EVENT bound to
//     NV2080_NOTIFIERS_FIFO_EVENT_MTHD. The channel raises a non-stall
//     interrupt; RM wakes the fd unconditionally and the CPU re-checks the
//     in-memory semaphore value to confirm completion (the NvRmChannelDemo
//     path).
//   * semsurf wait — an NV_SEMAPHORE_SURFACE with a value-targeted
//     RegisterWaiter. RM only fires the fd once the slot reaches the
//     registered wait value, so the wakeup is already filtered (the
//     NvRmChannelSemSurfDemo path).
//
// Each method submits a single WFI semaphore release + non-stall interrupt,
// then blocks the CPU until that one submission completes, one million times.
// Because exactly one submission is ever in flight, the loop time is dominated
// by submit + interrupt-delivery + wakeup latency — which is precisely what
// separates the two wait paths. The result is reported as submissions/sec.

#include <fcntl.h>
#include <unistd.h>
#include <poll.h>

#include <stdio.h>
#include <inttypes.h>

#include <chrono>
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


// One million submissions per method, each with its own CPU wait. Large
// enough that per-iteration latency dominates startup/teardown costs.
static const NvU64 kIterations = 1000000;

// poll() guard timeout. A correctly delivered wakeup arrives well within
// this; the timeout only bounds the loop if a wakeup is somehow missed, in
// which case the re-checked in-memory value still drives completion.
static const int kPollTimeoutMs = 1000;


// Emit a progress line to stderr (kept off stdout so the result table stays
// clean) every 10% so a multi-minute run shows it is alive. Negligible cost
// against the poll()/submit work it sits beside.
static void ReportProgress(const char *label, NvU64 done, NvU64 total)
{
	if (done % (total / 10) == 0) {
		fprintf(stderr, "\r  %s: %3" PRIu64 "%%", label, done * 100 / total);
		fflush(stderr);
		if (done == total) {
			fputc('\n', stderr);
		}
	}
}


// Event-wait path: subdevice OS event on the non-stall interrupt notifier.
// RM wakes the fd on every non-stall interrupt, so the loop re-checks the
// in-memory value to confirm this submission's release has landed. The shared
// `osEvent` already carries the caller's RC_ERROR binding; this path adds the
// FIFO_EVENT_MTHD completion binding (non-stall, so it touches no subdevice
// notifyActions state and is safe to allocate and free per run).
static double BenchEventWait(NvRmDevice &rmDev, NvOsEvent &osEvent, NvU64 iterations)
{
	NvChannel channel(rmDev);
	NvCommandBuffer cmdBuf(channel);

	NvGpuMemory data;
	data.AllocSystem(rmDev, channel.VaSpace(), 0x1000, 0x1000, /*mapCpu=*/true);
	auto dataValue = (volatile NvU64*)data.CpuAddr();
	*dataValue = 0;

	NvRmObject hCompletionEvent = AllocSubdeviceOsEvent(
		rmDev, osEvent, NV2080_NOTIFIERS_FIFO_EVENT_MTHD, /*nonStallIntr=*/true);

	auto start = std::chrono::steady_clock::now();
	for (NvU64 wSeq = 1; wSeq <= iterations; wSeq++) {
		WriteSemRelease64(cmdBuf, data.GpuAddr(), wSeq, /*waitForIdle=*/true);
		WriteNonStallInterrupt(cmdBuf);

		cmdBuf.SubmitTo(channel);

		while (*dataValue < wSeq) {
			channel.CheckError();
			struct pollfd pfd {
				.fd = osEvent.Fd(),
				.events = POLLIN | POLLPRI,
			};
			int pr = poll(&pfd, 1, kPollTimeoutMs);
			if (pr < 0) {
				CheckErrno(pr);
			}
		}

		ReportProgress("event  ", wSeq, iterations);
	}
	auto end = std::chrono::steady_clock::now();

	double seconds = std::chrono::duration<double>(end - start).count();
	return (double)iterations / seconds;
}


// Semsurf-wait path: value-targeted RegisterWaiter on slot 0. RM only fires
// the fd once the slot reaches the registered wait value, so the wakeup is
// already specific to this submission; the in-memory ReadValue() is still the
// authoritative guard against an early/spurious return.
static double BenchSemSurfWait(NvRmDevice &rmDev, NvOsEvent &osEvent, NvU64 iterations)
{
	NvChannel channel(rmDev);
	NvCommandBuffer cmdBuf(channel);

	NvSemSurf semSurf(rmDev, channel.VaSpace(), 1);
	semSurf.BindChannel(channel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});

	// RegisterWaiter wakes the fd on completion; the caller's shared RC_ERROR
	// binding makes a channel kill wake poll() too instead of riding out the
	// timeout. No FIFO_EVENT_MTHD subdevice OS event here, so the only thing
	// firing the fd on the happy path is the value-targeted waiter.

	auto start = std::chrono::steady_clock::now();
	for (NvU64 wSeq = 1; wSeq <= iterations; wSeq++) {
		semSurf.RecordSubmission(0, wSeq);
		semSurf.RegisterWaiter(0, wSeq, 0, (NvU64)osEvent.Fd());

		semSurf.WriteRelease(cmdBuf, 0, wSeq, /*waitForIdle=*/true);
		WriteNonStallInterrupt(cmdBuf);

		cmdBuf.SubmitTo(channel);

		while (semSurf.ReadValue(0) < wSeq) {
			channel.CheckError();
			struct pollfd pfd {
				.fd = osEvent.Fd(),
				.events = POLLIN | POLLPRI,
			};
			int pr = poll(&pfd, 1, kPollTimeoutMs);
			if (pr < 0) {
				CheckErrno(pr);
			}
		}

		ReportProgress("semsurf", wSeq, iterations);
	}
	auto end = std::chrono::steady_clock::now();

	semSurf.UnbindChannel(channel.Handle(), {NV2080_NOTIFIERS_FIFO_EVENT_MTHD});

	double seconds = std::chrono::duration<double>(end - start).count();
	return (double)iterations / seconds;
}


int main()
{
	try {
		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		// One OS event fd and one RC_ERROR binding shared across both runs.
		// Arming the subdevice's RC_ERROR notify action is one-shot per
		// subdevice — re-arming an already-armed action returns
		// NV_ERR_INVALID_STATE — so it must be set up once here rather than per
		// benchmark. Each benchmark adds only its own completion binding.
		NvOsEvent osEvent(rm);
		NvRmObject hErrorEvent = AllocSubdeviceOsEvent(
			rmDev, osEvent, NV2080_NOTIFIERS_RC_ERROR, /*nonStallIntr=*/false);

		printf("submissions per method: %" PRIu64 "\n\n", kIterations);

		double eventRate   = BenchEventWait(rmDev, osEvent, kIterations);
		double semsurfRate = BenchSemSurfWait(rmDev, osEvent, kIterations);

		puts("");
		printf("event wait:    %12.0f submissions/sec\n", eventRate);
		printf("semsurf wait:  %12.0f submissions/sec\n", semsurfRate);
		if (eventRate > 0.0) {
			printf("semsurf/event: %12.2fx\n", semsurfRate / eventRate);
		}
		return 0;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmChannelWaitBenchDemo: %s\n", ex.what());
		return 1;
	}
}
