# NvKmsMediaConsumerDemo — Architecture

This demo plays a video file straight onto an NVKMS scanout surface using the
Haiku Media Kit for decode and timing, and the NVIDIA RM/KMS stack for GPU
upload and presentation. It is also a worked example of three things that are
easy to get subtly wrong: **buffer scheduling**, **latency calculation**, and
**time-source handling**. This document explains how the pieces fit together
and why the code is shaped the way it is.

## Table of contents

- [The pipeline at a glance](#the-pipeline-at-a-glance)
- [Node topology](#node-topology)
- [The GPU/display side: NvKmsDisplay](#the-gpudisplay-side-nvkmsdisplay)
- [Buffer scheduling](#buffer-scheduling)
- [Latency calculation](#latency-calculation)
- [Time-source handling](#time-source-handling)
- [The present thread](#the-present-thread)
- [Startup and stalls: timeline re-anchoring](#startup-and-stalls-timeline-re-anchoring)
- [End-to-end frame walkthrough](#end-to-end-frame-walkthrough)
- [Known limitations](#known-limitations)

---

## The pipeline at a glance

```
                 BMediaFile/BMediaTrack (decode)
                          │
                          ▼
   ┌──────────────────────────────────────┐
   │ MediaFileProducer  (BBufferProducer)  │   decodes frames into GART buffers,
   │   - frame thread paces sends          │   stamps them with a performance
   │   - slaved to consumer's time source  │   time, sends downstream
   └──────────────────────────────────────┘
                          │  BBuffer (GART-backed, B_RGB32)
                          ▼
   ┌──────────────────────────────────────┐
   │ NvKmsMediaConsumer (BBufferConsumer   │   late-drop check, then hand off to
   │   + BMediaEventLooper + BTimeSource)  │   the present thread (show-latest)
   │   - control thread: dispatch/handoff  │
   │   - present thread: blit + flip       │
   │   - publishes the time source clock   │
   └──────────────────────────────────────┘
                          │  GART → VRAM scale-blit, then NVKMS flip
                          ▼
   ┌──────────────────────────────────────┐
   │ NvKmsDisplay (RM + KMS device pair)   │   FERMI_TWOD_A bilinear blit into a
   │   - VRAM scanout buffers              │   double-buffered VRAM surface,
   │   - flip-completion notifier (PTIMER) │   NVKMS_IOCTL_FLIP, FLIP_OCCURRED
   └──────────────────────────────────────┘
                          │
                          ▼
                       Display
```

The two media nodes live **in-process**: `main.cpp` builds them directly rather
than via dormant-node lookup, because the demo owns the whole decode and
presentation path.

Source files:

- [main.cpp](main.cpp) — builds the graph, connects, starts.
- [MediaFileProducer.cpp](MediaFileProducer.cpp) / [.h](MediaFileProducer.h) — file decode + frame pacing.
- [NvKmsMediaConsumer.cpp](NvKmsMediaConsumer.cpp) / [.h](NvKmsMediaConsumer.h) — scanout consumer + time source.
- [NvKmsDisplay.cpp](NvKmsDisplay.cpp) / [.h](NvKmsDisplay.h) — RM/KMS device, blit, flip.
- [GartBufferGroup.cpp](GartBufferGroup.cpp) / [.h](GartBufferGroup.h) — `BBufferGroup` backed by NVRM GART allocations.

---

## Node topology

There are two Media Kit nodes and one unusual wiring decision:

- **`MediaFileProducer`** is a `BBufferProducer` + `BMediaEventLooper`. It opens
  the file with `BMediaFile`/`BMediaTrack`, negotiates a `B_RGB32` decode
  format at the clip's natural resolution, and runs a private **frame thread**
  that decodes and sends buffers.
- **`NvKmsMediaConsumer`** is a `BBufferConsumer` + `BMediaEventLooper` **and a
  `BTimeSource`**. It exposes one `B_MEDIA_RAW_VIDEO` input, hands the producer
  a GART-backed `BBufferGroup` (so the producer decodes straight into memory the
  GPU can DMA from), blits each frame into VRAM, and flips it onscreen.

The wiring (`main.cpp`):

```cpp
roster->SetTimeSourceFor(consumer->ID(), consumer->ID());      // consumer is its own clock
...
roster->SetTimeSourceFor(producer->Node().node, consumer->Node().node); // producer slaves to it
```

So **the consumer is the master clock for the whole graph**, and it derives that
clock from when frames actually reach the screen (the GPU flip-completion
timestamp). The producer schedules its work against that clock. This is the
"present, then derive the clock from presentation" pattern; its consequences run
through the rest of this document.

> **Note:** because the consumer is its own time source, Haiku's `BTimeSource`
> logs `DirectAddMe should not add itself to slave nodes` once at startup. That
> is expected and harmless — a clock has no reason to slave to itself.

---

## The GPU/display side: NvKmsDisplay

`NvKmsDisplay` owns the RM/KMS device pair, a graphics-engine channel used to
drive `FERMI_TWOD_A`, two VRAM scanout buffers, and the per-flip completion
notifier. Two calls matter to the rest of the system:

### `BlitAndFlip(srcGpuAddr, srcW, srcH, srcPitch)`

1. Picks the *other* VRAM buffer (double buffering).
2. Computes an aspect-preserving centered destination rect; pixels outside it
   keep the black they were cleared to, giving free letterbox/pillarbox bars.
3. Emits a `FERMI_TWOD_A` bilinear scale-blit from the GART source into VRAM,
   followed by a semaphore release + non-stall interrupt.
4. **Blocks** until the copy-engine semaphore reaches the submitted value
   (`poll()` on the blit event fd), then submits an `NVKMS_IOCTL_FLIP`.

The blocking wait in step 4 is the key reason presentation runs on a separate
thread (see [The present thread](#the-present-thread)).

### `PollFlipNotifierTimestamp(outPtimerNs, timeoutMs)`

Drains the NVKMS event queue looking for `NVKMS_EVENT_TYPE_FLIP_OCCURRED`. When
one is found it returns the GPU **PTIMER** timestamp (1 ns/tick on Turing+) of
the flip that just completed. The demo calls it with `timeoutMs = 0`
(non-blocking drain).

> A subtlety the code documents: the very first flip after `SetMode` has no
> preceding flip to displace, so its `FLIP_OCCURRED` does not fire until the
> *second* flip lands. The clock simply starts publishing once events begin
> arriving.

---

## Buffer scheduling

### The Media Kit contract

Every buffer carries a `start_time` in **performance time** — the instant it
should be *presented*. Two independent mechanisms must cooperate to hit that
instant:

1. The **producer** must *send* the buffer early enough that it is in the
   consumer's hands before `start_time`.
2. The **consumer's** `BMediaEventLooper` must *dispatch* `HandleEvent` early
   enough to do the GPU work and latch a flip by `start_time`.

Both leads are expressed as **latency**, and they must agree. The looper makes
the rule explicit ([MediaEventLooper.cpp](https://git.haiku-os.org/haiku/tree/src/kits/media/MediaEventLooper.cpp), `ControlLoop`):

```cpp
waitUntil = TimeSource()->RealTimeFor(
    fEventQueue.FirstEventTime(),
    fEventLatency + fSchedulingLatency);
```

`RealTimeFor(perf, lat) == realtime(perf) − lat`, so an event is dispatched
`fEventLatency` (plus thread-scheduling slack) **before** its `start_time`. That
window is the consumer's processing budget.

### Producer side: the frame thread

`MediaFileProducer::FrameThread` ([MediaFileProducer.cpp](MediaFileProducer.cpp)) computes each
frame's performance time from a base and the frame index, then sleeps until it
should *send* that frame — which is its presentation time minus the producer's
own pipeline latency:

```cpp
bigtime_t framePerfTime = fPerformanceTimeBase
    + (bigtime_t)((fFrame - fFrameBase) * frameDurationUs);

bigtime_t lead = EventLatency() + SchedulingLatency();
waitUntil = TimeSource()->RealTimeFor(framePerfTime, lead);
```

Passing `lead` (rather than `0`) is what makes the buffer arrive *ahead* of its
deadline instead of exactly on it. The buffer is then stamped:

```cpp
bh->start_time = framePerfTime;
```

The producer only ever holds as many buffers as the GART group allows
(`RequestBuffer` returns `null` when all are in flight, and the frame is
skipped), so it self-throttles and cannot outrun the consumer without bound.

### Consumer side: dispatch and hand-off

When a buffer arrives, `BufferReceived` enqueues a `B_HANDLE_BUFFER` event at the
buffer's `start_time`. The looper dispatches it `fEventLatency` early. In
`HandleEvent` the consumer:

1. Checks run state / ownership.
2. **Drops late frames**: if `TimeSource()->Now() > start_time + kJitterUs`
   (20 ms), the frame is recycled — there is no point presenting a frame whose
   moment has passed.
3. Otherwise **hands the frame to the present thread** through a single-slot
   mailbox and returns immediately (it does *not* blit here — see below).

### Show-latest hand-off

The mailbox holds at most one pending frame. If the GPU is still busy when the
next frame is dispatched, the older pending frame is recycled and replaced:

```cpp
BAutolock _(fPresentLock);
if (fPendingBuffer != nullptr) {
    fPendingBuffer->Recycle();   // GPU is behind: drop the older frame
} else {
    signal = true;
}
fPendingBuffer = buffer;
fPendingGpuAddr = srcGpuAddr;
```

This bounds backlog to one in-flight + one pending frame and keeps presentation
on the freshest decoded frame.

---

## Latency calculation

The single most important correctness rule here is: **the consumer's reported
latency, its event latency, and the producer's send lead must all describe the
same budget.** Earlier versions of this demo reported one number to upstream
(`20 ms`) while setting the event latency to `0`, which meant the looper
dispatched at the presentation deadline and every frame was structurally late.

### Consumer presentation latency

Computed once in the constructor (the display mode is already set by then):

```cpp
NvU32 rateMilliHz = fDisplay.RefreshRateMilliHz();
bigtime_t refreshUs = rateMilliHz > 0 ? 1000000000LL / rateMilliHz : 16667;
fLatency = fBlitBudgetUs + refreshUs;   // blit headroom + one vsync latch

SetEventLatency(fLatency);              // dispatch this early
// GetLatencyFor() reports the same fLatency to the producer
```

The two terms:

- **`fBlitBudgetUs`** — time to copy one scaled frame GART → VRAM
  (`kInitialBlitBudgetUs`, ≈ one frame; can later be replaced by a measured
  value).
- **one refresh interval** — a flip submitted now only *latches* at the next
  vsync, so the submission must happen at least one refresh before the target.

The same value is reported via `GetLatencyFor`, so when the producer calls
`FindLatencyFor` during connection it learns exactly how much lead the consumer
needs.

### Producer lead

The producer adds a decode budget on top of the downstream latency it discovers:

```cpp
FindLatencyFor(fOutput.destination, &latency, &tsID);
SetEventLatency(latency + kDecodeLatencyHintUs);   // ~30 ms decode slack
```

and, as shown above, schedules its sends `EventLatency() + SchedulingLatency()`
ahead of each frame's presentation time. The net effect: the producer wakes
early, decodes, and delivers the buffer so it sits in the consumer's event queue
until the looper dispatches it one presentation-latency before `start_time`.

> **Consequence for buffer depth.** Because the producer now runs several frames
> ahead, the GART buffer group is sized to **5** ([NvKmsMediaConsumer.cpp](NvKmsMediaConsumer.cpp),
> `Connected`): producer lead (~3–4 frames) + one being blitted + one pending.
> Too few buffers and `RequestBuffer` starves, causing avoidable frame skips.

---

## Time-source handling

The consumer is a `BTimeSource`, and its clock is what the producer schedules
against. Haiku's time source is a simple linear map published as
`(performance_time, real_time, drift)` triples; readers compute:

```
RealTimeFor(perf)        = real + (perf − perf0) / drift
PerformanceTimeFor(real) = perf + (real − real0) * drift
```

### Establishing the clock

`TimeSourceOp` handles the lifecycle:

- **`B_TIMESOURCE_START`** publishes `(now, now, 1.0)` so slaves have a valid
  anchor before any flip has happened.
- **`B_TIMESOURCE_STOP`** republishes the last performance time at `drift = 0`
  to freeze slaves.
- **`B_TIMESOURCE_SEEK`** broadcasts a time warp.

### Deriving the clock from flips (PTIMER anchoring)

After each flip completes, the present thread reads the flip's PTIMER timestamp
and publishes performance time from it:

```cpp
int64 ptimerUs = (int64)(ptimerNs / 1000);
bigtime_t realUs = system_time();
if (!fPtimerOffsetSet) {
    fPtimerToPerfOffsetUs = realUs - ptimerUs;   // anchor PTIMER to system_time
    fPtimerOffsetSet = true;
}
bigtime_t perfUs = (bigtime_t)(ptimerUs + fPtimerToPerfOffsetUs);
PublishTime(perfUs, realUs, 1.0f);
```

Performance time then advances by **GPU PTIMER deltas** (vsync-locked), while
real time advances by `system_time()`. The first flip is anchored so the two
clocks coincide.

### Why the anchor uses `system_time()`, not the last published value

This is a real bug that was fixed and is worth understanding. The intuitive
anchor is "continue from the last performance time I published from
`B_TIMESOURCE_START`." But `main` issues a `SeekTimeSource(consumer, 0, ...)`
during setup, and `B_TIMESOURCE_SEEK` resets the stored last-published value to
**0**. Anchoring PTIMER to 0 makes performance time trail real time by
`system_time()` (hundreds of seconds), so:

- `RealTimeFor(framePerfTime)` returns a wall-clock time **far in the future**,
- the producer's frame thread sleeps "until then" → **playback freezes**.

Anchoring to `system_time()` at the first flip keeps the published timeline in
the same domain the producer's frames live in (its `B_START` event time is also
`system_time`-based), which is what makes the mapping correct. The fields used
for the anchor (`fLastPerformancePublishedUs` / `fLastRealPublishedUs`) are
`std::atomic` because they are touched by both the present thread and the
control thread.

### Clock circularity (caveat)

Because the clock is *derived from* presentation and the producer *schedules
against* it, there is a feedback loop. A clock that only advances when a flip
occurs is fragile between flips. For this single-producer demo it is workable; a
more robust design would give the time source a free-running fallback
(continuously sampled PTIMER or `system_time`) rather than advancing only at
flip boundaries.

---

## The present thread

`BlitAndFlip` blocks on the GPU copy semaphore, and `PollFlipNotifierTimestamp`
touches the kernel event queue. Running those on the `BMediaEventLooper` control
thread would stall **both** event dispatch and `BufferReceived` (they share that
thread), so frames pile up at the control port and get dropped as late. The demo
therefore offloads all blocking GPU work to a dedicated thread.

**Control thread (`HandleEvent`)** — non-blocking:
run-state / ownership / late-drop checks, then hand the frame to the single-slot
mailbox (show-latest) and return.

**Present thread (`PresentThread` → `PresentFrame`)** — blocking GPU work:
wait on the mailbox semaphore, grab the pending frame, `BlitAndFlip`, recycle the
buffer, `PollFlipNotifierTimestamp`, and `PublishTime`.

### Synchronisation invariants

- The mailbox semaphore is released **only on an empty → non-empty transition**,
  so its count never exceeds the single pending slot; a `buffer == nullptr`
  guard absorbs the quit-wake.
- Every buffer is recycled **exactly once**: via late-drop, ownership-fail,
  displacement in the slot, the present thread after blitting, or
  `StopPresentThread` draining a never-grabbed frame.
- **Teardown order** guarantees the present thread never touches a freed buffer
  group: the control thread is stopped (no new hand-offs) → the present thread is
  joined → `fBuffers` is destroyed. `StopPresentThread` is idempotent so both
  `Disconnected` and the destructor can call it.

---

## Startup and stalls: timeline re-anchoring

A naive frame thread anchored to a base captured at startup will, after
first-keyframe decode latency, find every early frame "already due" and deliver a
**burst** of dozens of late frames that the consumer simply drops. The producer
guards against this:

```cpp
if (waitUntil < system_time() - kResyncThresholdUs) {   // 100 ms behind
    fPerformanceTimeBase = TimeSource()->PerformanceTimeFor(system_time());
    fFrameBase = fFrame;
    framePerfTime = fPerformanceTimeBase;
    waitUntil = TimeSource()->RealTimeFor(framePerfTime, lead);
}
```

When the thread falls more than `kResyncThresholdUs` (100 ms) behind its
schedule, it **re-anchors the timeline to "now"** instead of chasing a stale
schedule. Crucially, no decoded frame is skipped — decode order stays
sequential; the mapping from "the frame I am decoding now" to "present it now" is
simply reset. The same mechanism absorbs both the startup gap and mid-stream
decode stalls, and is threshold-gated so it never fires during normal steady
state (where the thread self-paces to `waitUntil ≈ now`).

---

## End-to-end frame walkthrough

1. **Decode + stamp.** The producer's frame thread wakes `lead` before frame
   *N*'s presentation time, decodes into a GART buffer, stamps it
   `start_time = framePerfTime(N)`, and `SendBuffer`s it.
2. **Receive + enqueue.** The consumer's `BufferReceived` adds a
   `B_HANDLE_BUFFER` event at `start_time`. The buffer waits in the looper's
   queue.
3. **Dispatch.** The looper dispatches `HandleEvent` `fEventLatency` before
   `start_time`.
4. **Gate + hand off.** `HandleEvent` drops the frame if it is already late;
   otherwise it places it in the present mailbox (recycling any older pending
   frame) and signals the present thread.
5. **Blit + flip.** The present thread blits GART → VRAM (blocking on the copy
   engine), recycles the buffer, and submits an NVKMS flip that latches at the
   next vsync.
6. **Publish the clock.** It reads the completed flip's PTIMER timestamp and
   `PublishTime`s a new `(performance, real)` anchor, advancing the master clock
   the producer schedules against.

---

## Known limitations

- **Measured blit feedback is not wired.** `fBlitBudgetUs` is a constant; the
  present thread could time `BlitAndFlip` and refine the latency via
  `SetEventLatency`, but that also requires propagating the latency change
  upstream, so it is left as a follow-up.
- **Buffer depth (5) is a starting point**, tuned by reasoning rather than
  measurement.
- **Clock circularity** (above) remains the deepest design question; the
  PTIMER-from-flip clock has no free-running fallback between flips.
- **No audio**, no seeking UI, no format re-negotiation mid-stream
  (`FormatChanged` deliberately rejects).
- **Show-latest dropping** is correct for real-time playback but means a slow
  GPU silently drops frames rather than slowing playback.
