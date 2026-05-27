#pragma once

#include <cstddef>
#include <deque>
#include <list>
#include <vector>

#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvChannel.h"
#include "NvGpuMemory.h"


// One fixed-size sysmem allocation. Chunks are never freed individually;
// collectively they form the command buffer's logical ring (see below).
struct NvCommandChunk {
	NvGpuMemory mem;
};


// A command buffer whose backing store is a *logical ring of chunks*. Each
// chunk is a separate GART allocation, but the list of chunks is treated as
// one circular buffer with a reader end (how far the GPU has consumed) and a
// writer end (how far the CPU has written). Methods are appended via the
// bounded Push() / EndPush() API, flowing linearly from one chunk into the
// next; on SubmitTo() the span written since the previous submit is cut into
// GP entries (at chunk and submit boundaries, since each chunk is a distinct
// GPU VA) behind one doorbell ring.
//
// Reclamation is driven by the channel's progress tracker, not by a CPU wait:
// as the GPU retires submissions the reader end advances, freeing the chunks
// behind it for the writer to wrap back into. If the writer would lap the
// reader, a new chunk is spliced into the ring rather than stalling — GPFIFO
// depth (enforced in NvChannel::Submit) is the real backpressure. Multiple
// submissions may therefore be in flight without waiting between them.
class NvCommandBuffer {
public:
	static constexpr NvU64 kChunkBytes    = 0x10000;          // 64 KiB
	static constexpr NvU32 kChunkDwords   = (NvU32)(kChunkBytes / sizeof(NvU32));
	// Mirrors NVK_CMD_BUFFER_MAX_PUSH: the largest single Push() reservation
	// any caller may request. Compositions larger than this must be broken
	// into multiple Push()/EndPush() regions so chunk swaps can happen
	// between them.
	static constexpr NvU32 kMaxPushDwords = 512;

private:
	NvChannel *fChannel {};

	// Circular list of chunk allocations. Iterators are stable for the
	// buffer's lifetime (we only ever insert, never erase), so the cursors
	// below hold them directly.
	std::list<NvCommandChunk> fRing;
	using ChunkIt = std::list<NvCommandChunk>::iterator;

	// A position in the ring: a chunk plus a dword offset within it.
	struct Cursor {
		ChunkIt chunk {};
		NvU32 off {};
	};

	Cursor fRead {};    // host-consumed boundary; advanced by Reclaim()
	Cursor fSubmit {};  // GP-emitted boundary; advanced by SubmitTo()
	Cursor fWrite {};   // CPU write cursor; advanced by Push()/EndPush()
	bool fInited {};    // false until the first chunk is allocated

	// GP-entry spans written since the last SubmitTo (chunk fragments cut at
	// chunk and submit boundaries), plus the offset at which the currently
	// open span began in fWrite.chunk.
	std::vector<NvCmdEntry> fPending;
	NvU32 fSpanStartOff {};

	// One record per outstanding submission: the GP total it was assigned
	// (returned by NvChannel::Submit) and the write position at that point,
	// so Reclaim() can map a consumed GP total back to a reader position.
	struct SubmitRec {
		NvU32 gpTotal {};
		Cursor end {};
	};
	std::deque<SubmitRec> fSubmits;

	ChunkIt CircularNext(ChunkIt it);
	void EnsureInited();
	void CloseOpenSpan();
	void AdvanceWriteChunk();
	void Reclaim();

public:
	NvCommandBuffer() = default;
	NvCommandBuffer(NvCommandBuffer &&) = default;
	NvCommandBuffer &operator=(NvCommandBuffer &&) = default;
	NvCommandBuffer(const NvCommandBuffer &) = delete;
	NvCommandBuffer &operator=(const NvCommandBuffer &) = delete;

	// Bind to the channel whose VA space (and device) chunks are allocated
	// in, and whose progress tracker drives reclamation. The first chunk is
	// allocated lazily on first write — construction itself does no GPU work.
	explicit NvCommandBuffer(NvChannel &channel);

	// Reserve `dwMax` dwords (must be <= kMaxPushDwords). If the current
	// chunk wouldn't fit the reservation, advance to the next chunk in the
	// ring (reused if the reader has freed it, otherwise a fresh chunk is
	// spliced in). Returns a bounded NvPushWriter — caller commits the actual
	// dwords used via EndPush.
	NvPushWriter Push(NvU32 dwMax);

	// Commit a writer returned by Push(): records the actual dwords used.
	void EndPush(const NvPushWriter &wr);

	// Drop all data written since the last SubmitTo without submitting it;
	// the writer rewinds to the submit boundary. In-flight chunks are
	// untouched.
	void Reset();

	// Submit everything written since the last SubmitTo as GP entries behind
	// one doorbell ring. Does not wait: the chunks stay owned until the GPU
	// retires them and a later Reclaim() advances the reader. No-op if
	// nothing has been written.
	void SubmitTo(NvChannel &channel);
};
