/*
 * nvidia_gpu_stats.h - Shared GPU statistics structure
 *
 * This header defines the structure used to share GPU metrics between
 * the nvidia_gsp accelerant and nvidia-info monitoring tool.
 *
 * The accelerant creates a shared Haiku area named "nvidia_gpu_stats"
 * containing this structure. nvidia-info clones this area to read
 * real-time GPU metrics.
 */

#ifndef NVIDIA_GPU_STATS_H
#define NVIDIA_GPU_STATS_H

#include <SupportDefs.h>

#define NVIDIA_GPU_STATS_AREA_NAME "nvidia_gpu_stats"
#define NVIDIA_GPU_STATS_MAGIC 0x4E564753  // "NVGS"
#define NVIDIA_GPU_STATS_VERSION 1

// Flags indicating which fields contain valid data
#define NVIDIA_STATS_VALID_TEMP       (1 << 0)
#define NVIDIA_STATS_VALID_GPU_UTIL   (1 << 1)
#define NVIDIA_STATS_VALID_MEM_UTIL   (1 << 2)
#define NVIDIA_STATS_VALID_GPU_CLOCK  (1 << 3)
#define NVIDIA_STATS_VALID_MEM_CLOCK  (1 << 4)
#define NVIDIA_STATS_VALID_POWER      (1 << 5)
#define NVIDIA_STATS_VALID_MEM_USED   (1 << 6)
#define NVIDIA_STATS_VALID_MEM_TOTAL  (1 << 7)

typedef struct {
    // Header
    uint32 magic;           // Must be NVIDIA_GPU_STATS_MAGIC
    uint32 version;         // Structure version
    uint32 validMask;       // Bitmask of valid fields
    uint32 updateCount;     // Incremented each update

    // Temperature (NvTemp format: 1/256 degree Celsius units)
    int32 gpuTemp;

    // Utilization (0-100%)
    uint32 gpuUtil;
    uint32 memUtil;

    // Clock speeds in MHz
    uint32 gpuClockMHz;
    uint32 memClockMHz;

    // Power in milliwatts
    uint32 powerMw;

    // Memory usage in bytes
    uint64 memUsed;
    uint64 memTotal;

    // Timestamp of last update (system_time())
    bigtime_t lastUpdate;

    // Reserved for future use
    uint32 reserved[16];
} nvidia_gpu_stats_t;

#endif // NVIDIA_GPU_STATS_H
