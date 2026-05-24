/*
 * NvGpuStats.cpp - GPU statistics monitoring via RUSD
 */

#include "NvGpuStats.h"

#include <stdio.h>
#include <string.h>
#include <exception>
#include <OS.h>  // For debug_printf

#define TRACE(x...) /*debug_printf("NvGpuStats: " x)*/
#define TRACE_ALWAYS(x...) debug_printf("NvGpuStats: " x)

NvGpuStats::NvGpuStats(NvRmDevice &rmDev)
    : fRmDev(rmDev)
    , fRusdMapped(nullptr)
    , fSharedArea(-1)
    , fSharedStats(nullptr)
    , fPollThread(-1)
    , fRunning(false)
{
    TRACE_ALWAYS("Initializing GPU stats...\n");

    // Allocate RUSD object with polling for all data types
    NV00DE_ALLOC_PARAMETERS rusdParams = {};
    rusdParams.polledDataMask = NV00DE_RUSD_POLL_CLOCK |
                                NV00DE_RUSD_POLL_PERF |
                                NV00DE_RUSD_POLL_THERMAL |
                                NV00DE_RUSD_POLL_POWER;

    try {
        fRusd = rmDev.Subdevice().Alloc(RM_USER_SHARED_DATA, &rusdParams, sizeof(rusdParams));
        TRACE_ALWAYS("RUSD object allocated: handle=0x%x\n", fRusd.Get());

        // Map RUSD memory
        fRusdMapping = rmDev.MapMemory(fRusd.Get(), true, 0, sizeof(NV00DE_SHARED_DATA), 0);
        fRusdMapped = (NV00DE_SHARED_DATA*)fRusdMapping.Address();
        TRACE_ALWAYS("RUSD mapped at %p\n", fRusdMapped);
    } catch (const std::exception& e) {
        TRACE_ALWAYS("Failed to allocate RUSD: %s\n", e.what());
        fRusdMapped = nullptr;
        return;
    }

    if (!fRusdMapped) {
        TRACE_ALWAYS("RUSD mapping failed\n");
        return;
    }

    // Create shared Haiku area for external access
    size_t areaSize = (sizeof(nvidia_gpu_stats_t) + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1);
    fSharedArea = create_area(NVIDIA_GPU_STATS_AREA_NAME,
                              (void**)&fSharedStats,
                              B_ANY_ADDRESS,
                              areaSize,
                              B_NO_LOCK,
                              B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);

    if (fSharedArea < 0) {
        TRACE_ALWAYS("Failed to create shared area: %s\n", strerror(fSharedArea));
        return;
    }

    TRACE_ALWAYS("Shared area created: id=%d addr=%p\n", fSharedArea, fSharedStats);

    // Initialize shared stats structure
    memset(fSharedStats, 0, sizeof(nvidia_gpu_stats_t));
    fSharedStats->magic = NVIDIA_GPU_STATS_MAGIC;
    fSharedStats->version = NVIDIA_GPU_STATS_VERSION;

    // Do initial update
    Update();

    // Start polling thread
    fRunning = true;
    fPollThread = spawn_thread(PollThreadEntry, "nvidia_gpu_stats",
                               B_LOW_PRIORITY, this);
    if (fPollThread >= 0) {
        resume_thread(fPollThread);
        TRACE_ALWAYS("Polling thread started\n");
    }
}

NvGpuStats::~NvGpuStats()
{
    TRACE_ALWAYS("Shutting down GPU stats...\n");

    // Stop polling thread
    if (fPollThread >= 0) {
        fRunning = false;
        status_t status;
        wait_for_thread(fPollThread, &status);
    }

    // Delete shared area
    if (fSharedArea >= 0) {
        delete_area(fSharedArea);
    }
}

int32 NvGpuStats::PollThreadEntry(void* data)
{
    return ((NvGpuStats*)data)->PollThread();
}

int32 NvGpuStats::PollThread()
{
    TRACE("Polling thread running\n");

    while (fRunning) {
        // Poll every 500ms
        snooze(500000);

        if (fRunning) {
            Update();
        }
    }

    TRACE("Polling thread exiting\n");
    return 0;
}

void NvGpuStats::Update()
{
    if (!fRusdMapped || !fSharedStats)
        return;

    uint32 validMask = 0;

    // Read temperature
    NvTemp temp = 0;
    if (ReadRusdTemperature(&temp)) {
        fSharedStats->gpuTemp = temp;
        validMask |= NVIDIA_STATS_VALID_TEMP;
    }

    // Read utilization
    uint32 gpuUtil = 0, memUtil = 0;
    if (ReadRusdUtilization(&gpuUtil, &memUtil)) {
        fSharedStats->gpuUtil = gpuUtil;
        fSharedStats->memUtil = memUtil;
        validMask |= NVIDIA_STATS_VALID_GPU_UTIL | NVIDIA_STATS_VALID_MEM_UTIL;
    }

    // Read clocks
    uint32 gpuClock = 0, memClock = 0;
    if (ReadRusdClocks(&gpuClock, &memClock)) {
        fSharedStats->gpuClockMHz = gpuClock;
        fSharedStats->memClockMHz = memClock;
        validMask |= NVIDIA_STATS_VALID_GPU_CLOCK | NVIDIA_STATS_VALID_MEM_CLOCK;
    }

    // Read power
    uint32 powerMw = 0;
    if (ReadRusdPower(&powerMw)) {
        fSharedStats->powerMw = powerMw;
        validMask |= NVIDIA_STATS_VALID_POWER;
    }

    // Read VRAM usage
    uint64 vramUsed = 0, vramTotal = 0;
    if (QueryVramUsage(&vramUsed, &vramTotal)) {
        fSharedStats->memUsed = vramUsed;
        fSharedStats->memTotal = vramTotal;
        validMask |= NVIDIA_STATS_VALID_MEM_USED | NVIDIA_STATS_VALID_MEM_TOTAL;
    }

    // Update metadata
    fSharedStats->validMask = validMask;
    fSharedStats->lastUpdate = system_time();
    fSharedStats->updateCount++;
}

bool NvGpuStats::ReadRusdTemperature(NvTemp* outTemp)
{
    if (!fRusdMapped)
        return false;

    RUSD_TEMPERATURE temp;
    for (int attempts = 0; attempts < 10; attempts++) {
        NvU64 seq = fRusdMapped->temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp;
        __sync_synchronize();
        if (!RUSD_SEQ_DATA_VALID(seq))
            continue;
        memcpy(&temp, (void*)&fRusdMapped->temperatures[RUSD_TEMPERATURE_TYPE_GPU], sizeof(temp));
        __sync_synchronize();
        if (seq == fRusdMapped->temperatures[RUSD_TEMPERATURE_TYPE_GPU].lastModifiedTimestamp) {
            *outTemp = temp.temperature;
            return true;
        }
    }
    return false;
}

bool NvGpuStats::ReadRusdUtilization(uint32* gpuUtil, uint32* memUtil)
{
    if (!fRusdMapped)
        return false;

    RUSD_PERF_DEVICE_UTILIZATION util;
    for (int attempts = 0; attempts < 10; attempts++) {
        NvU64 seq = fRusdMapped->perfDevUtil.lastModifiedTimestamp;
        __sync_synchronize();
        if (!RUSD_SEQ_DATA_VALID(seq))
            continue;
        memcpy(&util, (void*)&fRusdMapped->perfDevUtil, sizeof(util));
        __sync_synchronize();
        if (seq == fRusdMapped->perfDevUtil.lastModifiedTimestamp) {
            *gpuUtil = util.info.gpuPercentBusy;
            *memUtil = util.info.memoryPercentBusy;
            return true;
        }
    }
    return false;
}

bool NvGpuStats::ReadRusdClocks(uint32* gpuClock, uint32* memClock)
{
    if (!fRusdMapped)
        return false;

    RUSD_CLK_PUBLIC_DOMAIN_INFOS clocks;
    for (int attempts = 0; attempts < 10; attempts++) {
        NvU64 seq = fRusdMapped->clkPublicDomainInfos.lastModifiedTimestamp;
        __sync_synchronize();
        if (!RUSD_SEQ_DATA_VALID(seq))
            continue;
        memcpy(&clocks, (void*)&fRusdMapped->clkPublicDomainInfos, sizeof(clocks));
        __sync_synchronize();
        if (seq == fRusdMapped->clkPublicDomainInfos.lastModifiedTimestamp) {
            *gpuClock = clocks.info[RUSD_CLK_PUBLIC_DOMAIN_GRAPHICS].targetClkMHz;
            *memClock = clocks.info[RUSD_CLK_PUBLIC_DOMAIN_MEMORY].targetClkMHz;
            return true;
        }
    }
    return false;
}

bool NvGpuStats::ReadRusdPower(uint32* powerMw)
{
    if (!fRusdMapped)
        return false;

    RUSD_INST_POWER_USAGE power;
    for (int attempts = 0; attempts < 10; attempts++) {
        NvU64 seq = fRusdMapped->instPowerUsage.lastModifiedTimestamp;
        __sync_synchronize();
        if (!RUSD_SEQ_DATA_VALID(seq))
            continue;
        memcpy(&power, (void*)&fRusdMapped->instPowerUsage, sizeof(power));
        __sync_synchronize();
        if (seq == fRusdMapped->instPowerUsage.lastModifiedTimestamp) {
            *powerMw = power.info.instGpuPower;
            return true;
        }
    }
    return false;
}

bool NvGpuStats::QueryVramUsage(uint64* used, uint64* total)
{
    try {
        NV2080_CTRL_FB_GET_INFO_V2_PARAMS fbParams = {};
        fbParams.fbInfoListSize = 2;
        fbParams.fbInfoList[0].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE;
        fbParams.fbInfoList[0].data = 0;
        fbParams.fbInfoList[1].index = NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE;
        fbParams.fbInfoList[1].data = 0;

        fRmDev.Subdevice().Control(NV2080_CTRL_CMD_FB_GET_INFO_V2, &fbParams, sizeof(fbParams));

        // Values are in KB
        uint64 heapSize = (uint64)fbParams.fbInfoList[0].data * 1024;
        uint64 heapFree = (uint64)fbParams.fbInfoList[1].data * 1024;

        *total = heapSize;
        *used = heapSize - heapFree;
        return true;
    } catch (...) {
        return false;
    }
}
