#pragma once

#include <cstddef>

#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvGpuMemory.h"


class NvCommandBuffer;


// A compute shader compiled for one shader model, as NvShaderDump takes it
// out of NVK: the machine code plus what a QMD needs to launch it. The
// shader reads its parameters (Vulkan push constants) from constant buffer 0
// at `pushOffset`, which is where NVK keeps them in its root descriptor
// table; the launch helpers below fill that buffer the same way.
struct NvComputeShader {
	const char *name;
	NvU32 sm;                 // shader model, major * 10 + minor (86 for GA10x)
	NvU32 numGprs;
	NvU32 numControlBarriers;
	NvU32 smemSize;           // shared memory, bytes
	NvU32 maxWarpsPerSm;
	NvU16 localSize[3];
	NvU32 pushOffset;         // offset of the parameters in constant buffer 0
	NvU32 pushSize;           // largest parameter block the shader reads
	const NvU32 *code;
	NvU32 codeSize;           // bytes
};


// Compute subchannel, following Mesa's numbering (3D 0, compute 1, copy 4).
static constexpr NvU32 kComputeSubch = 1;

// One launch needs a QMD and a constant buffer 0 that live until the launch
// has run; both go into a slot of this many bytes (256-byte aligned).
static constexpr NvU32 kComputeQmdSize = 0x100;
static constexpr NvU32 kComputeSlotSize = 0x1000;


// The compute class of the GPU, if it is one whose QMD layout these helpers
// write (QMD version 3: AMPERE_COMPUTE_A/B). Throws otherwise.
NvU32 PickComputeClass(NvRmDevice &dev);

// The GPU's shader model, major * 10 + minor (86 for an RTX A2000).
NvU32 QueryShaderModel(NvRmDevice &dev);


// A shader's code in GPU memory, ready to be launched by channels in
// `hVaSpace`. Checks the shader was compiled for this GPU's shader model.
class NvComputeProgram {
private:
	const NvComputeShader *fShader {};
	NvGpuMemory fCode;

public:
	NvComputeProgram() = default;
	NvComputeProgram(NvRmDevice &dev, NvHandle hVaSpace, const NvComputeShader &shader);

	const NvComputeShader &Shader() const {return *fShader;}
	NvU64 CodeAddr() const {return fCode.GpuAddr();}

	// Write the QMD and constant buffer 0 of one launch of `gridX` by
	// `gridY` by `gridZ` workgroups into a slot (`slotCpu` and `slotGpu`,
	// kComputeSlotSize bytes, 256-byte aligned), with `params` as the
	// shader's push constants. Returns the QMD's GPU address for
	// WriteComputeLaunch.
	NvU64 PrepareLaunch(void *slotCpu, NvU64 slotGpu, const void *params, std::size_t paramsSize,
		NvU32 gridX, NvU32 gridY, NvU32 gridZ) const;
};


// Bind the compute class to kComputeSubch and set the shared and local
// memory windows. Once per channel, before the first launch.
void WriteComputeInit(NvCommandBuffer &cmdBuf, NvU32 computeClass);

// Launch the QMD at `qmdAddr` (from NvComputeProgram::PrepareLaunch). The
// launch runs asynchronously; a host semaphore release with wait-for-idle
// after it is reached once it has finished.
void WriteComputeLaunch(NvCommandBuffer &cmdBuf, NvU64 qmdAddr);

// Wait for the channel's engines to go idle and flush their writes to
// system memory, so that what the shaders wrote is visible to the CPU once a
// following semaphore release lands.
void WriteWaitForIdleFlush(NvCommandBuffer &cmdBuf);
