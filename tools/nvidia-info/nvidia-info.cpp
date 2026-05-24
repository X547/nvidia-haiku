/*
 * nvidia-info - GPU monitoring tool for nvidia_gsp on Haiku
 *
 * Displays GPU information and real-time metrics by reading from
 * the shared GPU stats area created by the nvidia_gsp accelerant.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <signal.h>
#include <OS.h>

extern "C" {
#include "nvtypes.h"
#include "nv-ioctl.h"
#include "nv_escape.h"
#include "nvos.h"
#include "class/cl0000.h"  // NV01_ROOT
#include "class/cl0080.h"  // NV01_DEVICE_0
#include "class/cl2080.h"  // NV20_SUBDEVICE_0
#include "ctrl/ctrl2080/ctrl2080fb.h"  // FB_GET_INFO
}

#ifdef __HAIKU__
#include "nv-haiku.h"
#define IOCTL_BASE NV_HAIKU_BASE
#else
#define IOCTL_BASE 0
#endif

#include "nvidia_gpu_stats.h"

static volatile bool sRunning = true;

static void SignalHandler(int sig)
{
    sRunning = false;
}

static int NvIoctl(int fd, uint32_t cmd, void *params, uint32_t paramsSize)
{
    int res;
    do {
#ifdef __HAIKU__
        res = ioctl(fd, cmd + NV_HAIKU_BASE, params, paramsSize);
#else
        res = ioctl(fd, cmd, params, paramsSize);
#endif
    } while (res < 0 && (errno == EINTR || errno == EAGAIN));
    return res;
}

// RM handle management for querying GPU info
struct RmHandles {
    int fd;
    NvHandle hClient;
    NvHandle hDevice;
    NvHandle hSubdevice;
    bool valid;
};

static NvHandle sNextHandle = 0x80000001;

static NvHandle AllocHandle()
{
    return sNextHandle++;
}

static bool RmAlloc(int fd, NvHandle hClient, NvHandle hParent, NvHandle hObject,
                    NvV32 hClass, void* params, NvU32 paramsSize)
{
    NVOS21_PARAMETERS p = {};
    p.hRoot = hClient;
    p.hObjectParent = hParent;
    p.hObjectNew = hObject;
    p.hClass = hClass;
    p.pAllocParms = params;
    p.paramsSize = paramsSize;

    int ret = NvIoctl(fd, NV_ESC_RM_ALLOC, &p, sizeof(p));
    if (ret < 0 || p.status != 0) {
        return false;
    }
    return true;
}

static void RmFree(int fd, NvHandle hClient, NvHandle hObject)
{
    NVOS00_PARAMETERS p = {};
    p.hRoot = hClient;
    p.hObjectOld = hObject;
    NvIoctl(fd, NV_ESC_RM_FREE, &p, sizeof(p));
}

static bool RmControl(int fd, NvHandle hClient, NvHandle hObject,
                      NvV32 cmd, void* params, NvU32 paramsSize)
{
    NVOS54_PARAMETERS p = {};
    p.hClient = hClient;
    p.hObject = hObject;
    p.cmd = cmd;
    p.flags = 0;
    p.params = params;
    p.paramsSize = paramsSize;

    int ret = NvIoctl(fd, NV_ESC_RM_CONTROL, &p, sizeof(p));
    if (ret < 0 || p.status != 0) {
        return false;
    }
    return true;
}

static bool InitRmHandles(RmHandles* rm, int fd, uint32_t deviceId)
{
    rm->fd = fd;
    rm->valid = false;

    // Allocate client (root)
    rm->hClient = AllocHandle();
    NV0000_ALLOC_PARAMETERS clientParams = {};
    if (!RmAlloc(fd, rm->hClient, 0, rm->hClient, NV01_ROOT, &clientParams, sizeof(clientParams))) {
        return false;
    }

    // Allocate device
    rm->hDevice = AllocHandle();
    NV0080_ALLOC_PARAMETERS deviceParams = {};
    deviceParams.deviceId = deviceId;
    deviceParams.hClientShare = rm->hClient;
    if (!RmAlloc(fd, rm->hClient, rm->hClient, rm->hDevice, NV01_DEVICE_0, &deviceParams, sizeof(deviceParams))) {
        RmFree(fd, rm->hClient, rm->hClient);
        return false;
    }

    // Allocate subdevice
    rm->hSubdevice = AllocHandle();
    if (!RmAlloc(fd, rm->hClient, rm->hDevice, rm->hSubdevice, NV20_SUBDEVICE_0, NULL, 0)) {
        RmFree(fd, rm->hClient, rm->hDevice);
        RmFree(fd, rm->hClient, rm->hClient);
        return false;
    }

    rm->valid = true;
    return true;
}

static void CleanupRmHandles(RmHandles* rm)
{
    if (!rm->valid)
        return;
    RmFree(rm->fd, rm->hClient, rm->hSubdevice);
    RmFree(rm->fd, rm->hClient, rm->hDevice);
    RmFree(rm->fd, rm->hClient, rm->hClient);
    rm->valid = false;
}

// Query actual VRAM size using RM control call
static uint64_t QueryVramSize(RmHandles* rm)
{
    if (!rm->valid)
        return 0;

    NV2080_CTRL_FB_GET_INFO_V2_PARAMS fbParams = {};
    fbParams.fbInfoListSize = 1;
    fbParams.fbInfoList[0].index = NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE;
    fbParams.fbInfoList[0].data = 0;

    if (!RmControl(rm->fd, rm->hClient, rm->hSubdevice,
                   NV2080_CTRL_CMD_FB_GET_INFO_V2, &fbParams, sizeof(fbParams))) {
        return 0;
    }

    // TOTAL_RAM_SIZE is returned in KB
    return (uint64_t)fbParams.fbInfoList[0].data * 1024;
}

static const char* GetGpuName(uint16_t deviceId)
{
    switch (deviceId) {
        // RTX 40 series
        case 0x2684: return "GeForce RTX 4090";
        case 0x2704: return "GeForce RTX 4080";
        case 0x2782: return "GeForce RTX 4070 Ti";
        case 0x2786: return "GeForce RTX 4070";
        // RTX 30 series
        case 0x2204: return "GeForce RTX 3090";
        case 0x2206: return "GeForce RTX 3080";
        case 0x2208: return "GeForce RTX 3080 Ti";
        case 0x2216: return "GeForce RTX 3080 12GB";
        case 0x2484: return "GeForce RTX 3070";
        case 0x2488: return "GeForce RTX 3070 Ti";
        case 0x2504: return "GeForce RTX 3060 Ti";
        case 0x2544: return "GeForce RTX 3060";
        case 0x2560: return "GeForce RTX 3060 Mobile";
        default: return NULL;
    }
}

// Format temperature for display (NvTemp is 1/256 degree Celsius)
static const char* FormatTemp(int32_t nvTemp, char* buf, size_t bufSize)
{
    if (nvTemp == 0) {
        snprintf(buf, bufSize, "N/A");
    } else {
        snprintf(buf, bufSize, "%d C", nvTemp >> 8);
    }
    return buf;
}

// Format utilization for display
static const char* FormatUtil(uint32_t percent, char* buf, size_t bufSize)
{
    snprintf(buf, bufSize, "%u%%", percent);
    return buf;
}

// Format clock for display
static const char* FormatClock(uint32_t mhz, char* buf, size_t bufSize)
{
    if (mhz == 0) {
        snprintf(buf, bufSize, "N/A");
    } else {
        snprintf(buf, bufSize, "%u MHz", mhz);
    }
    return buf;
}

// Format power for display
static const char* FormatPower(uint32_t milliWatts, char* buf, size_t bufSize)
{
    if (milliWatts == 0) {
        snprintf(buf, bufSize, "N/A");
    } else {
        snprintf(buf, bufSize, "%.1f W", milliWatts / 1000.0);
    }
    return buf;
}

static void PrintUsage()
{
    printf("\nnvidia-info - NVIDIA GPU information tool for Haiku\n\n");
    printf("Usage: nvidia-info [options]\n");
    printf("  -l, --loop     Continuously update (1 second interval)\n");
    printf("  -h, --help     Show this help\n");
}

int main(int argc, char *argv[])
{
    bool loop = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--loop") == 0) {
            loop = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            PrintUsage();
            return 0;
        }
    }

    signal(SIGINT, SignalHandler);

    // Open NVIDIA control device
    int fd = open("/dev/nvidia-ctl", O_RDWR);
    if (fd < 0)
        fd = open("/dev/nvidiactl", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Cannot open NVIDIA control device: %s\n", strerror(errno));
        fprintf(stderr, "Make sure the nvidia_gsp driver is loaded.\n");
        return 1;
    }

    // Get card info
    nv_ioctl_card_info_t cards[8] = {};
    int ret = NvIoctl(fd, NV_ESC_CARD_INFO, cards, sizeof(cards));
    if (ret < 0) {
        fprintf(stderr, "Cannot get card info: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    // Initialize RM handles for each GPU and query actual VRAM
    RmHandles rmHandles[8] = {};
    uint64_t actualVram[8] = {};

    for (int i = 0; i < 8; i++) {
        if (cards[i].valid) {
            if (InitRmHandles(&rmHandles[i], fd, i)) {
                actualVram[i] = QueryVramSize(&rmHandles[i]);
            }
        }
    }

    // Try to find and clone the shared GPU stats area from the accelerant
    area_id statsAreaId = find_area(NVIDIA_GPU_STATS_AREA_NAME);
    nvidia_gpu_stats_t* stats = NULL;
    area_id clonedArea = -1;

    if (statsAreaId >= 0) {
        clonedArea = clone_area("nvidia_gpu_stats_clone",
                                (void**)&stats,
                                B_ANY_ADDRESS,
                                B_READ_AREA,
                                statsAreaId);
        if (clonedArea >= 0 && stats != NULL) {
            if (stats->magic != NVIDIA_GPU_STATS_MAGIC) {
                fprintf(stderr, "Warning: GPU stats area has invalid magic\n");
                delete_area(clonedArea);
                clonedArea = -1;
                stats = NULL;
            }
        }
    }

    do {
        if (loop) {
            printf("\033[2J\033[H");  // Clear screen
        }

        // Box width: 58 chars inside (60 total with borders)
        printf("+----------------------------------------------------------+\n");
        printf("|             nvidia-info - GPU Monitor                    |\n");
        printf("+----------------------------------------------------------+\n\n");

        int gpuCount = 0;
        for (int i = 0; i < 8; i++) {
            if (cards[i].valid) {
                gpuCount++;
                uint16_t deviceId = cards[i].pci_info.device_id;
                const char* name = GetGpuName(deviceId);

                // Print GPU header line with dynamic padding
                char header[80];
                if (name) {
                    snprintf(header, sizeof(header), "+-- GPU %d (%s) ", i, name);
                } else {
                    snprintf(header, sizeof(header), "+-- GPU %d ", i);
                }
                int headerLen = strlen(header);
                printf("%s", header);
                for (int p = headerLen; p < 59; p++) printf("-");
                printf("+\n");

                // Print info lines (58 chars inside)
                char lineBuf[80];

                snprintf(lineBuf, sizeof(lineBuf), "  PCI:         %04x:%02x:%02x.%x",
                         cards[i].pci_info.domain, cards[i].pci_info.bus,
                         cards[i].pci_info.slot, cards[i].pci_info.function);
                printf("|%-58s|\n", lineBuf);

                snprintf(lineBuf, sizeof(lineBuf), "  Device ID:   0x%04x (vendor: 0x%04x)",
                         deviceId, cards[i].pci_info.vendor_id);
                printf("|%-58s|\n", lineBuf);

                printf("|%-58s|\n", "");

                // Read GPU stats from shared area
                char buf[32];

                if (stats != NULL && i == 0) {
                    uint32_t validMask = stats->validMask;

                    // GPU utilization
                    snprintf(lineBuf, sizeof(lineBuf), "  GPU:         %s",
                             (validMask & NVIDIA_STATS_VALID_GPU_UTIL) ?
                                 FormatUtil(stats->gpuUtil, buf, sizeof(buf)) : "N/A");
                    printf("|%-58s|\n", lineBuf);

                    // VRAM usage (used / total)
                    if ((validMask & NVIDIA_STATS_VALID_MEM_USED) &&
                        (validMask & NVIDIA_STATS_VALID_MEM_TOTAL) && stats->memTotal > 0) {
                        snprintf(lineBuf, sizeof(lineBuf), "  VRAM:        %llu MB / %llu MB",
                                 (unsigned long long)stats->memUsed / (1024*1024),
                                 (unsigned long long)stats->memTotal / (1024*1024));
                    } else if (actualVram[i] > 0) {
                        snprintf(lineBuf, sizeof(lineBuf), "  VRAM:        N/A / %llu MB",
                                 (unsigned long long)actualVram[i] / (1024*1024));
                    } else {
                        snprintf(lineBuf, sizeof(lineBuf), "  VRAM:        N/A / %llu MB (BAR1)",
                                 (unsigned long long)cards[i].fb_size / (1024*1024));
                    }
                    printf("|%-58s|\n", lineBuf);

                    // Memory bandwidth
                    snprintf(lineBuf, sizeof(lineBuf), "  Mem BW:      %s",
                             (validMask & NVIDIA_STATS_VALID_MEM_UTIL) ?
                                 FormatUtil(stats->memUtil, buf, sizeof(buf)) : "N/A");
                    printf("|%-58s|\n", lineBuf);

                    printf("|%-58s|\n", "");

                    // Clocks together
                    snprintf(lineBuf, sizeof(lineBuf), "  GPU Clock:   %s",
                             (validMask & NVIDIA_STATS_VALID_GPU_CLOCK) ?
                                 FormatClock(stats->gpuClockMHz, buf, sizeof(buf)) : "N/A");
                    printf("|%-58s|\n", lineBuf);

                    snprintf(lineBuf, sizeof(lineBuf), "  Mem Clock:   %s",
                             (validMask & NVIDIA_STATS_VALID_MEM_CLOCK) ?
                                 FormatClock(stats->memClockMHz, buf, sizeof(buf)) : "N/A");
                    printf("|%-58s|\n", lineBuf);

                    printf("|%-58s|\n", "");

                    // Temperature + Power
                    snprintf(lineBuf, sizeof(lineBuf), "  Temperature: %s",
                             (validMask & NVIDIA_STATS_VALID_TEMP) ?
                                 FormatTemp(stats->gpuTemp, buf, sizeof(buf)) : "N/A");
                    printf("|%-58s|\n", lineBuf);

                    snprintf(lineBuf, sizeof(lineBuf), "  Power:       %s",
                             (validMask & NVIDIA_STATS_VALID_POWER) ?
                                 FormatPower(stats->powerMw, buf, sizeof(buf)) : "N/A");
                    printf("|%-58s|\n", lineBuf);

                    // Show update info if stale
                    bigtime_t age = system_time() - stats->lastUpdate;
                    if (age > 5000000) {
                        snprintf(lineBuf, sizeof(lineBuf), "  (stale - %.1fs old)", age / 1000000.0);
                        printf("|%-58s|\n", lineBuf);
                    }
                } else {
                    // No stats available - show what we can
                    if (actualVram[i] > 0) {
                        snprintf(lineBuf, sizeof(lineBuf), "  VRAM:        N/A / %llu MB",
                                 (unsigned long long)actualVram[i] / (1024*1024));
                    } else {
                        snprintf(lineBuf, sizeof(lineBuf), "  VRAM:        N/A / %llu MB (BAR1)",
                                 (unsigned long long)cards[i].fb_size / (1024*1024));
                    }
                    printf("|%-58s|\n", lineBuf);
                    printf("|%-58s|\n", "  GPU:         N/A  (stats not available)");
                    printf("|%-58s|\n", "  Mem BW:      N/A  (stats not available)");
                    printf("|%-58s|\n", "  Temperature: N/A  (stats not available)");
                    printf("|%-58s|\n", "  Power:       N/A  (stats not available)");
                }

                printf("+----------------------------------------------------------+\n\n");
            }
        }

        if (gpuCount == 0) {
            printf("No NVIDIA GPUs found.\n");
        }

        if (loop) {
            printf("Press Ctrl+C to exit\n");
            fflush(stdout);
            sleep(1);
        }
    } while (loop && sRunning);

    // Cleanup
    for (int i = 0; i < 8; i++) {
        CleanupRmHandles(&rmHandles[i]);
    }
    if (clonedArea >= 0) {
        delete_area(clonedArea);
    }
    close(fd);

    return 0;
}
