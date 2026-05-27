#include "MediaFileProducer.h"

#include <stdio.h>
#include <string.h>

#include <Autolock.h>
#include <Buffer.h>
#include <BufferGroup.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <TimedEventQueue.h>
#include <TimeSource.h>


// Per-frame extra latency budget added on top of the downstream FindLatencyFor
// response. Conservatively large — software decode of a real video can spike
// well past steady-state. Tune once measurements exist.
static const bigtime_t kDecodeLatencyHintUs = 30000;

// If the frame thread falls this far behind its schedule — at startup, where
// first-keyframe decode latency leaves the B_START-anchored timeline stale, or
// after a mid-stream decode stall — re-anchor the timeline to "now" instead of
// burst-delivering a backlog the consumer would only drop as late. No decoded
// frames are skipped; we simply stop chasing a stale schedule.
static const bigtime_t kResyncThresholdUs = 100000;


MediaFileProducer::MediaFileProducer(const char *name, const entry_ref &ref)
	: BMediaNode(name)
	, BMediaEventLooper()
	, BBufferProducer(B_MEDIA_RAW_VIDEO)
	, fInitStatus(B_NO_INIT)
	, fRef(ref)
	, fMediaFile(nullptr)
	, fVideoTrack(nullptr)
	, fOwnBufferGroup(nullptr)
	, fUsedBufferGroup(nullptr)
	, fThread(-1)
	, fFrameSync(-1)
	, fFrame(0)
	, fFrameBase(0)
	, fPerformanceTimeBase(0)
	, fRunning(false)
	, fConnected(false)
	, fEnabled(false)
{
	memset(&fConnectedFormat, 0, sizeof(fConnectedFormat));

	fMediaFile = new BMediaFile(&fRef);
	status_t err = fMediaFile->InitCheck();
	if (err != B_OK) {
		fprintf(stderr, "MediaFileProducer: BMediaFile InitCheck failed: %s\n",
			strerror(err));
		fInitStatus = err;
		return;
	}

	int32 numTracks = fMediaFile->CountTracks();
	for (int32 i = 0; i < numTracks; i++) {
		BMediaTrack *track = fMediaFile->TrackAt(i);
		if (track == nullptr) {
			continue;
		}
		media_format encFmt {};
		if (track->EncodedFormat(&encFmt) == B_OK && encFmt.IsVideo()) {
			fVideoTrack = track;
			break;
		}
		fMediaFile->ReleaseTrack(track);
	}
	if (fVideoTrack == nullptr) {
		fprintf(stderr, "MediaFileProducer: no video track in '%s'\n", fRef.name);
		fInitStatus = B_MEDIA_BAD_FORMAT;
		return;
	}

	// Negotiate decode format: ask for B_RGB32 and let the decoder pick
	// its natural resolution + stride. The 2D engine downstream will
	// scale to the display.
	media_format decFmt {};
	decFmt.type = B_MEDIA_RAW_VIDEO;
	decFmt.u.raw_video = media_raw_video_format::wildcard;
	decFmt.u.raw_video.display.format = B_RGB32;

	err = fVideoTrack->DecodedFormat(&decFmt);
	if (err != B_OK) {
		fprintf(stderr, "MediaFileProducer: DecodedFormat(B_RGB32) failed: %s\n",
			strerror(err));
		fInitStatus = err;
		return;
	}

	if (decFmt.u.raw_video.display.format != B_RGB32) {
		fprintf(stderr, "MediaFileProducer: decoder couldn't deliver B_RGB32 "
			"(got %d)\n", (int)decFmt.u.raw_video.display.format);
		fInitStatus = B_MEDIA_BAD_FORMAT;
		return;
	}

	fConnectedFormat = decFmt.u.raw_video;
	// Some decoders return bytes_per_row == 0 for "natural stride"; pin it
	// to width*4 in that case so downstream sizing math works.
	if (fConnectedFormat.display.bytes_per_row == 0) {
		fConnectedFormat.display.bytes_per_row =
			fConnectedFormat.display.line_width * 4;
	}

	printf("MediaFileProducer: %ux%u @ %.3f fps, stride %u, %" B_PRId64 " frames, "
		"%.3f s\n",
		(unsigned)fConnectedFormat.display.line_width,
		(unsigned)fConnectedFormat.display.line_count,
		fConnectedFormat.field_rate,
		(unsigned)fConnectedFormat.display.bytes_per_row,
		fVideoTrack->CountFrames(),
		(double)fVideoTrack->Duration() / 1e6);

	fOutput.destination = media_destination::null;
	fInitStatus = B_OK;
}


MediaFileProducer::~MediaFileProducer()
{
	if (fInitStatus == B_OK) {
		if (fConnected) {
			Disconnect(fOutput.source, fOutput.destination);
		}
		if (fRunning) {
			HandleStop();
		}
	}
	Quit();

	if (fMediaFile != nullptr) {
		// ReleaseAllTracks happens implicitly in ~BMediaFile.
		delete fMediaFile;
		fMediaFile = nullptr;
		fVideoTrack = nullptr;
	}
}


BMediaAddOn *
MediaFileProducer::AddOn(int32 *_internalID) const
{
	if (_internalID != nullptr) {
		*_internalID = 0;
	}
	return nullptr;
}


void MediaFileProducer::NodeRegistered()
{
	if (fInitStatus != B_OK) {
		ReportError(B_NODE_IN_DISTRESS);
		return;
	}

	fOutput.node = Node();
	fOutput.source.port = ControlPort();
	fOutput.source.id = 0;
	fOutput.destination = media_destination::null;
	strcpy(fOutput.name, Name());

	fOutput.format.type = B_MEDIA_RAW_VIDEO;
	fOutput.format.u.raw_video = fConnectedFormat;

	Run();
}


void MediaFileProducer::SetTimeSource(BTimeSource *timeSource)
{
	(void)timeSource;
	// Tell frame thread to recalculate its delay against the new clock.
	if (fFrameSync >= B_OK) {
		release_sem(fFrameSync);
	}
}


status_t
MediaFileProducer::HandleMessage(int32 message, const void *data, size_t size)
{
	(void)message; (void)data; (void)size;
	return B_ERROR;
}


void MediaFileProducer::Start(bigtime_t performanceTime)
{
	BMediaEventLooper::Start(performanceTime);
}


void MediaFileProducer::Stop(bigtime_t performanceTime, bool immediate)
{
	BMediaEventLooper::Stop(performanceTime, immediate);
}


void MediaFileProducer::Seek(bigtime_t mediaTime, bigtime_t performanceTime)
{
	BMediaEventLooper::Seek(mediaTime, performanceTime);
}


void MediaFileProducer::TimeWarp(bigtime_t atRealTime, bigtime_t toPerformanceTime)
{
	BMediaEventLooper::TimeWarp(atRealTime, toPerformanceTime);
}


status_t
MediaFileProducer::DeleteHook(BMediaNode *node)
{
	return BMediaEventLooper::DeleteHook(node);
}


void MediaFileProducer::HandleEvent(const media_timed_event *event,
	bigtime_t lateness, bool realTimeEvent)
{
	(void)lateness;
	(void)realTimeEvent;
	switch (event->type) {
	case BTimedEventQueue::B_START:
		HandleStart(event->event_time);
		break;
	case BTimedEventQueue::B_STOP:
		EventQueue()->FlushEvents(event->event_time,
			BTimedEventQueue::B_ALWAYS, true,
			BTimedEventQueue::B_HANDLE_BUFFER);
		HandleStop();
		break;
	case BTimedEventQueue::B_WARP:
		HandleTimeWarp(event->bigdata);
		break;
	case BTimedEventQueue::B_SEEK:
		HandleSeek(event->bigdata);
		break;
	default:
		break;
	}
}


// --- BBufferProducer ---

status_t
MediaFileProducer::FormatSuggestionRequested(media_type type, int32 quality,
	media_format *format)
{
	(void)quality;
	if (format == nullptr) {
		return B_BAD_VALUE;
	}
	if (type != B_MEDIA_RAW_VIDEO && type != B_MEDIA_UNKNOWN_TYPE) {
		return B_MEDIA_BAD_FORMAT;
	}
	*format = fOutput.format;
	return B_OK;
}


status_t
MediaFileProducer::FormatProposal(const media_source &output, media_format *format)
{
	if (format == nullptr) {
		return B_BAD_VALUE;
	}
	if (output != fOutput.source) {
		return B_MEDIA_BAD_SOURCE;
	}
	status_t err = format_is_compatible(*format, fOutput.format) ?
		B_OK : B_MEDIA_BAD_FORMAT;
	*format = fOutput.format;
	return err;
}


status_t
MediaFileProducer::FormatChangeRequested(const media_source &source,
	const media_destination &destination, media_format *io_format,
	int32 *_deprecated_)
{
	(void)destination; (void)io_format; (void)_deprecated_;
	if (source != fOutput.source) {
		return B_MEDIA_BAD_SOURCE;
	}
	// Output format is pinned to whatever the decoder negotiated against
	// the display mode; changing it mid-stream isn't supported.
	return B_ERROR;
}


status_t
MediaFileProducer::GetNextOutput(int32 *cookie, media_output *out_output)
{
	if (out_output == nullptr) {
		return B_BAD_VALUE;
	}
	if (*cookie != 0) {
		return B_BAD_INDEX;
	}
	*out_output = fOutput;
	(*cookie)++;
	return B_OK;
}


status_t
MediaFileProducer::DisposeOutputCookie(int32)
{
	return B_OK;
}


status_t
MediaFileProducer::SetBufferGroup(const media_source &for_source,
	BBufferGroup *group)
{
	if (for_source != fOutput.source) {
		return B_MEDIA_BAD_SOURCE;
	}
	// Per BeBook BBufferProducer::SetBufferGroup: the consumer's group
	// replaces ours. We never delete the consumer's group ourselves
	// (consumer owns it), but if we previously created our own fallback
	// group, drop it now.
	BAutolock _(fLock);
	if (fOwnBufferGroup != nullptr && fOwnBufferGroup != group) {
		delete fOwnBufferGroup;
		fOwnBufferGroup = nullptr;
	}
	fUsedBufferGroup = group;
	return B_OK;
}


status_t
MediaFileProducer::GetLatency(bigtime_t *out_latency)
{
	*out_latency = EventLatency() + SchedulingLatency();
	return B_OK;
}


status_t
MediaFileProducer::PrepareToConnect(const media_source &source,
	const media_destination &destination, media_format *format,
	media_source *out_source, char *out_name)
{
	if (fConnected) {
		return EALREADY;
	}
	if (source != fOutput.source) {
		return B_MEDIA_BAD_SOURCE;
	}
	if (fOutput.destination != media_destination::null) {
		return B_MEDIA_ALREADY_CONNECTED;
	}
	if (!format_is_compatible(*format, fOutput.format)) {
		*format = fOutput.format;
		return B_MEDIA_BAD_FORMAT;
	}

	// Specialize any remaining wildcards from the consumer side to our
	// known decoded format.
	media_raw_video_format &rv = format->u.raw_video;
	if (rv.display.format == media_raw_video_format::wildcard.display.format) {
		rv.display.format = B_RGB32;
	}
	if (rv.display.line_width == 0) {
		rv.display.line_width = fConnectedFormat.display.line_width;
	}
	if (rv.display.line_count == 0) {
		rv.display.line_count = fConnectedFormat.display.line_count;
	}
	if (rv.display.bytes_per_row == 0) {
		rv.display.bytes_per_row = fConnectedFormat.display.bytes_per_row;
	}
	if (rv.field_rate == 0) {
		rv.field_rate = fConnectedFormat.field_rate;
	}

	*out_source = fOutput.source;
	strcpy(out_name, fOutput.name);
	fOutput.destination = destination;
	return B_OK;
}


void MediaFileProducer::Connect(status_t error, const media_source &source,
	const media_destination &destination, const media_format &format,
	char *io_name)
{
	if (fConnected) {
		return;
	}
	if (source != fOutput.source || error < B_OK
		|| !const_cast<media_format *>(&format)->Matches(&fOutput.format)) {
		fprintf(stderr, "MediaFileProducer::Connect: error=%s\n", strerror(error));
		return;
	}

	fOutput.destination = destination;
	strcpy(io_name, fOutput.name);
	fConnectedFormat = format.u.raw_video;

	bigtime_t latency = 0;
	media_node_id tsID = 0;
	FindLatencyFor(fOutput.destination, &latency, &tsID);
	SetEventLatency(latency + kDecodeLatencyHintUs);

	{
		BAutolock _(fLock);
		// Fallback group if the consumer never pushed one via
		// SetBufferGroup (some consumer setups don't).
		if (fUsedBufferGroup == nullptr) {
			fOwnBufferGroup = new BBufferGroup(
				fConnectedFormat.display.bytes_per_row
					* fConnectedFormat.display.line_count,
				3);
			if (fOwnBufferGroup->InitCheck() != B_OK) {
				fprintf(stderr, "MediaFileProducer::Connect: fallback "
					"BBufferGroup InitCheck failed\n");
				delete fOwnBufferGroup;
				fOwnBufferGroup = nullptr;
				return;
			}
			fUsedBufferGroup = fOwnBufferGroup;
		}
	}

	fConnected = true;
	fEnabled = true;

	if (fFrameSync >= B_OK) {
		release_sem(fFrameSync);
	}
}


void MediaFileProducer::Disconnect(const media_source &source,
	const media_destination &destination)
{
	if (!fConnected) {
		return;
	}
	if (source != fOutput.source || destination != fOutput.destination) {
		return;
	}
	fEnabled = false;
	fOutput.destination = media_destination::null;

	{
		BAutolock _(fLock);
		if (fOwnBufferGroup != nullptr) {
			delete fOwnBufferGroup;
			fOwnBufferGroup = nullptr;
		}
		fUsedBufferGroup = nullptr;
	}

	fConnected = false;
}


void MediaFileProducer::LateNoticeReceived(const media_source &what,
	bigtime_t how_much, bigtime_t performance_time)
{
	(void)what; (void)how_much; (void)performance_time;
	// We're CPU-decode-bound — there's no slack to give back. A real
	// implementation would log this or speed up by skipping.
}


void MediaFileProducer::EnableOutput(const media_source &what, bool enabled,
	int32 *_deprecated_)
{
	(void)_deprecated_;
	if (what != fOutput.source) {
		return;
	}
	fEnabled = enabled;
}


void MediaFileProducer::AdditionalBufferRequested(const media_source &source,
	media_buffer_id prev_buffer, bigtime_t prev_time,
	const media_seek_tag *prev_tag)
{
	(void)source; (void)prev_buffer; (void)prev_time; (void)prev_tag;
}


void MediaFileProducer::LatencyChanged(const media_source &source,
	const media_destination &destination, bigtime_t new_latency,
	uint32 flags)
{
	(void)source; (void)destination; (void)new_latency; (void)flags;
}


// --- Frame thread ---

void MediaFileProducer::HandleStart(bigtime_t performanceTime)
{
	if (fRunning) {
		return;
	}
	fFrame = 0;
	fFrameBase = 0;
	fPerformanceTimeBase = performanceTime;

	fFrameSync = create_sem(0, "MediaFileProducer frame sync");
	if (fFrameSync < B_OK) {
		fprintf(stderr, "MediaFileProducer::HandleStart: create_sem failed\n");
		return;
	}
	fThread = spawn_thread(FrameThreadEntry, "MediaFileProducer frames",
		B_NORMAL_PRIORITY, this);
	if (fThread < B_OK) {
		fprintf(stderr, "MediaFileProducer::HandleStart: spawn_thread failed\n");
		delete_sem(fFrameSync);
		fFrameSync = -1;
		return;
	}
	resume_thread(fThread);
	fRunning = true;
}


void MediaFileProducer::HandleStop()
{
	if (!fRunning) {
		return;
	}
	delete_sem(fFrameSync);
	fFrameSync = -1;
	status_t exitVal;
	wait_for_thread(fThread, &exitVal);
	fThread = -1;
	fRunning = false;
}


void MediaFileProducer::HandleTimeWarp(bigtime_t performanceTime)
{
	fPerformanceTimeBase = performanceTime;
	fFrameBase = fFrame;
	if (fFrameSync >= B_OK) {
		release_sem(fFrameSync);
	}
}


void MediaFileProducer::HandleSeek(bigtime_t performanceTime)
{
	fPerformanceTimeBase = performanceTime;
	fFrameBase = fFrame;
	if (fFrameSync >= B_OK) {
		release_sem(fFrameSync);
	}
}


int32 MediaFileProducer::FrameThreadEntry(void *data)
{
	return static_cast<MediaFileProducer *>(data)->FrameThread();
}


int32 MediaFileProducer::FrameThread()
{
	bigtime_t waitUntil = system_time();
	bool decodeExhausted = false;

	while (true) {
		status_t err = acquire_sem_etc(fFrameSync, 1, B_ABSOLUTE_TIMEOUT, waitUntil);
		if (err != B_OK && err != B_TIMED_OUT) {
			// fFrameSync deleted (HandleStop) — exit cleanly.
			break;
		}

		// Recompute wakeup for the *next* frame from the current time
		// source mapping.
		fFrame++;
		double frameDurationUs = 1000000.0 /
			(fConnectedFormat.field_rate > 0 ? fConnectedFormat.field_rate : 30.0);
		bigtime_t framePerfTime = fPerformanceTimeBase
			+ (bigtime_t)((fFrame - fFrameBase) * frameDurationUs);
		// Wake EventLatency() + SchedulingLatency() *ahead* of the frame's
		// presentation time so the decoded buffer is delivered with enough
		// lead for the consumer to blit and latch a flip by start_time.
		// EventLatency() here is the downstream (consumer) latency plus our
		// decode hint, set in Connect(). Passing 0 would wake us at the
		// deadline and the buffer would always arrive late.
		bigtime_t lead = EventLatency() + SchedulingLatency();
		waitUntil = TimeSource()->RealTimeFor(framePerfTime, lead);
		if (waitUntil < system_time() - kResyncThresholdUs) {
			// We've fallen far behind the schedule. Re-anchor so the frame
			// we're about to send targets ~now (current performance time),
			// dropping the stale backlog without skipping any decoded frame.
			// Covers startup decode latency and mid-stream stalls alike.
			fprintf(stderr, "MediaFileProducer: re-anchoring timeline "
				"(%" B_PRId64 " us behind schedule)\n",
				system_time() - waitUntil);
			fPerformanceTimeBase = TimeSource()->PerformanceTimeFor(system_time());
			fFrameBase = fFrame;
			framePerfTime = fPerformanceTimeBase;
			waitUntil = TimeSource()->RealTimeFor(framePerfTime, lead);
		}

		if (err == B_OK) {
			// Semaphore was released to signal a timing change — go round
			// the loop with the recomputed wakeup time.
			continue;
		}

		if (!fRunning || !fEnabled || fUsedBufferGroup == nullptr
			|| decodeExhausted) {
			continue;
		}

		BAutolock _(fLock);
		if (fUsedBufferGroup == nullptr) {
			continue;
		}

		const size_t frameSize = (size_t)fConnectedFormat.display.bytes_per_row
			* fConnectedFormat.display.line_count;

		BBuffer *buffer = fUsedBufferGroup->RequestBuffer(frameSize, 0LL);
		if (buffer == nullptr) {
			// All buffers in flight — skip this frame. Persistent starvation
			// means the group is too shallow for the current lead.
			fprintf(stderr, "MediaFileProducer: no free buffer, skipping "
				"frame %" B_PRId64 "\n", fFrame);
			continue;
		}

		int64 frameCount = 1;
		media_header h {};
		err = fVideoTrack->ReadFrames(buffer->Data(), &frameCount, &h);
		if (err == B_LAST_BUFFER_ERROR || err != B_OK) {
			if (err != B_LAST_BUFFER_ERROR) {
				fprintf(stderr, "MediaFileProducer: ReadFrames failed: %s\n",
					strerror(err));
			}
			buffer->Recycle();
			decodeExhausted = true;
			// Tell downstream the stream is done so it can drain.
			SendDataStatus(B_PRODUCER_STOPPED, fOutput.destination,
				TimeSource()->Now());
			continue;
		}

		media_header *bh = buffer->Header();
		bh->type = B_MEDIA_RAW_VIDEO;
		bh->time_source = TimeSource()->ID();
		bh->size_used = frameSize;
		bh->start_time = framePerfTime;
		bh->file_pos = h.file_pos;
		bh->orig_size = h.orig_size;
		bh->data_offset = 0;
		bh->u.raw_video.field_gamma = 1.0;
		bh->u.raw_video.field_sequence = (uint32)fFrame;
		bh->u.raw_video.field_number = 0;
		bh->u.raw_video.pulldown_number = 0;
		bh->u.raw_video.first_active_line = 0;
		bh->u.raw_video.line_count = fConnectedFormat.display.line_count;

		if (SendBuffer(buffer, fOutput.source, fOutput.destination) != B_OK) {
			buffer->Recycle();
		}
	}

	return B_OK;
}
