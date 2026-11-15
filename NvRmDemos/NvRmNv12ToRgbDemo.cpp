// NvRmNv12ToRgbDemo
//
// Converts an NV12 picture to B, G, R, A bytes with a compute shader
// (shaders/nv12_to_rgb32.comp, as shaders/Nv12ToRgb32Shader.h), launched
// through the compute class as in NvRmComputeDemo, and compares the result
// byte for byte with the CPU conversion NvDecPlugin uses.
//
//   NvRmNv12ToRgbDemo [<picture.nv12> <width> <height> [<out.png>]]
//
// The picture is the first of a raw NV12 file without padding (what
// `nvdecstream` writes with NVDEC_NV12=1, or ffmpeg with -pix_fmt nv12);
// without one, a generated 3840x2160 picture that covers every luma and
// chroma value is used. The conversion is timed with the picture and the
// result in system or video memory, in the four combinations; the copy
// engine moves pictures between the two.

#include <fcntl.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <chrono>
#include <system_error>
#include <vector>

extern "C" {
#include "nvtypes.h"
#include "nv-haiku.h"
#include "class/clc5b5.h" // TURING_DMA_COPY_A
#include "class/clc6b5.h" // AMPERE_DMA_COPY_A
#include "class/clc7b5.h" // AMPERE_DMA_COPY_B
#include "ctrl/ctrl0080/ctrl0080gpu.h" // NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2
}

#include <ErrorUtils.h>
#include <FileDesc.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "ImageIo.h"
#include "NvChannel.h"
#include "NvCommandBuffer.h"
#include "NvCompute.h"
#include "NvGpuMemory.h"
#include "shaders/Nv12ToRgb32Shader.h"


// The shader's push constants (std430).
struct Params {
	NvU64 luma;
	NvU64 chroma;
	NvU64 pixels;
	NvU32 lumaPitch;
	NvU32 chromaPitch;
	NvU32 pixelsPitch;
	NvU32 width;
	NvU32 height;
	NvU32 left;
	NvU32 top;
	NvS32 yMul;
	NvS32 rV;
	NvS32 gU;
	NvS32 gV;
	NvS32 bU;
};

struct Coefficients {
	const char *name;
	int yMul;
	int rV, gU, gV, bU;
};

// nvdec_convert.c's
static const Coefficients kBT601 = {"BT.601", 298, 409, -100, -208, 516};
static const Coefficients kBT709 = {"BT.709", 298, 459, -55, -136, 541};

struct Report {
	NvU64 value;
	NvU64 timestamp;
};

static constexpr NvU32 kCopySubch = 4;
static constexpr int kIterations = 20;


static NvU8 Clamp(int value)
{
	return (NvU8)(value < 0 ? 0 : (value > 255 ? 255 : value));
}


// nvdecFrameToRGB32() on plain lines, without cropping.
static void ConvertOnCpu(const NvU8 *luma, const NvU8 *chroma, NvU32 pitch, NvU32 width, NvU32 height,
	const Coefficients &c, NvU8 *out)
{
	for (NvU32 y = 0; y < height; y++) {
		const NvU8 *lumaLine = luma + (size_t)y * pitch;
		const NvU8 *chromaLine = chroma + (size_t)(y / 2) * pitch;
		NvU8 *line = out + (size_t)y * width * 4;
		for (NvU32 x = 0; x < width; x++) {
			int yy = (lumaLine[x] - 16) * c.yMul;
			int cb = chromaLine[x & ~1u] - 128;
			int cr = chromaLine[(x & ~1u) + 1] - 128;
			line[4 * x + 0] = Clamp((yy + c.bU * cb + 128) >> 8);
			line[4 * x + 1] = Clamp((yy + c.gU * cb + c.gV * cr + 128) >> 8);
			line[4 * x + 2] = Clamp((yy + c.rV * cr + 128) >> 8);
			line[4 * x + 3] = 255;
		}
	}
}


static NvU32 PickCopyClass(NvRmDevice &dev)
{
	static const NvU32 kCandidates[] = {AMPERE_DMA_COPY_B, AMPERE_DMA_COPY_A, TURING_DMA_COPY_A};

	NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS params {};
	dev.Device().Control(NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &params, sizeof(params));
	for (NvU32 candidate : kCandidates) {
		for (NvU32 i = 0; i < params.numClasses; i++) {
			if (params.classList[i] == candidate) {
				return candidate;
			}
		}
	}
	throw std::system_error(ENOTSUP, std::generic_category(), "no copy class");
}


class Demo {
private:
	NvChannel fChannel;
	NvCommandBuffer fCmdBuf;
	NvRmObject fCompute;
	NvRmObject fCopy;
	NvComputeProgram fProgram;
	NvGpuMemory fSlot;
	NvGpuMemory fReports;
	NvU64 fSequence {};

	NvU32 fWidth, fHeight, fPitch;
	NvU64 fPlanesSize, fPixelsSize;
	NvGpuMemory fPlanesSystem, fPlanesVideo, fPixelsSystem, fPixelsVideo;

	volatile Report *Reports() const {return (volatile Report*)fReports.CpuAddr();}

	// Submit what has been written, with a release of `fSequence` at the
	// end, and wait for it.
	void Finish()
	{
		fSequence++;
		WriteWaitForIdleFlush(fCmdBuf);
		WriteSemRelease64(fCmdBuf, fReports.GpuAddr() + sizeof(Report), fSequence, true, true);
		fCmdBuf.SubmitTo(fChannel);
		for (int i = 0; Reports()[1].value < fSequence; i++) {
			fChannel.CheckError();
			if (i > 5000000 / 20) {
				throw std::system_error(ETIMEDOUT, std::generic_category(), "the GPU did not finish");
			}
			usleep(20);
		}
	}

public:
	Demo(NvRmDevice &dev, NvU32 width, NvU32 height):
		fChannel(dev),
		fCmdBuf(fChannel),
		fCompute(fChannel.Channel().Alloc(PickComputeClass(dev), nullptr, 0)),
		fCopy(fChannel.Channel().Alloc(PickCopyClass(dev), nullptr, 0)),
		fProgram(dev, fChannel.VaSpace(), kNv12ToRgb32Shader),
		fWidth(width),
		fHeight(height),
		fPitch((width + 63) & ~63u)
	{
		fSlot.AllocSystem(dev, fChannel.VaSpace(), kComputeSlotSize, 0x1000, true);
		fReports.AllocSystem(dev, fChannel.VaSpace(), 0x1000, 0x1000, true);

		fPlanesSize = ((NvU64)fPitch * height * 3 / 2 + 0x1fff) & ~(NvU64)0xfff;
		fPixelsSize = ((NvU64)width * 4 * height + 0xfff) & ~(NvU64)0xfff;
		fPlanesSystem.AllocSystem(dev, fChannel.VaSpace(), fPlanesSize, 0x1000, true);
		fPlanesVideo.AllocLocal(dev, fChannel.VaSpace(), fPlanesSize, 0x10000);
		fPixelsSystem.AllocSystem(dev, fChannel.VaSpace(), fPixelsSize, 0x1000, true);
		fPixelsVideo.AllocLocal(dev, fChannel.VaSpace(), fPixelsSize, 0x10000);

		WriteComputeInit(fCmdBuf, PickComputeClass(dev));
		WriteSetObject(fCmdBuf, kCopySubch, PickCopyClass(dev));
		Finish();
	}

	NvU32 Pitch() const {return fPitch;}
	NvU8 *Luma() const {return (NvU8*)fPlanesSystem.CpuAddr();}
	NvU8 *Chroma() const {return Luma() + (size_t)fPitch * fHeight;}
	const NvU8 *Pixels() const {return (const NvU8*)fPixelsSystem.CpuAddr();}

	// Put the picture in system memory into video memory too.
	void Upload()
	{
		WriteCopyDma(fCmdBuf, kCopySubch, fPlanesSystem.GpuAddr(), fPlanesVideo.GpuAddr(), fPlanesSize);
		Finish();
	}

	// Convert from system or video memory into system or video memory;
	// what ends up in video memory is copied to system memory afterwards so
	// it can be checked. Returns the GPU time of the conversion, ns, with
	// `timeCopy` including that copy.
	NvU64 Convert(bool planesInVideo, bool pixelsInVideo, const Coefficients &c, bool timeCopy = false)
	{
		const NvGpuMemory &planes = planesInVideo ? fPlanesVideo : fPlanesSystem;
		const NvGpuMemory &pixels = pixelsInVideo ? fPixelsVideo : fPixelsSystem;
		memset(fPixelsSystem.CpuAddr(), 0xcc, fPixelsSize);

		Params params {
			.luma = planes.GpuAddr(),
			.chroma = planes.GpuAddr() + (NvU64)fPitch * fHeight,
			.pixels = pixels.GpuAddr(),
			.lumaPitch = fPitch,
			.chromaPitch = fPitch,
			.pixelsPitch = fWidth * 4,
			.width = fWidth,
			.height = fHeight,
			.left = 0,
			.top = 0,
			.yMul = c.yMul,
			.rV = c.rV,
			.gU = c.gU,
			.gV = c.gV,
			.bU = c.bU,
		};
		const NvComputeShader &shader = fProgram.Shader();
		NvU32 columnsPerGroup = shader.localSize[0] * 4;
		NvU32 linesPerGroup = shader.localSize[1];
		NvU64 qmd = fProgram.PrepareLaunch(fSlot.CpuAddr(), fSlot.GpuAddr(), &params, sizeof(params),
			(fWidth + columnsPerGroup - 1) / columnsPerGroup, (fHeight + linesPerGroup - 1) / linesPerGroup, 1);

		WriteSemRelease64(fCmdBuf, fReports.GpuAddr(), fSequence + 1, true, true);
		WriteComputeLaunch(fCmdBuf, qmd);
		WriteWaitForIdleFlush(fCmdBuf);
		if (!timeCopy) {
			WriteSemRelease64(fCmdBuf, fReports.GpuAddr() + 2 * sizeof(Report), fSequence + 1, true, true);
		}
		if (pixelsInVideo) {
			WriteCopyDma(fCmdBuf, kCopySubch, fPixelsVideo.GpuAddr(), fPixelsSystem.GpuAddr(), fPixelsSize);
		}
		if (timeCopy) {
			WriteSemRelease64(fCmdBuf, fReports.GpuAddr() + 2 * sizeof(Report), fSequence + 1, true, true);
		}
		Finish();
		return Reports()[2].timestamp - Reports()[0].timestamp;
	}
};


static size_t CountDifferences(const NvU8 *a, const NvU8 *b, size_t size, size_t *first)
{
	size_t count = 0;
	for (size_t i = 0; i < size; i++) {
		if (a[i] != b[i]) {
			if (count == 0) {
				*first = i;
			}
			count++;
		}
	}
	return count;
}


int main(int argc, char **argv)
{
	if (argc != 1 && argc != 4 && argc != 5) {
		fprintf(stderr, "usage: %s [<picture.nv12> <width> <height> [<out.png>]]\n", argv[0]);
		return 2;
	}
	try {
		NvU32 width = 3840;
		NvU32 height = 2160;
		std::vector<NvU8> file;
		if (argc >= 4) {
			width = (NvU32)atoi(argv[2]);
			height = (NvU32)atoi(argv[3]);
			if (width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0) {
				throw std::system_error(EINVAL, std::generic_category(), "width and height must be even");
			}
			file.resize((size_t)width * height * 3 / 2);
			FILE *in = fopen(argv[1], "rb");
			if (in == nullptr || fread(file.data(), 1, file.size(), in) != file.size()) {
				throw std::system_error(EIO, std::generic_category(), std::string("cannot read a picture from ") + argv[1]);
			}
			fclose(in);
		}

		FileDesc devFd(open("/dev/" NVIDIA_DEVICE_NAME "0", O_RDWR | O_CLOEXEC));
		CheckErrno(devFd.Get());
		NvRmApi rm;
		NvRmDevice dev(rm, 0);
		Demo demo(dev, width, height);

		NvU32 pitch = demo.Pitch();
		for (NvU32 y = 0; y < height; y++) {
			NvU8 *line = demo.Luma() + (size_t)y * pitch;
			for (NvU32 x = 0; x < width; x++) {
				line[x] = file.empty() ? (NvU8)(x + y * 3 + ((x * y) >> 7)) : file[(size_t)y * width + x];
			}
		}
		for (NvU32 y = 0; y < height / 2; y++) {
			NvU8 *line = demo.Chroma() + (size_t)y * pitch;
			for (NvU32 x = 0; x < width; x++) {
				line[x] = file.empty()
					? (NvU8)((x & 1) ? (y + (x >> 4)) : (x + (y >> 3)))
					: file[(size_t)width * height + (size_t)y * width + x];
			}
		}
		demo.Upload();

		printf("picture:        %ux%u, pitch %u, %s\n", width, height, pitch, file.empty() ? "generated" : argv[1]);
		printf("shader:         %s, %u bytes, %u registers, workgroup %ux%u\n", kNv12ToRgb32Shader.name,
			kNv12ToRgb32Shader.codeSize, kNv12ToRgb32Shader.numGprs, kNv12ToRgb32Shader.localSize[0],
			kNv12ToRgb32Shader.localSize[1]);

		size_t pixelsSize = (size_t)width * height * 4;
		std::vector<NvU8> expected(pixelsSize);
		int failures = 0;

		for (const Coefficients *c : {&kBT601, &kBT709}) {
			auto began = std::chrono::steady_clock::now();
			ConvertOnCpu(demo.Luma(), demo.Chroma(), pitch, width, height, *c, expected.data());
			double cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
			printf("\n%s, the CPU on one thread: %.1f ms\n", c->name, cpuMs);

			// The last one converts into video memory and has the copy
			// engine move the result into system memory, timing both.
			for (int combination = 0; combination < 5; combination++) {
				bool planesInVideo = (combination & 1) != 0 || combination == 4;
				bool pixelsInVideo = (combination & 2) != 0 || combination == 4;
				bool timeCopy = combination == 4;
				NvU64 best = ~(NvU64)0;
				NvU64 sum = 0;
				size_t differences = 0;
				size_t first = 0;
				for (int i = 0; i < kIterations; i++) {
					NvU64 ns = demo.Convert(planesInVideo, pixelsInVideo, *c, timeCopy);
					sum += ns;
					best = (ns < best) ? ns : best;
					if (i == 0) {
						differences = CountDifferences(demo.Pixels(), expected.data(), pixelsSize, &first);
					}
				}
				printf("  %s memory to %s memory%s: %.3f ms (best %.3f), ",
					planesInVideo ? "video " : "system", pixelsInVideo ? "video " : "system",
					timeCopy ? ", copied to system memory" : "",
					sum / 1e6 / kIterations, best / 1e6);
				if (differences == 0) {
					printf("as the CPU's\n");
				} else {
					size_t pixel = first / 4;
					printf("%zu bytes differ, the first at pixel (%zu, %zu): %02x instead of %02x\n", differences,
						pixel % width, pixel / width, demo.Pixels()[first], expected[first]);
					failures++;
				}
			}
		}

		if (argc == 5) {
			demo.Convert(true, false, kBT709);
			WriteImageA8R8G8B8(argv[4], width, height, demo.Pixels());
			printf("\nwrote %s\n", argv[4]);
		}
		return failures == 0 ? 0 : 2;
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] NvRmNv12ToRgbDemo: %s\n", ex.what());
		return 1;
	}
}
