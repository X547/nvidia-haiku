/*
 * GpuStats.h - Live GPU metrics straight from the NVIDIA RM
 *
 * Allocates RUSD (Runtime User-Space Data) on the GPU subdevice and reads
 * temperature, utilization, clocks and power from it, plus VRAM usage via an
 * FB_GET_INFO control call. This talks to /dev/graphics/nvidia_gsp directly
 * through nvrm_cpp_sdk (the same surface the NvRmDemos use), so the monitor
 * does not depend on the accelerant publishing a shared area.
 */

#pragma once

#include <NvRmApi.h>
#include <NvRmDevice.h>

extern "C" {
#include "class/cl00de.h"               // RM_USER_SHARED_DATA, NV00DE_SHARED_DATA
#include "ctrl/ctrl2080/ctrl2080fb.h"   // NV2080_CTRL_CMD_FB_GET_INFO_V2
}

#include "nvidia_gpu_stats.h"


class GpuStats {
public:
	GpuStats();

	// True once RUSD has been allocated and mapped. When false, Read()
	// returns nothing useful and the caller should treat the GPU as
	// unavailable.
	bool IsAvailable() const { return fRusdMapped != nullptr; }

	// Fill `out` with the latest snapshot, setting validMask for the
	// fields that were successfully read. Returns false if RUSD is not
	// available.
	bool Read(nvidia_gpu_stats_t& out);

private:
	NvRmApi fRm;
	NvRmDevice fRmDev;
	NvRmObject fRusd;
	NvRmMemoryMapping fRusdMapping;
	NV00DE_SHARED_DATA* fRusdMapped;

	// Sequence-checked reads of individual RUSD sub-structures.
	bool ReadTemperature(NvTemp* outTemp);
	bool ReadUtilization(uint32* gpuUtil, uint32* memUtil);
	bool ReadClocks(uint32* gpuClock, uint32* memClock);
	bool ReadPower(uint32* powerMw);

	// VRAM usage via RM control (not part of RUSD).
	bool QueryVramUsage(uint64* used, uint64* total);
};
