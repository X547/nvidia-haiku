#pragma once

#include <vector>
#include <memory>

#include <Buffer.h>
#include <BufferGroup.h>
#include <MediaDefs.h>

extern "C" {
#include "nvtypes.h"
}

#include "NvRmDevice.h"

#include <nvrm-helpers/NvGpuMemory.h>
#include <nvrm-helpers/NvChannel.h>


// A BBufferGroup whose underlying buffers live in NVRM GART (sysmem)
// allocations mapped into the copy-engine channel's VA space. The CPU
// mapping is published to Media Kit via buffer_clone_info{area_id}, so the
// producer writes into pages the GPU can DMA from directly — no copy on
// the producer's side, only a CE-driven GART -> VRAM copy at flip time on
// ours.
class GartBufferGroup {
private:
	struct Buf {
		NvGpuMemory mem;
		BBuffer *buffer; // owned by the BBufferGroup, weak pointer here.
		media_buffer_id id;
	};

	std::unique_ptr<BBufferGroup> fGroup;
	std::vector<Buf> fBuffers;
	NvU32 fWidth;
	NvU32 fHeight;
	NvU32 fBytesPerRow;

public:
	GartBufferGroup() = default;
	GartBufferGroup(const GartBufferGroup &) = delete;
	GartBufferGroup &operator=(const GartBufferGroup &) = delete;

	// Allocate `count` GART blocks of `bytesPerRow*height` bytes each,
	// mapped into `channel`'s VA space, then build a BBufferGroup whose
	// buffers reference those allocations by area_id. Throws on failure.
	GartBufferGroup(NvRmDevice &rmDev, NvChannel &channel,
		NvU32 width, NvU32 height, NvU32 bytesPerRow, int32 count);

	BBufferGroup *Group() {return fGroup.get();}

	NvU32 Width() const {return fWidth;}
	NvU32 Height() const {return fHeight;}
	NvU32 BytesPerRow() const {return fBytesPerRow;}
	NvU64 BufferSize() const {return (NvU64)fBytesPerRow * fHeight;}

	// GPU virtual address of the GART backing of the BBuffer with the given
	// media_buffer_id, or 0 if the id isn't one of ours.
	NvU64 GpuAddrFor(media_buffer_id id) const;

	// True if `id` references one of the buffers we created.
	bool Owns(media_buffer_id id) const;
};
