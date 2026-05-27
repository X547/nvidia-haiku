#include "NvKmsMediaConsumer.h"

#include <stdio.h>
#include <string.h>

#include <Autolock.h>
#include <Buffer.h>
#include <BufferGroup.h>
#include <MediaRoster.h>
#include <TimedEventQueue.h>
#include <scheduler.h>

#include "NvKmsDisplay.h"
#include "GartBufferGroup.h"


// Reject frames that arrive more than JITTER late, matching CodyCam.
static const bigtime_t kJitterUs = 20000;
// Initial budget for the GART -> VRAM blit, used to size the presentation
// latency until Phase 2's present thread can feed back a measured value.
// ~1 frame at 60 Hz; the real cost is the copy of one scaled frame.
static const bigtime_t kInitialBlitBudgetUs = 16000;


NvKmsMediaConsumer::NvKmsMediaConsumer(const char *name, NvKmsDisplay &display)
	: BMediaNode(name)
	, BMediaEventLooper()
	, BBufferConsumer(B_MEDIA_RAW_VIDEO)
	, BTimeSource()
	, fDisplay(display)
	, fBlitBudgetUs(kInitialBlitBudgetUs)
	, fLatency(0)
	, fConnected(false)
	, fSrcWidth(0)
	, fSrcHeight(0)
	, fSrcBytesPerRow(0)
	, fFirstFlipSeen(false)
	, fPtimerOffsetSet(false)
	, fPtimerToPerfOffsetUs(0)
	, fLastPerformancePublishedUs(0)
	, fLastRealPublishedUs(0)
	, fPresentThread(-1)
	, fPresentSem(-1)
	, fPresentLock("NvKmsMediaConsumer present")
	, fPendingBuffer(nullptr)
	, fPendingGpuAddr(0)
	, fPresentQuit(false)
{
	AddNodeKind(B_PHYSICAL_OUTPUT);

	// Presentation latency = blit budget + one refresh interval. The looper
	// dispatches HandleEvent this far before a buffer's start_time, giving us
	// time to blit and submit a flip that latches at the target vsync; the
	// same value is reported upstream (GetLatencyFor) so the producer delivers
	// frames early enough. EventLatency() == reported latency is the contract.
	NvU32 rateMilliHz = fDisplay.RefreshRateMilliHz();
	bigtime_t refreshUs = rateMilliHz > 0 ? 1000000000LL / rateMilliHz : 16667;
	fLatency = fBlitBudgetUs + refreshUs;

	SetEventLatency(fLatency);
	SetPriority(B_REAL_TIME_PRIORITY);
}


NvKmsMediaConsumer::~NvKmsMediaConsumer()
{
	// Stop the control thread first so no further buffers are handed off,
	// then drain the present thread before our members (fBuffers) destruct.
	Quit();
	StopPresentThread();
}


BMediaAddOn *
NvKmsMediaConsumer::AddOn(int32 *_internalID) const
{
	if (_internalID != nullptr) {
		*_internalID = 0;
	}
	return nullptr;
}


void NvKmsMediaConsumer::NodeRegistered()
{
	fInput.destination.port = ControlPort();
	fInput.destination.id = 0;
	fInput.source = media_source::null;
	fInput.node = Node();
	// Advertise B_RGB32 as the only required pin; width/height/stride and
	// field_rate stay wildcard so the producer drives them from the clip's
	// natural resolution. The GPU 2D engine does the scaling at flip time.
	fInput.format.type = B_MEDIA_RAW_VIDEO;
	fInput.format.u.raw_video = media_raw_video_format::wildcard;
	fInput.format.u.raw_video.display.format = B_RGB32;
	snprintf(fInput.name, B_MEDIA_NAME_LENGTH, "NVKMS scanout");

	Run();
}


void NvKmsMediaConsumer::Start(bigtime_t performanceTime)
{
	BMediaEventLooper::Start(performanceTime);
}

void NvKmsMediaConsumer::Stop(bigtime_t performanceTime, bool immediate)
{
	BMediaEventLooper::Stop(performanceTime, immediate);
}

void NvKmsMediaConsumer::Seek(bigtime_t mediaTime, bigtime_t performanceTime)
{
	BMediaEventLooper::Seek(mediaTime, performanceTime);
}

void NvKmsMediaConsumer::TimeWarp(bigtime_t atRealTime, bigtime_t toPerformanceTime)
{
	BMediaEventLooper::TimeWarp(atRealTime, toPerformanceTime);
}

void NvKmsMediaConsumer::SetRunMode(run_mode mode)
{
	BMediaEventLooper::SetRunMode(mode);
}

void NvKmsMediaConsumer::SetTimeSource(BTimeSource *timeSource)
{
	// empty
}


status_t
NvKmsMediaConsumer::HandleMessage(int32 message, const void *data, size_t size)
{
	if (BBufferConsumer::HandleMessage(message, data, size) == B_OK) {
		return B_OK;
	}
	if (BMediaEventLooper::HandleMessage(message, data, size) == B_OK) {
		return B_OK;
	}
	if (BTimeSource::HandleMessage(message, data, size) == B_OK) {
		return B_OK;
	}
	return BMediaNode::HandleMessage(message, data, size);
}


status_t
NvKmsMediaConsumer::AcceptFormat(const media_destination &destination, media_format *format)
{
	if (destination != fInput.destination) {
		return B_MEDIA_BAD_DESTINATION;
	}
	if (format->type == B_MEDIA_NO_TYPE) {
		format->type = B_MEDIA_RAW_VIDEO;
	}
	if (format->type != B_MEDIA_RAW_VIDEO) {
		return B_MEDIA_BAD_FORMAT;
	}

	media_raw_video_format &rv = format->u.raw_video;

	if (rv.display.format != B_RGB32
		&& rv.display.format != media_raw_video_format::wildcard.display.format) {
		fprintf(stderr, "NvKmsMediaConsumer::AcceptFormat: rejecting format %d "
			"(only B_RGB32 supported)\n", (int)rv.display.format);
		return B_MEDIA_BAD_FORMAT;
	}
	rv.display.format = B_RGB32;

	// Accept whatever resolution the producer offers — the 2D engine
	// will resize at scanout time. Leave width/height/stride alone here;
	// the producer pins them in PrepareToConnect.
	return B_OK;
}


status_t
NvKmsMediaConsumer::GetNextInput(int32 *cookie, media_input *out_input)
{
	if (*cookie >= 1) {
		return B_MEDIA_BAD_DESTINATION;
	}
	*out_input = fInput;
	(*cookie)++;
	return B_OK;
}


void NvKmsMediaConsumer::DisposeInputCookie(int32)
{
}


void NvKmsMediaConsumer::BufferReceived(BBuffer *buffer)
{
	if (RunState() == B_STOPPED) {
		buffer->Recycle();
		return;
	}
	media_timed_event event(buffer->Header()->start_time,
		BTimedEventQueue::B_HANDLE_BUFFER, buffer,
		BTimedEventQueue::B_RECYCLE_BUFFER);
	EventQueue()->AddEvent(event);
}


void NvKmsMediaConsumer::ProducerDataStatus(const media_destination &for_whom,
	int32 status, bigtime_t at_media_time)
{
	(void)status;
	(void)at_media_time;
	if (for_whom != fInput.destination) {
		return;
	}
}


status_t
NvKmsMediaConsumer::GetLatencyFor(const media_destination &for_whom,
	bigtime_t *out_latency, media_node_id *out_timesource)
{
	if (for_whom != fInput.destination) {
		return B_MEDIA_BAD_DESTINATION;
	}
	*out_latency = fLatency;
	*out_timesource = TimeSource()->ID();
	return B_OK;
}


status_t
NvKmsMediaConsumer::Connected(const media_source &producer,
	const media_destination &where, const media_format &with_format,
	media_input *out_input)
{
	if (where != fInput.destination) {
		return B_MEDIA_BAD_DESTINATION;
	}
	if (with_format.type != B_MEDIA_RAW_VIDEO) {
		return B_MEDIA_BAD_FORMAT;
	}

	fInput.source = producer;
	fInput.format = with_format;
	fInput.node = Node();
	fDestination = where;

	// Now that the producer has pinned its format, build GART buffers sized
	// to the *clip*, not the display. Sizing here (rather than at construct
	// time) keeps the consumer agnostic of source resolution.
	const media_raw_video_format &rv = with_format.u.raw_video;
	fSrcWidth = rv.display.line_width;
	fSrcHeight = rv.display.line_count;
	fSrcBytesPerRow = rv.display.bytes_per_row;
	if (fSrcWidth == 0 || fSrcHeight == 0 || fSrcBytesPerRow == 0) {
		fprintf(stderr, "NvKmsMediaConsumer::Connected: producer left "
			"width/height/stride wildcard (%u x %u, stride %u)\n",
			(unsigned)fSrcWidth, (unsigned)fSrcHeight, (unsigned)fSrcBytesPerRow);
		return B_MEDIA_BAD_FORMAT;
	}
	if ((NvU64)fSrcBytesPerRow < (NvU64)fSrcWidth * 4) {
		fprintf(stderr, "NvKmsMediaConsumer::Connected: stride %u shorter "
			"than width %u * 4\n",
			(unsigned)fSrcBytesPerRow, (unsigned)fSrcWidth);
		return B_MEDIA_BAD_FORMAT;
	}
	printf("NvKmsMediaConsumer: connected at %u x %u (stride %u) — "
		"display is %u x %u, blit will scale\n",
		(unsigned)fSrcWidth, (unsigned)fSrcHeight, (unsigned)fSrcBytesPerRow,
		(unsigned)fDisplay.Width(), (unsigned)fDisplay.Height());

	try {
		// Depth must cover the producer's lead (it now delivers
		// EventLatency-ahead, ~3-4 frames) plus the one being blitted and the
		// one pending in the present slot, or RequestBuffer starves and the
		// producer skips frames. Tune against the measured lead if needed.
		fBuffers = std::make_unique<GartBufferGroup>(
			fDisplay.RmDevice(), fDisplay.BlitChannel(),
			fSrcWidth, fSrcHeight, fSrcBytesPerRow, /*count=*/5);
	} catch (const std::exception &ex) {
		fprintf(stderr, "NvKmsMediaConsumer::Connected: GartBufferGroup failed: %s\n", ex.what());
		return B_NO_MEMORY;
	}

	uint32 userData = 0;
	int32 changeTag = 1;
	status_t ret = SetOutputBuffersFor(producer, fDestination, fBuffers->Group(),
		&userData, &changeTag, /*willReclaim=*/true);
	if (ret != B_OK) {
		fprintf(stderr, "NvKmsMediaConsumer::Connected: SetOutputBuffersFor failed: %s\n",
			strerror(ret));
	}

	fConnected = true;
	fFirstFlipSeen = false;

	// Buffers exist now — start the present thread so HandleEvent can hand
	// frames off without blocking on the GPU.
	StartPresentThread();

	*out_input = fInput;
	return B_OK;
}


void NvKmsMediaConsumer::Disconnected(const media_source &producer,
	const media_destination &where)
{
	if (where == fInput.destination && producer == fInput.source) {
		fInput.source = media_source::null;
		fConnected = false;
		// Drain and join the present thread before tearing down the buffer
		// group: it may hold a buffer to recycle and reads fBuffers' VA space.
		StopPresentThread();
		fBuffers.reset();
	}
}


status_t
NvKmsMediaConsumer::FormatChanged(const media_source &producer,
	const media_destination &consumer, int32 change_tag,
	const media_format &format)
{
	(void)producer;
	(void)consumer;
	(void)change_tag;
	(void)format;
	// Re-negotiating format mid-stream is not supported by this demo —
	// the buffer group is sized for the picked display mode. Reject so
	// the producer disconnects rather than silently breaking.
	return B_ERROR;
}


void NvKmsMediaConsumer::HandleEvent(const media_timed_event *event,
	bigtime_t lateness, bool realTimeEvent)
{
	(void)lateness;
	(void)realTimeEvent;

	switch (event->type) {
	case BTimedEventQueue::B_START:
		break;
	case BTimedEventQueue::B_STOP:
		EventQueue()->FlushEvents(event->event_time,
			BTimedEventQueue::B_ALWAYS, true,
			BTimedEventQueue::B_HANDLE_BUFFER);
		break;
	case BTimedEventQueue::B_HANDLE_BUFFER: {
		BBuffer *buffer = static_cast<BBuffer *>(event->pointer);
		if (RunState() != B_STARTED || !fConnected || fBuffers == nullptr) {
			buffer->Recycle();
			break;
		}
		if (!fBuffers->Owns(buffer->ID())) {
			// Producer ignored our SetOutputBuffersFor and is using its
			// own buffers — we can't GPU-DMA from a memory we don't own
			// in our copy channel's VA space, so drop the frame.
			static bool warned = false;
			if (!warned) {
				fprintf(stderr, "NvKmsMediaConsumer: producer is not using "
					"our GART buffer group; frames will be dropped\n");
				warned = true;
			}
			buffer->Recycle();
			break;
		}

		// Drop late frames unless we are in offline mode.
		if (RunMode() != B_OFFLINE) {
			bigtime_t now = TimeSource()->Now();
			if (now > buffer->Header()->start_time + kJitterUs) {
				fprintf(stderr, "NvKmsMediaConsumer: dropping late frame "
					"(%" B_PRId64 " us late, start_time %" B_PRId64 ")\n",
					now - buffer->Header()->start_time,
					buffer->Header()->start_time);
				buffer->Recycle();
				break;
			}
		}

		// Hand the frame to the present thread instead of blitting here:
		// BlitAndFlip blocks on the GPU copy + flip-notifier poll, and doing
		// that on the control thread would stall event dispatch and buffer
		// reception. Single slot, replace-newest — if a frame is already
		// pending the GPU is behind, so we drop the older one (show-latest).
		NvU64 srcGpuAddr = fBuffers->GpuAddrFor(buffer->ID());
		bool signal = false;
		{
			BAutolock _(fPresentLock);
			if (fPendingBuffer != nullptr) {
				// A frame is still queued for the GPU — it can't keep up, so
				// drop the older one and keep the newest (show-latest).
				fprintf(stderr, "NvKmsMediaConsumer: GPU behind, dropping "
					"queued frame (show-latest)\n");
				fPendingBuffer->Recycle();
			} else {
				signal = true;
			}
			fPendingBuffer = buffer;
			fPendingGpuAddr = srcGpuAddr;
		}
		// Only release on an empty -> non-empty transition so the semaphore
		// count can't grow past the one pending slot.
		if (signal && fPresentSem >= B_OK) {
			release_sem(fPresentSem);
		}
		break;
	}
	default:
		break;
	}
}


status_t
NvKmsMediaConsumer::TimeSourceOp(const time_source_op_info &op, void *)
{
	switch (op.op) {
	case B_TIMESOURCE_START: {
		// Slaves need *some* published (performance, real) anchor before
		// they can compute their schedule, otherwise their PerformanceTimeFor
		// call returns garbage. Mirror SystemTimeSource: publish the current
		// real time as performance time at drift = 1.0. Subsequent flip
		// notifier events will replace this with PTIMER-derived values.
		bigtime_t real = system_time();
		PublishTime(real, real, 1.0f);
		fLastPerformancePublishedUs = (NvU64)real;
		fLastRealPublishedUs = real;
		break;
	}
	case B_TIMESOURCE_STOP:
	case B_TIMESOURCE_STOP_IMMEDIATELY:
		// Freeze slaves at the last published performance time by
		// publishing with drift = 0.
		PublishTime(fLastPerformancePublishedUs.load(),
			system_time(), 0.0f);
		break;
	case B_TIMESOURCE_SEEK:
		BroadcastTimeWarp(op.real_time, op.performance_time);
		fLastPerformancePublishedUs = (NvU64)op.performance_time;
		fLastRealPublishedUs = op.real_time;
		break;
	}
	return B_OK;
}


void NvKmsMediaConsumer::StartPresentThread()
{
	if (fPresentThread >= B_OK) {
		return; // already running
	}
	fPresentQuit = false;
	fPendingBuffer = nullptr;
	fPendingGpuAddr = 0;

	fPresentSem = create_sem(0, "NvKmsMediaConsumer present");
	if (fPresentSem < B_OK) {
		fprintf(stderr, "NvKmsMediaConsumer::StartPresentThread: create_sem failed: %s\n",
			strerror(fPresentSem));
		fPresentSem = -1;
		return;
	}
	fPresentThread = spawn_thread(PresentThreadEntry, "NvKmsMediaConsumer present",
		B_REAL_TIME_PRIORITY, this);
	if (fPresentThread < B_OK) {
		fprintf(stderr, "NvKmsMediaConsumer::StartPresentThread: spawn_thread failed\n");
		delete_sem(fPresentSem);
		fPresentSem = -1;
		fPresentThread = -1;
		return;
	}
	resume_thread(fPresentThread);
}


void NvKmsMediaConsumer::StopPresentThread()
{
	if (fPresentThread < B_OK) {
		return;
	}
	// Signal quit and wake the thread out of acquire_sem().
	{
		BAutolock _(fPresentLock);
		fPresentQuit = true;
	}
	release_sem(fPresentSem);

	status_t exitVal;
	wait_for_thread(fPresentThread, &exitVal);
	fPresentThread = -1;

	delete_sem(fPresentSem);
	fPresentSem = -1;

	// Recycle a frame the thread never got to (handed off but not grabbed).
	BAutolock _(fPresentLock);
	if (fPendingBuffer != nullptr) {
		fPendingBuffer->Recycle();
		fPendingBuffer = nullptr;
	}
}


int32 NvKmsMediaConsumer::PresentThreadEntry(void *data)
{
	return static_cast<NvKmsMediaConsumer *>(data)->PresentThread();
}


int32 NvKmsMediaConsumer::PresentThread()
{
	while (true) {
		status_t err = acquire_sem(fPresentSem);
		if (err != B_OK) {
			break; // sem deleted
		}
		if (fPresentQuit) {
			break;
		}

		BBuffer *buffer = nullptr;
		NvU64 srcGpuAddr = 0;
		{
			BAutolock _(fPresentLock);
			buffer = fPendingBuffer;
			srcGpuAddr = fPendingGpuAddr;
			fPendingBuffer = nullptr;
		}
		if (buffer == nullptr) {
			// Coalesced/spurious wakeup — nothing pending.
			continue;
		}

		PresentFrame(srcGpuAddr);
		buffer->Recycle();
	}
	return 0;
}


void NvKmsMediaConsumer::PresentFrame(NvU64 srcGpuAddr)
{
	try {
		fDisplay.BlitAndFlip(srcGpuAddr,
			fSrcWidth, fSrcHeight, fSrcBytesPerRow);
	} catch (const std::exception &ex) {
		fprintf(stderr, "NvKmsMediaConsumer: BlitAndFlip failed: %s\n", ex.what());
		return;
	}

	// Pull the just-completed flip's PTIMER timestamp. The first flip after
	// SetMode has no preceding flip to displace, so FLIP_OCCURRED won't fire
	// for it; the publish simply starts once events begin landing.
	NvU64 ptimerNs = 0;
	// 0 ms = non-blocking drain; we don't want to stall waiting for the
	// kernel-side queue even on this thread.
	bool got = fDisplay.PollFlipNotifierTimestamp(ptimerNs, 0);
	if (got) {
		int64 ptimerUs = (int64)(ptimerNs / 1000);
		bigtime_t realUs = system_time();
		if (!fPtimerOffsetSet) {
			// Anchor the GPU PTIMER clock to system_time at the first flip.
			// The producer's frame performance times are anchored to
			// system_time too, so keeping our published timeline in that same
			// domain is what makes RealTimeFor() map frames to the right
			// wall-clock instants.
			//
			// Do NOT anchor to fLastPerformancePublishedUs: main calls
			// SeekTimeSource(consumer, 0, ...) right after StartTimeSource,
			// which resets that value to 0 via B_TIMESOURCE_SEEK. Anchoring to
			// 0 makes perf trail real by ~system_time(), so RealTimeFor()
			// returns wall-clock times ~hundreds of seconds in the future and
			// the producer's frame thread sleeps forever (freeze).
			fPtimerToPerfOffsetUs = realUs - ptimerUs;
			fPtimerOffsetSet = true;
		}
		// Performance time advances by GPU PTIMER deltas (vsync-locked),
		// anchored to system_time at the first flip.
		bigtime_t perfUs = (bigtime_t)(ptimerUs + fPtimerToPerfOffsetUs);
		PublishTime(perfUs, realUs, 1.0f);
		fLastPerformancePublishedUs = (NvU64)perfUs;
		fLastRealPublishedUs = realUs;
		fFirstFlipSeen = true;
	}
}


