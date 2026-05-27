#include "NvCommandBuffer.h"

#include <cassert>
#include <iterator>
#include <utility>

#include "NvChannel.h"


NvCommandBuffer::NvCommandBuffer(NvChannel &channel):
	fChannel(&channel)
{
}


NvCommandBuffer::ChunkIt NvCommandBuffer::CircularNext(ChunkIt it)
{
	ChunkIt n = std::next(it);
	return (n == fRing.end()) ? fRing.begin() : n;
}


void NvCommandBuffer::EnsureInited()
{
	if (fInited) {
		return;
	}
	fRing.emplace_back();
	fRing.back().mem.AllocSystem(fChannel->Device(), fChannel->VaSpace(),
		kChunkBytes, 0x1000, true);

	ChunkIt first = fRing.begin();
	fRead   = Cursor{first, 0};
	fSubmit = Cursor{first, 0};
	fWrite  = Cursor{first, 0};
	fSpanStartOff = 0;
	fInited = true;
}


void NvCommandBuffer::CloseOpenSpan()
{
	if (!fInited || fWrite.off <= fSpanStartOff) {
		return;
	}
	fPending.push_back(NvCmdEntry{
		.gpuAddr = fWrite.chunk->mem.GpuAddr() + (NvU64)fSpanStartOff * sizeof(NvU32),
		.lengthDwords = fWrite.off - fSpanStartOff,
	});
	fSpanStartOff = fWrite.off;
}


void NvCommandBuffer::AdvanceWriteChunk()
{
	// Freeze the current chunk's span before leaving it; its tail (from
	// fWrite.off to the chunk end) is simply left unused.
	CloseOpenSpan();

	// The reader may have retired submissions and freed chunks ahead of us.
	Reclaim();

	ChunkIt next = CircularNext(fWrite.chunk);
	if (next == fRead.chunk) {
		// Ring full: every chunk from the writer around to the reader is
		// still in flight. Splice a fresh chunk in rather than stalling the
		// producer.
		ChunkIt pos = std::next(fWrite.chunk);
		next = fRing.insert(pos, NvCommandChunk{});
		next->mem.AllocSystem(fChannel->Device(), fChannel->VaSpace(),
			kChunkBytes, 0x1000, true);
	}

	fWrite = Cursor{next, 0};
	fSpanStartOff = 0;
}


void NvCommandBuffer::Reclaim()
{
	while (!fSubmits.empty() && fChannel->SeqReached(fSubmits.front().gpTotal)) {
		fRead = fSubmits.front().end;
		fSubmits.pop_front();
	}
}


NvPushWriter NvCommandBuffer::Push(NvU32 dwMax)
{
	assert(dwMax <= kMaxPushDwords);
	EnsureInited();
	Reclaim();

	if (dwMax > kChunkDwords - fWrite.off) {
		AdvanceWriteChunk();
	}

	NvU32 *base = (NvU32*)fWrite.chunk->mem.CpuAddr();
	return NvPushWriter(base + fWrite.off, base + fWrite.off + dwMax);
}


void NvCommandBuffer::EndPush(const NvPushWriter &wr)
{
	NvU32 *base = (NvU32*)fWrite.chunk->mem.CpuAddr();
	assert(wr.Ptr() >= base + fWrite.off);
	assert(wr.Ptr() <= base + kChunkDwords);
	fWrite.off = (NvU32)(wr.Ptr() - base);
}


void NvCommandBuffer::Reset()
{
	if (!fInited) {
		return;
	}
	// Discard everything written since the last submit: rewind the writer to
	// the submit boundary. Chunks the writer touched while building the
	// dropped batch become free space ahead of it again.
	fPending.clear();
	fWrite = fSubmit;
	fSpanStartOff = fSubmit.off;
}


void NvCommandBuffer::SubmitTo(NvChannel &channel)
{
	if (!fInited) {
		return;
	}
	CloseOpenSpan();
	if (fPending.empty()) {
		return;
	}

	NvU32 gpTotal = channel.Submit(fPending.data(), fPending.size());
	fPending.clear();

	// The next batch continues writing where this one stopped; a later write
	// in the same chunk opens a fresh span starting at fWrite.off.
	fSubmit = fWrite;
	fSpanStartOff = fWrite.off;
	fSubmits.push_back(SubmitRec{gpTotal, fWrite});
}
