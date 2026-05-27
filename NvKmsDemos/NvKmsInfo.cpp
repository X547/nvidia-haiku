#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <iostream>
#include <string>
#include <yaml-cpp/yaml.h>

extern "C" {
#include "nvkms-ioctl.h"
#include "nvkms-api.h"
#include "nvkms-sync.h"

#include "nvos.h"
#include "class/cl003e.h" // NV01_MEMORY_SYSTEM
#include "class/cl0040.h" // NV01_MEMORY_LOCAL_USER

#include <class/cl917c.h> /* NV_DISP_BASE_NOTIFIER_1, NV_DISP_NOTIFICATION_2 */
#include <class/clc37d.h> /* NV_DISP_NOTIFIER */
}

#include "ErrorUtils.h"
#include "FileDesc.h"

#include "NvRmApi.h"
#include "NvRmDevice.h"

#include "NvKmsApi.h"
#include "NvKmsDevice.h"
#include "NvKmsSurface.h"


static std::string TrimTrailingWhitespace(std::string_view src)
{
	std::string dst;

	while (!src.empty()) {
		size_t p = 0, end = 0;
		while (end != src.size() && src[end] != '\n') {
			if (src[end] > ' ') {
				p = end;
			}
			end++;
		}
		dst.append(src.substr(0, p + 1));
		if (end != src.size()) {
			dst.append("\n");
			end++;
		}
		src.remove_prefix(end);
	}

	return dst;
}

void WriteMode(YAML::Emitter &wr, const NvKmsMode &mode, bool valid, bool preferred, const char *infoString)
{
	wr << YAML::BeginMap;
		wr << YAML::Key << "name"; wr << YAML::Value << mode.name;
		wr << YAML::Key << "valid"; wr << YAML::Value << valid;
		wr << YAML::Key << "preferred"; wr << YAML::Value << preferred;
		wr << YAML::Key << "validationMsg"; wr << YAML::Value << YAML::Literal << TrimTrailingWhitespace(infoString);
		wr << YAML::Key << "timings";
		wr << YAML::BeginMap;
			wr << YAML::Key << "pixelClockHz"; wr << YAML::Value << mode.timings.pixelClockHz;
			wr << YAML::Key << "RRx1k"; wr << YAML::Value << mode.timings.RRx1k;
			wr << YAML::Key << "hVisible"; wr << YAML::Value << mode.timings.hVisible;
			wr << YAML::Key << "hSyncStart"; wr << YAML::Value << mode.timings.hSyncStart;
			wr << YAML::Key << "hSyncEnd"; wr << YAML::Value << mode.timings.hSyncEnd;
			wr << YAML::Key << "hTotal"; wr << YAML::Value << mode.timings.hTotal;
			wr << YAML::Key << "hSkew"; wr << YAML::Value << mode.timings.hSkew;
			wr << YAML::Key << "vVisible"; wr << YAML::Value << mode.timings.vVisible;
			wr << YAML::Key << "vSyncStart"; wr << YAML::Value << mode.timings.vSyncStart;
			wr << YAML::Key << "vSyncEnd"; wr << YAML::Value << mode.timings.vSyncEnd;
			wr << YAML::Key << "vTotal"; wr << YAML::Value << mode.timings.vTotal;
			wr << YAML::Key << "sizeMM";
			wr << YAML::BeginMap;
				wr << YAML::Key << "w"; wr << YAML::Value << mode.timings.sizeMM.w;
				wr << YAML::Key << "h"; wr << YAML::Value << mode.timings.sizeMM.h;
			wr << YAML::EndMap;
			wr << YAML::Key << "interlaced"; wr << YAML::Value << (bool)mode.timings.interlaced;
			wr << YAML::Key << "doubleScan"; wr << YAML::Value << (bool)mode.timings.doubleScan;
			wr << YAML::Key << "hSyncPos"; wr << YAML::Value << (bool)mode.timings.hSyncPos;
			wr << YAML::Key << "hSyncNeg"; wr << YAML::Value << (bool)mode.timings.hSyncNeg;
			wr << YAML::Key << "vSyncPos"; wr << YAML::Value << (bool)mode.timings.vSyncPos;
			wr << YAML::Key << "vSyncNeg"; wr << YAML::Value << (bool)mode.timings.vSyncNeg;
			wr << YAML::Key << "hdmi3D"; wr << YAML::Value << (bool)mode.timings.hdmi3D;
			wr << YAML::Key << "yuv420Mode";
			switch (mode.timings.yuv420Mode) {
				case NV_YUV420_MODE_NONE: wr << YAML::Value << "NONE"; break;
				case NV_YUV420_MODE_SW: wr << YAML::Value << "SW"; break;
				case NV_YUV420_MODE_HW: wr << YAML::Value << "HW"; break;
				default: wr << YAML::Value << mode.timings.yuv420Mode; break;
			}
		wr << YAML::EndMap;
	wr << YAML::EndMap;
}

void DumpEnum(YAML::Emitter &wr, NvKmsDpyAttributeColorBpcValue val)
{
	switch (val) {
		case NV_KMS_DPY_ATTRIBUTE_CURRENT_COLOR_BPC_UNKNOWN:
			wr << YAML::Value << "UNKNOWN";
			break;
		case NV_KMS_DPY_ATTRIBUTE_CURRENT_COLOR_BPC_6:
			wr << YAML::Value << "6";
			break;
		case NV_KMS_DPY_ATTRIBUTE_CURRENT_COLOR_BPC_8:
			wr << YAML::Value << "8";
			break;
		case NV_KMS_DPY_ATTRIBUTE_CURRENT_COLOR_BPC_10:
			wr << YAML::Value << "10";
			break;
		default:
			wr << YAML::Value << val;
	}
}

void WriteStruct(YAML::Emitter &wr, const NvKmsDpyOutputColorFormatInfo &val)
{
	wr << YAML::BeginMap;
		wr << YAML::Key << "rgb444";
		wr << YAML::BeginMap;
			wr << YAML::Key << "maxBpc"; DumpEnum(wr, val.rgb444.maxBpc);
			wr << YAML::Key << "minBpc"; DumpEnum(wr, val.rgb444.minBpc);
		wr << YAML::EndMap;
		wr << YAML::Key << "yuv444";
		wr << YAML::BeginMap;
			wr << YAML::Key << "maxBpc"; DumpEnum(wr, val.yuv444.maxBpc);
			wr << YAML::Key << "minBpc"; DumpEnum(wr, val.yuv444.minBpc);
		wr << YAML::EndMap;
		wr << YAML::Key << "yuv422";
		wr << YAML::BeginMap;
			wr << YAML::Key << "maxBpc"; DumpEnum(wr, val.yuv422.maxBpc);
			wr << YAML::Key << "minBpc"; DumpEnum(wr, val.yuv422.minBpc);
		wr << YAML::EndMap;
	wr << YAML::EndMap;
}

void WriteStruct(YAML::Emitter &wr, const NvKmsSuperframeInfo &val)
{
	wr << YAML::BeginMap;
		wr << YAML::Key << "numViews"; wr << YAML::Value << (unsigned)val.numViews;
		wr << YAML::Key << "view";
		wr << YAML::BeginSeq;
			for (NvU32 i = 0; i < val.numViews; i++) {
				wr << YAML::BeginMap;
				wr << YAML::Key << "x"; wr << YAML::Value << val.view[i].x;
				wr << YAML::Key << "y"; wr << YAML::Value << val.view[i].y;
				wr << YAML::Key << "width"; wr << YAML::Value << val.view[i].width;
				wr << YAML::Key << "height"; wr << YAML::Value << val.view[i].height;
				wr << YAML::EndMap;
			}
		wr << YAML::EndSeq;
	wr << YAML::EndMap;
}

void WriteDpyDynamicData(YAML::Emitter &wr, const NvKmsQueryDpyDynamicDataReply &val)
{
	wr << YAML::BeginMap;
		wr << YAML::Key << "name"; wr << YAML::Value << val.name;
		wr << YAML::Key << "maxPixelClockKHz"; wr << YAML::Value << val.maxPixelClockKHz;
		wr << YAML::Key << "connected"; wr << YAML::Value << (bool)val.connected;
		wr << YAML::Key << "isVirtualRealityHeadMountedDisplay"; wr << YAML::Value << (bool)val.isVirtualRealityHeadMountedDisplay;
		wr << YAML::Key << "physicalDimensions";
		wr << YAML::BeginMap;
			wr << YAML::Key << "heightInCM"; wr << YAML::Value << (unsigned)val.physicalDimensions.heightInCM;
			wr << YAML::Key << "widthInCM"; wr << YAML::Value << (unsigned)val.physicalDimensions.widthInCM;
		wr << YAML::EndMap;
		wr << YAML::Key << "vrrType";
		switch (val.vrrType) {
			case NVKMS_DPY_VRR_TYPE_NONE: wr << YAML::Value << "NONE"; break;
			case NVKMS_DPY_VRR_TYPE_GSYNC: wr << YAML::Value << "GSYNC"; break;
			case NVKMS_DPY_VRR_TYPE_ADAPTIVE_SYNC_DEFAULTLISTED: wr << YAML::Value << "ADAPTIVE_SYNC_DEFAULTLISTED"; break;
			case NVKMS_DPY_VRR_TYPE_ADAPTIVE_SYNC_NON_DEFAULTLISTED: wr << YAML::Value << "ADAPTIVE_SYNC_NON_DEFAULTLISTED"; break;
			default: wr << YAML::Value << val.vrrType; break;
		}
		wr << YAML::Key << "supportsHDR"; wr << YAML::Value << (bool)val.supportsHDR;
		wr << YAML::Key << "stereo3DVision";
		wr << YAML::BeginMap;
			wr << YAML::Key << "supported"; wr << YAML::Value << (bool)val.stereo3DVision.supported;
			wr << YAML::Key << "isDLP"; wr << YAML::Value << (bool)val.stereo3DVision.isDLP;
			wr << YAML::Key << "isAegis"; wr << YAML::Value << (bool)val.stereo3DVision.isAegis;
			wr << YAML::Key << "subType"; wr << YAML::Value << val.stereo3DVision.subType;
		wr << YAML::EndMap;
		wr << YAML::Key << "dp";
		wr << YAML::BeginMap;
			if (val.dp.guid.valid) {
				wr << YAML::Key << "guid";
				wr << YAML::BeginMap;
						wr << YAML::Key << "buffer";
						wr << YAML::Value << YAML::Binary(val.dp.guid.buffer, NVKMS_GUID_SIZE);
						wr << YAML::Key << "str"; wr << YAML::Value << val.dp.guid.str;
				wr << YAML::EndMap;
			}
		wr << YAML::EndMap;
		if (val.edid.valid) {
			wr << YAML::Key << "edid";
			wr << YAML::BeginMap;
				wr << YAML::Key << "buffer";
				wr << YAML::Value << YAML::Binary(val.edid.buffer, val.edid.bufferSize);
				wr << YAML::Key << "infoString"; wr << YAML::Value << YAML::Literal << val.edid.infoString;
			wr << YAML::EndMap;
		}
		wr << YAML::Key << "supportedOutputColorFormats"; WriteStruct(wr, val.supportedOutputColorFormats);
		wr << YAML::Key << "superframeInfo"; WriteStruct(wr, val.superframeInfo);
	wr << YAML::EndMap;
}

void EnumerateModes(YAML::Emitter &wr, NvKmsApi &nvKms, NvKmsDeviceHandle deviceHandle, NvKmsDispHandle dispHandle, NVDpyId dpyId)
{
	wr << YAML::BeginSeq;
	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = deviceHandle;
		params.request.dispHandle = dispHandle;
		params.request.dpyId = dpyId;
		params.request.modeIndex = i;
		nvKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params));
		if (params.reply.end) {
			break;
		}

		char infoString[NVKMS_MODE_VALIDATION_MAX_INFO_STRING_LENGTH] {};
		NvKmsValidateModeParams validateModeParams {};
		validateModeParams.request.deviceHandle = deviceHandle;
		validateModeParams.request.dispHandle = dispHandle;
		validateModeParams.request.dpyId = dpyId;
		validateModeParams.request.mode = params.reply.mode;
		validateModeParams.request.infoStringSize = NVKMS_MODE_VALIDATION_MAX_INFO_STRING_LENGTH;
		validateModeParams.request.pInfoString = nvKmsPointerToNvU64(infoString);
		nvKms.Control(NVKMS_IOCTL_VALIDATE_MODE, &validateModeParams, sizeof(validateModeParams));

		WriteMode(wr, params.reply.mode, validateModeParams.reply.valid, params.reply.preferredMode, infoString);
	}
	wr << YAML::EndSeq;
}


// #pragma mark -

int main()
{
	FileDesc devFd(open("/dev/graphics/nvidia0", O_RDWR | O_CLOEXEC));
	CheckErrno(devFd.Get());

	NvU32 deviceId = 0;

	NvRmApi rm;
	NvRmDevice rmDev(rm, deviceId);

	NvKmsApi nvKms;

	NvKmsDeviceHandle deviceHandle {};
	NvKmsDispHandle dispHandle = 1; // !!!

	NvKmsDevice nvKmsDevice(nvKms, deviceId);

	deviceHandle = nvKmsDevice.Info().deviceHandle;
	if (nvKmsDevice.Info().numDisps > 0) {
		dispHandle = nvKmsDevice.Info().dispHandles[0];
	}

	YAML::Emitter wr(std::cout);

	NVDpyIdList validDpys;
	{
		NvKmsQueryDispParams params {};
		params.request.deviceHandle = deviceHandle;
		params.request.dispHandle = dispHandle;
		nvKms.Control(NVKMS_IOCTL_QUERY_DISP, &params, sizeof(params));
		validDpys = params.reply.validDpys;
	}

	wr << YAML::BeginMap;
		wr << YAML::Key << "dpys";
		wr << YAML::BeginMap;

		for (
			NVDpyId curDpyId = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(), validDpys);
			!nvDpyIdIsInvalid(curDpyId);
			curDpyId = nvNextDpyIdInDpyIdListUnsorted(curDpyId, validDpys)
		) {
			NvKmsQueryDpyDynamicDataParams params {};
			params.request.deviceHandle = deviceHandle;
			params.request.dispHandle = dispHandle;
			params.request.dpyId = curDpyId;
			params.request.forceConnected = true;
			nvKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params));

			{
				char buf[32];
				sprintf(buf, "%u", nvDpyIdToIndex(curDpyId));
				wr << YAML::Key << buf;
			}
			wr << YAML::BeginMap;
				wr << YAML::Key << "dynamicData"; WriteDpyDynamicData(wr, params.reply);
				if (params.reply.connected) {
					wr << YAML::Key << "modes";
					EnumerateModes(wr, nvKms, deviceHandle, dispHandle, curDpyId);
				}
			wr << YAML::EndMap;
		}

		wr << YAML::EndMap;
	wr << YAML::EndMap;

	std::cout << std::endl;

	return 0;
}
