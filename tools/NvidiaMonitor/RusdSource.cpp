/*
 * RusdSource.cpp - see RusdSource.h
 */

#include "RusdSource.h"

#include <exception>

extern "C" {
#include "nvstatus.h"
#include "class/cl2080.h"               // NV20_SUBDEVICE_0
#include "ctrl/ctrl2080/ctrl2080gpu.h"  // NV2080_CTRL_CMD_GPU_GET_PIDS / GET_PID_INFO
}


RusdSource::RusdSource()
	:
	fMapped(nullptr)
{
	// Request polling for every data class so the polled sections (clocks,
	// perf, memory, power, thermal, pci) are kept up to date by RM. The
	// non-polled sections (bar1, pma, grInfo, shadowErrCont) are always live.
	NV00DE_ALLOC_PARAMETERS params = {};
	params.polledDataMask = NV00DE_RUSD_POLL_CLOCK |
	                        NV00DE_RUSD_POLL_PERF |
	                        NV00DE_RUSD_POLL_MEMORY |
	                        NV00DE_RUSD_POLL_POWER |
	                        NV00DE_RUSD_POLL_THERMAL |
	                        NV00DE_RUSD_POLL_PCI;

	try {
		fRm.reset(new NvRmApi());
		fRmDev.reset(new NvRmDevice(*fRm, 0));
		fRusd = fRmDev->Subdevice().Alloc(RM_USER_SHARED_DATA, &params, sizeof(params));
		fMapping = fRmDev->MapMemory(fRusd.Get(), true, 0, sizeof(NV00DE_SHARED_DATA), 0);
		fMapped = (NV00DE_SHARED_DATA*)fMapping.Address();
	} catch (const std::exception& ex) {
		fError = ex.what();
		fMapped = nullptr;
	} catch (...) {
		fError = "unknown error opening GPU";
		fMapped = nullptr;
	}
}


bool
RusdSource::QueryClients(std::vector<GpuClientInfo>& out) const
{
	out.clear();
	if (fRmDev == nullptr)
		return false;

	try {
		// 1. Enumerate every process that has an object on this GPU. Using
		// idType=CLASS with id=NV20_SUBDEVICE_0 returns all attached PIDs.
		std::unique_ptr<NV2080_CTRL_GPU_GET_PIDS_PARAMS> pids(
			new NV2080_CTRL_GPU_GET_PIDS_PARAMS());
		memset(pids.get(), 0, sizeof(*pids));
		pids->idType = NV2080_CTRL_GPU_GET_PIDS_ID_TYPE_CLASS;
		pids->id = NV20_SUBDEVICE_0;
		fRmDev->Subdevice().Control(NV2080_CTRL_CMD_GPU_GET_PIDS, pids.get(), sizeof(*pids));

		NvU32 count = pids->pidTblCount;
		if (count > NV2080_CTRL_GPU_GET_PIDS_MAX_COUNT)
			count = NV2080_CTRL_GPU_GET_PIDS_MAX_COUNT;

		// RM reports a process once per matching client object, so the same PID
		// can appear several times. Collapse to a unique set: the per-PID VRAM
		// total already aggregates all of the process's allocations, so the
		// duplicates would only produce identical rows.
		std::vector<NvU32> uniquePids;
		for (NvU32 i = 0; i < count; i++) {
			NvU32 pid = pids->pidTbl[i];
			bool seen = false;
			for (size_t u = 0; u < uniquePids.size(); u++) {
				if (uniquePids[u] == pid) {
					seen = true;
					break;
				}
			}
			if (!seen)
				uniquePids.push_back(pid);
		}

		// 2. Query per-PID VRAM usage, batched to the control's max list size.
		for (size_t base = 0; base < uniquePids.size();
				base += NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT) {
			size_t batch = uniquePids.size() - base;
			if (batch > NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT)
				batch = NV2080_CTRL_GPU_GET_PID_INFO_MAX_COUNT;

			std::unique_ptr<NV2080_CTRL_GPU_GET_PID_INFO_PARAMS> info(
				new NV2080_CTRL_GPU_GET_PID_INFO_PARAMS());
			memset(info.get(), 0, sizeof(*info));
			info->pidInfoListCount = (NvU32)batch;
			for (size_t i = 0; i < batch; i++) {
				info->pidInfoList[i].pid = uniquePids[base + i];
				info->pidInfoList[i].index = NV2080_CTRL_GPU_PID_INFO_INDEX_VIDEO_MEMORY_USAGE;
			}
			fRmDev->Subdevice().Control(NV2080_CTRL_CMD_GPU_GET_PID_INFO, info.get(), sizeof(*info));

			for (size_t i = 0; i < batch; i++) {
				const NV2080_CTRL_GPU_PID_INFO& pi = info->pidInfoList[i];
				GpuClientInfo client = {};
				client.pid = uniquePids[base + i];
				if (pi.result == NV_OK) {
					const NV2080_CTRL_GPU_PID_INFO_VIDEO_MEMORY_USAGE_DATA& m =
						pi.data.vidMemUsage;
					client.vramPrivate = m.memPrivate;
					client.vramShared = m.memSharedOwned + m.memSharedDuped;
					client.vramTotal = m.memPrivate + m.memSharedOwned + m.memSharedDuped;
				}
				out.push_back(client);
			}
		}
		return true;
	} catch (const std::exception&) {
		out.clear();
		return false;
	}
}
