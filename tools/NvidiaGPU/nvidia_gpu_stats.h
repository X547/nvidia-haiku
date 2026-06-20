/*
 * nvidia_gpu_stats.h - GPU statistics snapshot
 *
 * Plain data container for the GPU metrics shown by the NvidiaGPU monitor.
 * It is filled by GpuStats, which reads the values straight from the NVIDIA
 * RM via RUSD (Runtime User-Space Data) using nvrm_cpp_sdk -- the monitor no
 * longer clones a shared Haiku area published by the accelerant.
 */

#ifndef NVIDIA_GPU_STATS_H
#define NVIDIA_GPU_STATS_H

#include <SupportDefs.h>

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
    // Bitmask of valid fields (NVIDIA_STATS_VALID_*)
    uint32 validMask;

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
} nvidia_gpu_stats_t;

#endif // NVIDIA_GPU_STATS_H
