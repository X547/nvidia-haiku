/*
 * RusdSource.h - RAII access to the GPU's NV00DE_SHARED_DATA (RUSD)
 *
 * Opens /dev/graphics/nvidia_gsp through nvrm_cpp_sdk (the same surface the
 * NvRmDemos use), allocates an RM_USER_SHARED_DATA object on the subdevice with
 * every polled data class requested, and maps it into the address space. The
 * mapped NV00DE_SHARED_DATA is then read field-by-field with the sequence
 * protocol the class header documents.
 *
 * Construction never throws: if the driver is missing or RUSD is unsupported
 * the object stays "unavailable" and IsAvailable() returns false.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string.h>
#include <vector>

#include <NvRmApi.h>
#include <NvRmDevice.h>

extern "C" {
#include "class/cl00de.h"   // RM_USER_SHARED_DATA, NV00DE_SHARED_DATA, RUSD_*
}


// Per-process GPU client info gathered from NV2080_CTRL_CMD_GPU_GET_PIDS +
// NV2080_CTRL_CMD_GPU_GET_PID_INFO. VRAM figures are in bytes.
struct GpuClientInfo {
	uint32_t pid;
	uint64_t vramTotal;     // private + shared (owned + duped)
	uint64_t vramPrivate;   // exclusively owned by this process
	uint64_t vramShared;    // shared owned + shared duped
};


class RusdSource {
public:
	RusdSource();

	// True once RUSD has been allocated and mapped. When false the GPU (or its
	// RUSD support) is unavailable and the error string explains why.
	bool IsAvailable() const { return fMapped != nullptr; }
	const char* Error() const { return fError.c_str(); }

	// The mapped shared-data block. Only valid while IsAvailable() is true.
	const NV00DE_SHARED_DATA* Data() const { return fMapped; }

	// Enumerate the processes that hold objects on this GPU and their VRAM
	// usage. Returns false (and leaves `out` empty) if the device is not open
	// or the RM control calls fail. An empty list with a true return simply
	// means no client processes were found.
	bool QueryClients(std::vector<GpuClientInfo>& out) const;

	// Sequence-checked snapshot of one top-level RUSD member into `out`. `field`
	// must be a sub-struct whose first member is `volatile NvU64
	// lastModifiedTimestamp` (every member of NV00DE_SHARED_DATA qualifies).
	// Returns false if the data is invalid/unsupported or could not be read
	// consistently within a bounded number of retries.
	template<typename T>
	bool Read(const T& field, T& out) const
	{
		if (fMapped == nullptr)
			return false;

		for (int attempt = 0; attempt < 10; attempt++) {
			NvU64 seq = field.lastModifiedTimestamp;
			__sync_synchronize();
			if (!RUSD_SEQ_DATA_VALID(seq))
				continue;
			memcpy(&out, (const void*)&field, sizeof(T));
			__sync_synchronize();
			if (seq == field.lastModifiedTimestamp)
				return true;
		}
		return false;
	}

private:
	// Declaration order matters: fRusd / fMapping reference the RM client owned
	// by fRm / fRmDev and must be torn down before them. Members destruct in
	// reverse declaration order, so the clients are listed first.
	std::unique_ptr<NvRmApi> fRm;
	std::unique_ptr<NvRmDevice> fRmDev;
	NvRmObject fRusd;
	NvRmMemoryMapping fMapping;
	NV00DE_SHARED_DATA* fMapped;
	std::string fError;
};
