// NvRmComputeDemo
//
// Runs a compute shader on the graphics engine's compute class without
// Vulkan: the shader's machine code (shaders/FillShader.h, taken out of NVK
// by NvShaderDump) is copied into GPU memory, a QMD describing the launch is
// written next to a constant buffer holding the shader's parameters, and the
// channel hands the QMD to the compute class. The shader fills a buffer in
// system memory with a pattern the CPU then checks.
//
// The two launches use the same QMD slot with different parameters, so the
// second shows that rewritten constant buffers are picked up.

#include <fcntl.h>
#include <unistd.h>

#include <stdio.h>
#include <inttypes.h>

#include <system_error>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
}

#include <ErrorUtils.h>
#include <FileDesc.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvChannel.h"
#include "NvCommandBuffer.h"
#include "NvCompute.h"
#include "NvGpuMemory.h"
#include "shaders/FillShader.h"


struct Params {
	NvU64 data;
	NvU32 count;
	NvU32 seed;
};

// A 64-bit semaphore release with a timestamp is a value and a PTIMER
// value, sixteen bytes.
struct Report {
	NvU64 value;
	NvU64 timestamp;
};

static constexpr NvU32 kWordCount = 1u << 22;


static void WaitFor(const NvChannel &channel, const volatile Report *report, NvU64 value)
{
	for (int i = 0; report->value < value; i++) {
		channel.CheckError();
		if (i > 5000000 / 20) {
			throw std::system_error(ETIMEDOUT, std::generic_category(), "the launch did not finish");
		}
		usleep(20);
	}
}


int main()
{
	try {
		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());

		NvRmApi rm;
		NvRmDevice dev(rm, 0);
		NvChannel channel(dev);
		NvCommandBuffer cmdBuf(channel);

		NvU32 computeClass = PickComputeClass(dev);
		NvRmObject compute = channel.Channel().Alloc(computeClass, nullptr, 0);
		NvComputeProgram program(dev, channel.VaSpace(), kFillShader);

		NvGpuMemory slot;
		slot.AllocSystem(dev, channel.VaSpace(), kComputeSlotSize, 0x1000, true);
		NvGpuMemory reports;
		reports.AllocSystem(dev, channel.VaSpace(), 0x1000, 0x1000, true);
		auto report = (volatile Report*)reports.CpuAddr();
		NvGpuMemory data;
		data.AllocSystem(dev, channel.VaSpace(), (NvU64)kWordCount * 4, 0x1000, true);
		auto words = (volatile NvU32*)data.CpuAddr();

		printf("compute class:  %#" PRIx32 "\n", computeClass);
		printf("shader model:   %" PRIu32 "\n", QueryShaderModel(dev));
		printf("shader:         %s, %" PRIu32 " bytes at %#" PRIx64 ", %" PRIu32 " registers\n",
			kFillShader.name, kFillShader.codeSize, (uint64_t)program.CodeAddr(), kFillShader.numGprs);

		WriteComputeInit(cmdBuf, computeClass);

		NvU32 localSize = kFillShader.localSize[0];
		int failures = 0;
		for (NvU64 seq = 1; seq <= 2; seq++) {
			Params params {
				.data = data.GpuAddr(),
				.count = kWordCount,
				.seed = (NvU32)(seq * 0x9e3779b9u),
			};
			for (NvU32 i = 0; i < kWordCount; i++) {
				words[i] = 0xcccccccc;
			}
			NvU64 qmd = program.PrepareLaunch(slot.CpuAddr(), slot.GpuAddr(), &params, sizeof(params),
				(kWordCount + localSize - 1) / localSize, 1, 1);

			WriteSemRelease64(cmdBuf, reports.GpuAddr(), seq, true, true);
			WriteComputeLaunch(cmdBuf, qmd);
			WriteWaitForIdleFlush(cmdBuf);
			WriteSemRelease64(cmdBuf, reports.GpuAddr() + sizeof(Report), seq, true, true);
			cmdBuf.SubmitTo(channel);
			WaitFor(channel, &report[1], seq);

			NvU32 wrong = 0;
			NvU32 firstWrong = 0;
			for (NvU32 i = 0; i < kWordCount; i++) {
				if (words[i] != i * 2654435761u + params.seed) {
					if (wrong == 0) {
						firstWrong = i;
					}
					wrong++;
				}
			}
			NvU64 ns = report[1].timestamp - report[0].timestamp;
			printf("launch %" PRIu64 ": %u words in %.1f us (%.2f GB/s), ", (uint64_t)seq, kWordCount, ns / 1000.0,
				kWordCount * 4.0 / ns);
			if (wrong == 0) {
				printf("all as expected\n");
			} else {
				printf("%u wrong, the first at %u: %#x instead of %#x\n", wrong, firstWrong,
					words[firstWrong], firstWrong * 2654435761u + params.seed);
				failures++;
			}
		}
		return failures == 0 ? 0 : 2;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmComputeDemo: %s\n", ex.what());
		return 1;
	}
}
