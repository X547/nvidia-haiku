/*
 * NvGpuStats.h - GPU statistics monitoring via RUSD
 *
 * This class allocates RUSD (Runtime User-Space Data) from NVIDIA driver
 * and exposes it through a shared Haiku area for nvidia-info to read.
 */

#pragma once

#include <OS.h>
#include <NvRmDevice.h>

extern "C" {
#include "class/cl00de.h"   // RM_USER_SHARED_DATA, NV00DE_SHARED_DATA
#include "ctrl/ctrl2080/ctrl2080fb.h"  // FB_GET_INFO
}

#include "nvidia_gpu_stats.h"

class NvGpuStats {
public:
    NvGpuStats(NvRmDevice &rmDev);
    ~NvGpuStats();

    // Update the shared stats area from RUSD
    void Update();

    // Check if RUSD is available
    bool IsAvailable() const { return fRusdMapped != nullptr; }

private:
    NvRmDevice &fRmDev;
    NvRmObject fRusd;
    NvRmMemoryMapping fRusdMapping;
    NV00DE_SHARED_DATA* fRusdMapped;

    // Shared Haiku area for external access
    area_id fSharedArea;
    nvidia_gpu_stats_t* fSharedStats;

    // Polling thread
    thread_id fPollThread;
    volatile bool fRunning;

    static int32 PollThreadEntry(void* data);
    int32 PollThread();

    // Read RUSD data with sequence checking
    bool ReadRusdTemperature(NvTemp* outTemp);
    bool ReadRusdUtilization(uint32* gpuUtil, uint32* memUtil);
    bool ReadRusdClocks(uint32* gpuClock, uint32* memClock);
    bool ReadRusdPower(uint32* powerMw);
    
    // Query VRAM usage via RM control
    bool QueryVramUsage(uint64* used, uint64* total);
};
