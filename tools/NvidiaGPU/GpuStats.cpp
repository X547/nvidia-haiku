/*
 * GpuStats.cpp - Live GPU metrics straight from the NVIDIA RM
 */

#include "GpuStats.h"

#include <string.h>

#include <exception>


GpuStats::GpuStats()
	: fRmDev(fRm, 0)
	, fRusdMapped(nullptr)
{
	// Allocate RUSD with polling enabled for every data type we read.
	// If this fails (e.g. the driver does not expose RUSD) the object stays
	// "unavailable" rather than throwing out of the constructor; a failure
	// to open the device at all propagates from the fRm/fRmDev members.
	NV00DE_ALLOC_PARAMETERS rusdParams = {};
	rusdParams.polledDataMask = NV00DE_RUSD_POLL_CLOCK |
	                            NV00DE_RUSD_POLL_PERF |
	                            NV00DE_RUSD_POLL_THERMAL |
	                            NV00DE_RUSD_POLL_POWER;

	try {
		fRusd = fRmDev.Subdevice().Alloc(RM_USER_SHARED_DATA, &rusdParams, sizeof(rusdParams));
		fRusdMapping = fRmDev.MapMemory(fRusd.Get(), true, 0, sizeof(NV00DE_SHARED_DATA), 0);
		fRusdMapped = (NV00DE_SHARED_DATA*)fRusdMapping.Address();
	} catch (const std::exception&) {
		fRusdMapped = nullptr;
	}
}


bool GpuStats::Read(nvidia_gpu_stats_t& out)
{
	if (!fRusdMapped)
		return false;

	memset(&out, 0, sizeof(out));
	uint32 validMask = 0;

	NvTemp temp = 0;
	if (ReadTemperature(&temp)) {
		out.gpuTemp = temp;
		validMask |= NVIDIA_STATS_VALID_TEMP;
	}

	uint32 gpuUtil = 0, memUtil = 0;
	if (ReadUtilization(&gpuUtil, &memUtil)) {
		out.gpuUtil = gpuUtil;
		out.memUtil = memUtil;
		validMask |= NVIDIA_STATS_VALID_GPU_UTIL | NVIDIA_STATS_VALID_MEM_UTIL;
	}

	uint32 gpuClock = 0, memClock = 0;
	if (ReadClocks(&gpuClock, &memClock)) {
		out.gpuClockMHz = gpuClock;
		out.memClockMHz = memClock;
		validMask |= NVIDIA_STATS_VALID_GPU_CLOCK | NVIDIA_STATS_VALID_MEM_CLOCK;
	}

	uint32 powerMw = 0;
	if (ReadPower(&powerMw)) {
		out.powerMw = powerMw;
		validMask |= NVIDIA_STATS_VALID_POWER;
	}

	uint64 vramUsed = 0, vramTotal = 0;
	if (QueryVramUsage(&vramUsed, &vramTotal)) {
		out.memUsed = vramUsed;
		out.memTotal = vramTotal;
		validMask |= NVIDIA_STATS_VALID_MEM_USED | NVIDIA_STATS_VALID_MEM_TOTAL;
	}

	out.validMask = validMask;
	return true;
}


bool GpuStats::ReadTemperature(NvTemp* outTemp)
{
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


bool GpuStats::ReadUtilization(uint32* gpuUtil, uint32* memUtil)
{
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


bool GpuStats::ReadClocks(uint32* gpuClock, uint32* memClock)
{
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


bool GpuStats::ReadPower(uint32* powerMw)
{
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


bool GpuStats::QueryVramUsage(uint64* used, uint64* total)
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
	} catch (const std::exception&) {
		return false;
	}
}
