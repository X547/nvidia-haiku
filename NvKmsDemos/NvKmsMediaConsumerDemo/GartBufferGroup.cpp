#include "GartBufferGroup.h"

#include <stdio.h>
#include <string.h>

#include <system_error>

#include "ErrorUtils.h"


GartBufferGroup::GartBufferGroup(NvRmDevice &rmDev, NvChannel &channel,
	NvU32 width, NvU32 height, NvU32 bytesPerRow, int32 count)
	: fWidth(width), fHeight(height), fBytesPerRow(bytesPerRow)
{
	const NvU64 size = (NvU64)bytesPerRow * height;

	fGroup = std::make_unique<BBufferGroup>();
	status_t status = fGroup->InitCheck();
	if (status != B_OK) {
		throw std::system_error(status, std::generic_category(),
			"BBufferGroup InitCheck failed");
	}

	fBuffers.reserve(count);
	for (int32 i = 0; i < count; i++) {
		Buf buf {};
		buf.mem.AllocSystem(rmDev, channel.VaSpace(), size, 0x1000, /*mapCpu=*/true);
		// Zero so the producer sees deterministic state on first decode.
		memset(buf.mem.CpuAddr(), 0, size);

		buffer_clone_info info {};
		info.area = buf.mem.CpuMap().Area();
		info.offset = 0;
		info.size = (size_t)size;
		info.flags = i;

		BBuffer *b = nullptr;
		status = fGroup->AddBuffer(info, &b);
		if (status != B_OK || b == nullptr) {
			throw std::system_error(status == B_OK ? B_ERROR : status,
				std::generic_category(), "BBufferGroup::AddBuffer failed");
		}
		buf.buffer = b;
		buf.id = b->ID();
		fBuffers.push_back(std::move(buf));
	}
}


NvU64 GartBufferGroup::GpuAddrFor(media_buffer_id id) const
{
	for (const auto &b : fBuffers) {
		if (b.id == id) {
			return b.mem.GpuAddr();
		}
	}
	return 0;
}


bool GartBufferGroup::Owns(media_buffer_id id) const
{
	for (const auto &b : fBuffers) {
		if (b.id == id) {
			return true;
		}
	}
	return false;
}
