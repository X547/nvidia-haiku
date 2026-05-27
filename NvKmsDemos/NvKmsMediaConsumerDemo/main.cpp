#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <system_error>

#include <Application.h>
#include <Entry.h>
#include <MediaDefs.h>
#include <MediaNode.h>
#include <MediaRoster.h>
#include <Path.h>
#include <TimeSource.h>
#include <scheduler.h>

#include "ErrorUtils.h"

#include "NvKmsDisplay.h"
#include "NvKmsMediaConsumer.h"
#include "MediaFileProducer.h"


static const char *kAppSignature = "application/x-vnd.nvidia-haiku.NvKmsMediaConsumerDemo";


static void DieIfErr(status_t st, const char *what)
{
	if (st < B_OK) {
		fprintf(stderr, "[!] %s: %s\n", what, strerror(st));
		exit(1);
	}
}


class DemoApp : public BApplication {
public:
	DemoApp(): BApplication(kAppSignature) {}
};


static DemoApp *gApp = nullptr;

static void HandleSigInt(int)
{
	if (gApp != nullptr) {
		gApp->PostMessage(B_QUIT_REQUESTED);
	}
}


int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <media-file>\n", argv[0]);
		return 1;
	}

	entry_ref inRef;
	status_t st = get_ref_for_path(argv[1], &inRef);
	if (st != B_OK) {
		fprintf(stderr, "[!] can't resolve '%s': %s\n", argv[1], strerror(st));
		return 1;
	}

	// BApplication first so the Media Kit's BLooper machinery (the roster
	// is itself a BLooper client) has somewhere to live.
	DemoApp app;
	gApp = &app;
	signal(SIGINT, HandleSigInt);
	signal(SIGTERM, HandleSigInt);

	NvKmsDisplay display;
	try {
		display.SetupDisplay();
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvKmsDisplay setup: %s\n", ex.what());
		return 1;
	}

	BMediaRoster *roster = BMediaRoster::Roster(&st);
	DieIfErr(st, "BMediaRoster::Roster");

	// 1. Build the consumer node and register it.
	NvKmsMediaConsumer *consumer = new NvKmsMediaConsumer("NvKmsMediaConsumer", display);
	st = roster->RegisterNode(consumer);
	DieIfErr(st, "RegisterNode(consumer)");
	st = roster->SetTimeSourceFor(consumer->ID(), consumer->ID());
	DieIfErr(st, "SetTimeSourceFor(consumer)");

	// 2. Build the file producer in-process (no SniffRef / dormant-node
	//    lookup — we own the decode path). BMediaFile + BMediaTrack inside
	//    the producer pick up whatever installed codec handles the file.
	//    The producer reports the clip's natural resolution; the 2D engine
	//    scales it onto the display at flip time.
	MediaFileProducer *producer = new MediaFileProducer("MediaFileProducer", inRef);
	if (producer->InitCheck() != B_OK) {
		fprintf(stderr, "[!] MediaFileProducer init failed: %s\n",
			strerror(producer->InitCheck()));
		delete producer;
		return 1;
	}
	st = roster->RegisterNode(producer);
	DieIfErr(st, "RegisterNode(producer)");

	// 3. Slave the producer to the consumer's clock. (The consumer was
	//    already made its own time source above.)
	st = roster->SetTimeSourceFor(producer->Node().node, consumer->Node().node);
	DieIfErr(st, "SetTimeSourceFor(producer)");

	// 4. Find the producer's free output and our consumer's input.
	media_output prodOut {};
	media_input consIn {};
	int32 count = 1;
	st = roster->GetFreeOutputsFor(producer->Node(), &prodOut, 1, &count,
		B_MEDIA_RAW_VIDEO);
	if (st != B_OK || count < 1) {
		fprintf(stderr, "[!] producer has no free B_MEDIA_RAW_VIDEO output: %s\n",
			strerror(st));
		return 1;
	}
	count = 1;
	st = roster->GetFreeInputsFor(consumer->Node(), &consIn, 1, &count,
		B_MEDIA_RAW_VIDEO);
	if (st != B_OK || count < 1) {
		fprintf(stderr, "[!] consumer has no free input: %s\n", strerror(st));
		return 1;
	}

	// 5. Connect. Only B_RGB32 is pinned — width/height/stride come from
	//    the producer's negotiated format (the clip's natural size). The
	//    2D engine resizes onto the display at flip time, so the consumer
	//    handles any source dimensions.
	media_format fmt {};
	fmt.type = B_MEDIA_RAW_VIDEO;
	fmt.u.raw_video = media_raw_video_format::wildcard;
	fmt.u.raw_video.display.format = B_RGB32;

	st = roster->Connect(prodOut.source, consIn.destination, &fmt,
		&prodOut, &consIn);
	if (st != B_OK) {
		fprintf(stderr, "[!] Connect: %s\n", strerror(st));
		return 1;
	}

	roster->SetRunModeNode(producer->Node(), BMediaNode::B_INCREASE_LATENCY);
	roster->SetRunModeNode(consumer->Node(), BMediaNode::B_DROP_DATA);

	roster->PrerollNode(consumer->Node());
	roster->PrerollNode(producer->Node());

	BTimeSource *ts = roster->MakeTimeSourceFor(consumer->Node());
	if (ts == nullptr) {
		fprintf(stderr, "[!] MakeTimeSourceFor returned NULL\n");
		return 1;
	}

	bigtime_t producerLatency = 0;
	roster->GetLatencyFor(producer->Node(), &producerLatency);

	bigtime_t now = ts->Now();
	roster->StartTimeSource(consumer->Node(), BTimeSource::RealTime());

	roster->StartNode(consumer->Node(), now);
	roster->StartNode(producer->Node(), now + producerLatency);
	ts->Release();

	printf("playing %s — Ctrl-C to stop\n", argv[1]);

	app.Run();

	// Teardown.
	roster->StopNode(producer->Node(), 0, true);
	roster->StopNode(consumer->Node(), 0, true);
	roster->Disconnect(producer->Node().node, prodOut.source,
		consumer->Node().node, consIn.destination);
	roster->ReleaseNode(producer->Node());
	roster->ReleaseNode(consumer->Node());

	return 0;
}
