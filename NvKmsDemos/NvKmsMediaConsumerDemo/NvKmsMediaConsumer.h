#pragma once

#include <atomic>
#include <memory>

#include <Locker.h>
#include <OS.h>

#include <BufferConsumer.h>
#include <MediaDefs.h>
#include <MediaEventLooper.h>
#include <TimeSource.h>

extern "C" {
#include "nvtypes.h"
}


class NvKmsDisplay;
class GartBufferGroup;


// A Media Kit consumer node that:
//   - exposes a single B_MEDIA_RAW_VIDEO input,
//   - hands the upstream producer a BBufferGroup backed by NVRM GART
//     allocations (so the producer decodes straight into pages the GPU
//     can DMA from),
//   - on each delivered buffer, performs a GART -> VRAM copy via the
//     GPU copy engine and submits an NVKMS flip,
//   - and *also* acts as a BTimeSource, publishing performance time
//     derived from each flip-completion notifier's GPU PTIMER value
//     (1 ns/tick on Turing+).
class NvKmsMediaConsumer
	: public BMediaEventLooper
	, public BBufferConsumer
	, public BTimeSource {
public:
	NvKmsMediaConsumer(const char *name, NvKmsDisplay &display);
	virtual ~NvKmsMediaConsumer();

	NvKmsMediaConsumer(const NvKmsMediaConsumer &) = delete;
	NvKmsMediaConsumer &operator=(const NvKmsMediaConsumer &) = delete;

	// BMediaNode
	virtual BMediaAddOn *AddOn(int32 *_internalID) const;

protected:
	virtual void NodeRegistered();

	virtual void Start(bigtime_t performanceTime);
	virtual void Stop(bigtime_t performanceTime, bool immediate);
	virtual void Seek(bigtime_t mediaTime, bigtime_t performanceTime);
	virtual void TimeWarp(bigtime_t atRealTime, bigtime_t toPerformanceTime);
	virtual void SetRunMode(run_mode mode);
	virtual void SetTimeSource(BTimeSource *timeSource);

	virtual status_t HandleMessage(int32 message, const void *data, size_t size);

	// BBufferConsumer
	virtual status_t AcceptFormat(const media_destination &destination,
		media_format *format);
	virtual status_t GetNextInput(int32 *cookie, media_input *out_input);
	virtual void DisposeInputCookie(int32 cookie);
	virtual void BufferReceived(BBuffer *buffer);
	virtual void ProducerDataStatus(const media_destination &for_whom,
		int32 status, bigtime_t at_media_time);
	virtual status_t GetLatencyFor(const media_destination &for_whom,
		bigtime_t *out_latency, media_node_id *out_timesource);
	virtual status_t Connected(const media_source &producer,
		const media_destination &where, const media_format &with_format,
		media_input *out_input);
	virtual void Disconnected(const media_source &producer,
		const media_destination &where);
	virtual status_t FormatChanged(const media_source &producer,
		const media_destination &consumer, int32 change_tag,
		const media_format &format);

	// BMediaEventLooper
	virtual void HandleEvent(const media_timed_event *event,
		bigtime_t lateness, bool realTimeEvent);

	// BTimeSource
	virtual status_t TimeSourceOp(const time_source_op_info &op, void *_reserved);

private:
	NvKmsDisplay &fDisplay;

	media_input fInput;
	media_destination fDestination;
	// Budget for the GART -> VRAM blit; presentation latency is this plus one
	// refresh interval. Phase 2's present thread can refine it with a measured
	// blit duration.
	bigtime_t fBlitBudgetUs;
	// Presentation latency: set as the event latency (how early the looper
	// dispatches HandleEvent) and reported via GetLatencyFor. The two must
	// match so the producer's lead time and our processing budget agree.
	bigtime_t fLatency;
	bool fConnected;

	std::unique_ptr<GartBufferGroup> fBuffers;

	// Resolved at Connected() from the negotiated media_format — drive
	// HandleEvent's BlitAndFlip call.
	uint32 fSrcWidth;
	uint32 fSrcHeight;
	uint32 fSrcBytesPerRow;

	// Tracks whether the next FLIP_OCCURRED corresponds to the very first
	// flip post-SetMode (in which case there is no "displaced" previous
	// flip and the notifier timestamp should not be published).
	bool fFirstFlipSeen;
	// Offset added to (ptimer_ns / 1000) so that performance time stays
	// continuous across the (synthetic) START anchor and the first
	// notifier-driven publish. Computed once when the first valid PTIMER
	// timestamp arrives.
	bool fPtimerOffsetSet;
	int64 fPtimerToPerfOffsetUs;
	// Touched by both the present thread (publish path) and the control thread
	// (TimeSourceOp START/STOP/SEEK); atomic to avoid torn cross-thread access.
	std::atomic<bigtime_t> fLastPerformancePublishedUs;
	std::atomic<bigtime_t> fLastRealPublishedUs;

	// Present thread: the GART -> VRAM blit and the flip-completion poll both
	// block on the GPU. Running them here keeps the BMediaEventLooper control
	// thread free to dispatch events and receive buffers on schedule. The
	// handoff is a single slot with replace-newest (show-latest) semantics so
	// a slow GPU never builds a backlog. Spawned per connection.
	thread_id fPresentThread;
	sem_id fPresentSem;
	BLocker fPresentLock;
	BBuffer *fPendingBuffer;
	NvU64 fPendingGpuAddr;
	bool fPresentQuit;

	void StartPresentThread();
	void StopPresentThread();
	void PresentFrame(NvU64 srcGpuAddr);
	static int32 PresentThreadEntry(void *data);
	int32 PresentThread();
};
