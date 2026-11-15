#include "NvCompute.h"
#include "NvChannel.h"
#include "NvCommandBuffer.h"

#include <string.h>

#include <iterator>
#include <string>
#include <system_error>

extern "C" {
#include "nvmisc.h"
#include "class/clc36f.h" // NVC36F_SET_REFERENCE
#include "class/clc6c0.h" // AMPERE_COMPUTE_A
#include "class/clc7c0.h" // AMPERE_COMPUTE_B, NVC7C0_*
#include "ctrl/ctrl0080/ctrl0080gpu.h" // NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2
#include "ctrl/ctrl0080/ctrl0080gr.h" // NV0080_CTRL_CMD_GR_GET_INFO_V2
}


// Where the shared and local memory windows sit in every shader's address
// space; Mesa's choice, at the top of the low 4 GiB.
static constexpr NvU64 kSharedWindow = 0xfe000000;
static constexpr NvU64 kLocalWindow = 0xff000000;

// Constant buffer 0 is NVK's root descriptor table, whose push constants
// end the table.
static constexpr NvU32 kMaxPushSize = 0x100;


NvU32 PickComputeClass(NvRmDevice &dev)
{
	static const NvU32 kCandidates[] = {
		AMPERE_COMPUTE_B,
		AMPERE_COMPUTE_A,
	};

	NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS params {};
	dev.Device().Control(NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &params, sizeof(params));

	for (NvU32 candidate : kCandidates) {
		for (NvU32 i = 0; i < params.numClasses; i++) {
			if (params.classList[i] == candidate) {
				return candidate;
			}
		}
	}
	throw std::system_error(ENOTSUP, std::generic_category(), "no compute class with a version 3 QMD (Ampere)");
}


NvU32 QueryShaderModel(NvRmDevice &dev)
{
	NV0080_CTRL_GR_GET_INFO_V2_PARAMS params {};
	params.grInfoListSize = 1;
	params.grInfoList[0].index = NV0080_CTRL_GR_INFO_INDEX_SM_VERSION;
	dev.Device().Control(NV0080_CTRL_CMD_GR_GET_INFO_V2, &params, sizeof(params));

	NvU32 version = params.grInfoList[0].data;
	return (version >> 8) * 10 + (version & 0xf);
}


NvComputeProgram::NvComputeProgram(NvRmDevice &dev, NvHandle hVaSpace, const NvComputeShader &shader):
	fShader(&shader)
{
	NvU32 sm = QueryShaderModel(dev);
	if (sm != shader.sm) {
		throw std::system_error(ENOTSUP, std::generic_category(),
			std::string("shader ") + shader.name + " is for SM " + std::to_string(shader.sm)
			+ ", the GPU is SM " + std::to_string(sm));
	}
	if (shader.pushSize > kMaxPushSize) {
		throw std::system_error(EINVAL, std::generic_category(), "shader parameters too large");
	}

	// The instruction fetch runs ahead of the program, so the code is
	// followed by a page of padding rather than by the end of the mapping.
	NvU64 size = (shader.codeSize + 0x1fff) & ~(NvU64)0xfff;
	fCode.AllocSystem(dev, hVaSpace, size, 0x1000, true);
	memset(fCode.CpuAddr(), 0, size);
	memcpy(fCode.CpuAddr(), shader.code, shader.codeSize);
}


// A field of the QMD, given by its bit range as the class headers' MW(hi:lo).
static void SetQmdField(NvU32 *qmd, NvU32 hi, NvU32 lo, NvU64 value)
{
	for (NvU32 bit = lo; bit <= hi; bit++) {
		NvU32 &word = qmd[bit / 32];
		NvU32 mask = 1u << (bit % 32);
		if ((value >> (bit - lo)) & 1) {
			word |= mask;
		} else {
			word &= ~mask;
		}
	}
}


// The shared memory carve-outs of an SM, in KiB, as Mesa's nv_device_info
// lists them, and the QMD's encoding of one.
static NvU32 PickSharedMemConfig(NvU32 sm, NvU32 size, NvU32 workgroups)
{
	static const NvU32 kSizesSm80[] = {0, 8, 16, 32, 64, 100, 132, 164};
	static const NvU32 kSizesSm86[] = {0, 8, 16, 32, 64, 100};
	const NvU32 *sizes = (sm == 80) ? kSizesSm80 : kSizesSm86;
	NvU32 count = (sm == 80) ? std::size(kSizesSm80) : std::size(kSizesSm86);

	NvU32 highest = sizes[count - 1];
	NvU32 fit = highest * 1024 / (size > 0 ? size : 1);
	if (workgroups > fit) {
		workgroups = fit;
	}
	NvU32 kib = highest;
	for (NvU32 i = 0; i < count; i++) {
		if (sizes[i] * 1024 >= size * workgroups) {
			kib = sizes[i];
			break;
		}
	}
	return kib / 4 + 1;
}


static NvU32 MaxSharedMemConfig(NvU32 sm)
{
	return ((sm == 80) ? 164 : 100) / 4 + 1;
}


NvU64 NvComputeProgram::PrepareLaunch(void *slotCpu, NvU64 slotGpu, const void *params, std::size_t paramsSize,
	NvU32 gridX, NvU32 gridY, NvU32 gridZ) const
{
	const NvComputeShader &shader = *fShader;
	if (paramsSize > kMaxPushSize || (slotGpu & 0xff) != 0) {
		throw std::system_error(EINVAL, std::generic_category(), "bad compute launch slot or parameters");
	}

	// Constant buffer 0, laid out as NVK's root descriptor table: the
	// dispatch's base workgroup (zero) and workgroup count, which NVK's
	// shaders add to and read as gl_NumWorkGroups, then the parameters where
	// the shader expects its push constants.
	NvU8 *cbuf = (NvU8*)slotCpu + kComputeQmdSize;
	NvU32 cbufSize = shader.pushOffset + kMaxPushSize;
	memset(cbuf, 0, cbufSize);
	const NvU32 groupCount[3] = {gridX, gridY, gridZ};
	memcpy(cbuf + 3 * sizeof(NvU32), groupCount, sizeof(groupCount));
	memcpy(cbuf + shader.pushOffset, params, paramsSize);
	NvU64 cbufAddr = slotGpu + kComputeQmdSize;

	NvU32 qmd[kComputeQmdSize / 4] {};
	SetQmdField(qmd, 579, 576, 0);  // QMD_VERSION
	SetQmdField(qmd, 583, 580, 3);  // QMD_MAJOR_VERSION
	SetQmdField(qmd, 378, 378, 1);  // API_VISIBLE_CALL_LIMIT: NO_CHECK
	SetQmdField(qmd, 382, 382, 0);  // SAMPLER_INDEX: INDEPENDENTLY
	SetQmdField(qmd, 134, 134, 1);  // SM_GLOBAL_CACHING_ENABLE

	SetQmdField(qmd, 767, 763, shader.numControlBarriers); // BARRIER_COUNT
	SetQmdField(qmd, 415, 384, gridX);  // CTA_RASTER_WIDTH
	SetQmdField(qmd, 431, 416, gridY);  // CTA_RASTER_HEIGHT
	SetQmdField(qmd, 463, 448, gridZ);  // CTA_RASTER_DEPTH
	SetQmdField(qmd, 607, 592, shader.localSize[0]); // CTA_THREAD_DIMENSION0
	SetQmdField(qmd, 623, 608, shader.localSize[1]); // CTA_THREAD_DIMENSION1
	SetQmdField(qmd, 639, 624, shader.localSize[2]); // CTA_THREAD_DIMENSION2

	NvU64 codeAddr = CodeAddr();
	SetQmdField(qmd, 1567, 1536, (NvU32)codeAddr);         // PROGRAM_ADDRESS_LOWER
	SetQmdField(qmd, 1584, 1568, codeAddr >> 32);          // PROGRAM_ADDRESS_UPPER
	SetQmdField(qmd, 656, 648, shader.numGprs);            // REGISTER_COUNT_V
	SetQmdField(qmd, 759, 736, 0);                         // SHADER_LOCAL_MEMORY_LOW_SIZE
	SetQmdField(qmd, 1623, 1600, 0);                       // SHADER_LOCAL_MEMORY_HIGH_SIZE

	NvU32 smemSize = (shader.smemSize + 0xff) & ~0xffu;
	NvU32 threads = shader.localSize[0] * shader.localSize[1] * shader.localSize[2];
	NvU32 warps = (threads + 31) / 32;
	NvU32 workgroupsPerSm = shader.maxWarpsPerSm / (warps > 0 ? warps : 1);
	SetQmdField(qmd, 561, 544, smemSize);                                         // SHARED_MEMORY_SIZE
	SetQmdField(qmd, 567, 562, PickSharedMemConfig(shader.sm, smemSize, 1));      // MIN_SM_CONFIG_SHARED_MEM_SIZE
	SetQmdField(qmd, 574, 569, MaxSharedMemConfig(shader.sm));                    // MAX_SM_CONFIG_SHARED_MEM_SIZE
	SetQmdField(qmd, 662, 657, PickSharedMemConfig(shader.sm, smemSize, workgroupsPerSm)); // TARGET_SM_CONFIG_SHARED_MEM_SIZE

	SetQmdField(qmd, 1055, 1024, (NvU32)cbufAddr);         // CONSTANT_BUFFER_ADDR_LOWER(0)
	SetQmdField(qmd, 1072, 1056, cbufAddr >> 32);          // CONSTANT_BUFFER_ADDR_UPPER(0)
	SetQmdField(qmd, 1087, 1075, cbufSize >> 4);           // CONSTANT_BUFFER_SIZE_SHIFTED4(0)
	SetQmdField(qmd, 640, 640, 1);                         // CONSTANT_BUFFER_VALID(0)

	memcpy(slotCpu, qmd, sizeof(qmd));
	return slotGpu;
}


void WriteComputeInit(NvCommandBuffer &cmdBuf, NvU32 computeClass)
{
	WriteSetObject(cmdBuf, kComputeSubch, computeClass);

	NvPushWriter wr = cmdBuf.Push(8);
	wr.IncMethod(kComputeSubch, NVC7C0_SET_SHADER_SHARED_MEMORY_WINDOW_A, 2);
	wr.U32((NvU32)(kSharedWindow >> 32));
	wr.U32((NvU32)kSharedWindow);
	wr.IncMethod(kComputeSubch, NVC7C0_SET_SHADER_LOCAL_MEMORY_WINDOW_A, 2);
	wr.U32((NvU32)(kLocalWindow >> 32));
	wr.U32((NvU32)kLocalWindow);
	cmdBuf.EndPush(wr);
}


void WriteComputeLaunch(NvCommandBuffer &cmdBuf, NvU64 qmdAddr)
{
	NvPushWriter wr = cmdBuf.Push(8);
	// Constant buffers are rewritten in place between launches.
	wr.IncMethod(kComputeSubch, NVC7C0_INVALIDATE_SHADER_CACHES_NO_WFI, 1);
	wr.U32(
		DRF_DEF(C7C0, _INVALIDATE_SHADER_CACHES_NO_WFI, _CONSTANT, _TRUE) |
		DRF_DEF(C7C0, _INVALIDATE_SHADER_CACHES_NO_WFI, _GLOBAL_DATA, _TRUE)
	);
	wr.IncMethod(kComputeSubch, NVC7C0_INVALIDATE_SKED_CACHES, 1);
	wr.U32(0);
	wr.IncMethod(kComputeSubch, NVC7C0_SEND_PCAS_A, 1);
	wr.U32((NvU32)(qmdAddr >> 8));
	wr.IncMethod(kComputeSubch, NVC7C0_SEND_SIGNALING_PCAS2_B, 1);
	wr.U32(DRF_DEF(C7C0, _SEND_SIGNALING_PCAS2_B, _PCAS_ACTION, _INVALIDATE_COPY_SCHEDULE));
	cmdBuf.EndPush(wr);
}


void WriteWaitForIdleFlush(NvCommandBuffer &cmdBuf)
{
	NvPushWriter wr = cmdBuf.Push(2);
	wr.IncMethod(0, NVC36F_SET_REFERENCE, 1);
	wr.U32(0);
	cmdBuf.EndPush(wr);
}
