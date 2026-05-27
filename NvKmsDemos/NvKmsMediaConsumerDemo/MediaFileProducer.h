#pragma once

#include <BufferProducer.h>
#include <Entry.h>
#include <Locker.h>
#include <MediaDefs.h>
#include <MediaEventLooper.h>
#include <kernel/OS.h>


class BBuffer;
class BBufferGroup;
class BMediaFile;
class BMediaTrack;


// A BBufferProducer that reads frames from a media file directly via
// BMediaFile / BMediaTrack and pushes them downstream as B_MEDIA_RAW_VIDEO.
//
// Unlike the Media Roster approach (SniffRef + InstantiateDormantNode), this
// node is constructed in-process from an entry_ref — there is no separate
// addon to find. It picks the first B_MEDIA_RAW_VIDEO track in the file,
// asks the decoder to deliver B_RGB32 frames at the negotiated stride, and
// runs a single frame-generator thread that paces output via the time
// source.
class MediaFileProducer
	: public BMediaEventLooper
	, public BBufferProducer {
public:
	MediaFileProducer(const char *name, const entry_ref &ref);
	virtual ~MediaFileProducer();

	MediaFileProducer(const MediaFileProducer &) = delete;
	MediaFileProducer &operator=(const MediaFileProducer &) = delete;

	status_t InitCheck() const { return fInitStatus; }

	// BMediaNode
	virtual BMediaAddOn *AddOn(int32 *_internalID) const;

protected:
	virtual void NodeRegistered();
	virtual void SetTimeSource(BTimeSource *timeSource);
	virtual status_t HandleMessage(int32 message, const void *data, size_t size);

	// BMediaEventLooper
	virtual void Start(bigtime_t performanceTime);
	virtual void Stop(bigtime_t performanceTime, bool immediate);
	virtual void Seek(bigtime_t mediaTime, bigtime_t performanceTime);
	virtual void TimeWarp(bigtime_t atRealTime, bigtime_t toPerformanceTime);
	virtual void HandleEvent(const media_timed_event *event,
		bigtime_t lateness, bool realTimeEvent);
	virtual status_t DeleteHook(BMediaNode *node);

	// BBufferProducer
	virtual status_t FormatSuggestionRequested(media_type type, int32 quality,
		media_format *format);
	virtual status_t FormatProposal(const media_source &output,
		media_format *format);
	virtual status_t FormatChangeRequested(const media_source &source,
		const media_destination &destination, media_format *io_format,
		int32 *_deprecated_);
	virtual status_t GetNextOutput(int32 *cookie, media_output *out_output);
	virtual status_t DisposeOutputCookie(int32 cookie);
	virtual status_t SetBufferGroup(const media_source &for_source,
		BBufferGroup *group);
	virtual status_t GetLatency(bigtime_t *out_latency);
	virtual status_t PrepareToConnect(const media_source &what,
		const media_destination &where, media_format *format,
		media_source *out_source, char *out_name);
	virtual void Connect(status_t error, const media_source &source,
		const media_destination &destination, const media_format &format,
		char *io_name);
	virtual void Disconnect(const media_source &what,
		const media_destination &where);
	virtual void LateNoticeReceived(const media_source &what,
		bigtime_t how_much, bigtime_t performance_time);
	virtual void EnableOutput(const media_source &what, bool enabled,
		int32 *_deprecated_);
	virtual void AdditionalBufferRequested(const media_source &source,
		media_buffer_id prev_buffer, bigtime_t prev_time,
		const media_seek_tag *prev_tag);
	virtual void LatencyChanged(const media_source &source,
		const media_destination &destination, bigtime_t new_latency,
		uint32 flags);

private:
	void HandleStart(bigtime_t performanceTime);
	void HandleStop();
	void HandleTimeWarp(bigtime_t performanceTime);
	void HandleSeek(bigtime_t performanceTime);

	static int32 FrameThreadEntry(void *data);
	int32 FrameThread();

	status_t fInitStatus;

	entry_ref fRef;
	BMediaFile *fMediaFile;
	BMediaTrack *fVideoTrack;

	BLocker fLock;
	BBufferGroup *fOwnBufferGroup;
	BBufferGroup *fUsedBufferGroup;

	thread_id fThread;
	sem_id fFrameSync;

	int64 fFrame;
	int64 fFrameBase;
	bigtime_t fPerformanceTimeBase;
	media_output fOutput;
	media_raw_video_format fConnectedFormat;
	bool fRunning;
	bool fConnected;
	bool fEnabled;
};
