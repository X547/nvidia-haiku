// NvShaderDump
//
// Compiles a SPIR-V compute shader with NVK and writes the machine code it
// got, with what launching it needs, as a C++ header for NvCompute.h:
//
//   NvShaderDump <shader.spv> <Name> <out.h>
//
// declares kNameShader. The code comes out of a pipeline binary
// (VK_KHR_pipeline_binary), whose data is NVK's serialised shader. The
// shader may take parameters only as push constants and buffer device
// addresses: NVK's root descriptor table is the one constant buffer the
// launch helpers provide, and they fill only its push constants. Anything
// else the shader would need is refused here rather than at launch.
//
// The serialised layout is NVK's (nvk_shader_serialize) and NAK's
// (struct nak_shader_info) of the Mesa this runs on; the offsets below are
// checked against what can be checked and must follow Mesa if it changes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>


// struct nak_shader_info
static constexpr size_t kInfoSize = 864;
static constexpr size_t kInfoStage = 0;
static constexpr size_t kInfoSm = 4;
static constexpr size_t kInfoNumGprs = 5;
static constexpr size_t kInfoNumControlBarriers = 6;
static constexpr size_t kInfoMaxWarpsPerSm = 8;
static constexpr size_t kInfoNumInstrs = 12;
static constexpr size_t kInfoSlmSize = 40;
static constexpr size_t kInfoCrsSize = 44;
static constexpr size_t kInfoLocalSize = 48;
static constexpr size_t kInfoSmemSize = 54;
static constexpr uint32_t kMesaShaderCompute = 5;

// struct nvk_cbuf_map, then the rest of nvk_shader_serialize
static constexpr size_t kCbufMapSize = 4 + 16 * 8;
static constexpr uint8_t kNvkCbufTypeRootDesc = 1;

// offsetof(struct nvk_root_descriptor_table, push) and NVK_MAX_PUSH_SIZE
static constexpr uint32_t kPushOffset = 0x700;
static constexpr uint32_t kMaxPushSize = 0x100;
// sizeof(nvk_root_descriptor_table.cs): base_group[3] and group_count[3],
// which NvComputeProgram::PrepareLaunch fills as well
static constexpr uint32_t kDispatchSize = 0x18;


static void Check(VkResult result, const char *what)
{
	if (result != VK_SUCCESS) {
		throw std::runtime_error(std::string(what) + " failed: " + std::to_string((int)result));
	}
}


template <typename T>
static T Read(const std::vector<uint8_t> &blob, size_t offset)
{
	if (offset + sizeof(T) > blob.size()) {
		throw std::runtime_error("serialised shader too short");
	}
	T value;
	memcpy(&value, blob.data() + offset, sizeof(T));
	return value;
}


static std::vector<uint8_t> ReadFile(const char *path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file) {
		throw std::runtime_error(std::string("cannot read ") + path);
	}
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}


struct Vulkan {
	VkInstance instance {};
	VkDevice device {};

	~Vulkan()
	{
		if (device != VK_NULL_HANDLE) {
			vkDestroyDevice(device, nullptr);
		}
		if (instance != VK_NULL_HANDLE) {
			vkDestroyInstance(instance, nullptr);
		}
	}
};


static VkPhysicalDevice FindNvk(VkInstance instance)
{
	uint32_t count = 0;
	Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
	std::vector<VkPhysicalDevice> devices(count);
	Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");

	for (VkPhysicalDevice device : devices) {
		VkPhysicalDeviceDriverProperties driver {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
		VkPhysicalDeviceProperties2 properties {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
		vkGetPhysicalDeviceProperties2(device, &properties);
		if (driver.driverID == VK_DRIVER_ID_MESA_NVK) {
			return device;
		}
	}
	throw std::runtime_error("no NVK device");
}


// The serialised NVK shader of the one compute pipeline made from `spirv`.
static std::vector<uint8_t> CompileWithNvk(Vulkan &vk, const std::vector<uint8_t> &spirv)
{
	VkApplicationInfo app {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "NvShaderDump",
		.apiVersion = VK_API_VERSION_1_3,
	};
	VkInstanceCreateInfo instanceInfo {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	Check(vkCreateInstance(&instanceInfo, nullptr, &vk.instance), "vkCreateInstance");
	VkPhysicalDevice physical = FindNvk(vk.instance);

	VkPhysicalDevicePipelineBinaryFeaturesKHR binaryFeatures {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR,
		.pipelineBinaries = VK_TRUE,
	};
	VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executableFeatures {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,
		.pNext = &binaryFeatures,
		.pipelineExecutableInfo = VK_TRUE,
	};
	VkPhysicalDeviceVulkan13Features features13 {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
		.pNext = &executableFeatures,
		.maintenance4 = VK_TRUE,
	};
	VkPhysicalDeviceVulkan12Features features12 {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.pNext = &features13,
		.storageBuffer8BitAccess = VK_TRUE,
		.bufferDeviceAddress = VK_TRUE,
	};
	VkPhysicalDeviceFeatures2 features {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
		.pNext = &features12,
	};
	features.features.shaderInt64 = VK_TRUE;

	const char *extensions[] = {
		VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
		VK_KHR_PIPELINE_BINARY_EXTENSION_NAME,
		VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME,
	};
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0,
		.queueCount = 1,
		.pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo deviceInfo {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &features,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queueInfo,
		.enabledExtensionCount = (uint32_t)std::size(extensions),
		.ppEnabledExtensionNames = extensions,
	};
	Check(vkCreateDevice(physical, &deviceInfo, nullptr, &vk.device), "vkCreateDevice");
	VkDevice device = vk.device;

	auto createBinaries = (PFN_vkCreatePipelineBinariesKHR)vkGetDeviceProcAddr(device, "vkCreatePipelineBinariesKHR");
	auto getBinaryData = (PFN_vkGetPipelineBinaryDataKHR)vkGetDeviceProcAddr(device, "vkGetPipelineBinaryDataKHR");
	auto destroyBinary = (PFN_vkDestroyPipelineBinaryKHR)vkGetDeviceProcAddr(device, "vkDestroyPipelineBinaryKHR");
	if (createBinaries == nullptr || getBinaryData == nullptr || destroyBinary == nullptr) {
		throw std::runtime_error("no VK_KHR_pipeline_binary entry points");
	}

	VkShaderModuleCreateInfo moduleInfo {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = spirv.size(),
		.pCode = (const uint32_t*)spirv.data(),
	};
	VkShaderModule module;
	Check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");

	VkPushConstantRange pushRange {
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset = 0,
		.size = kMaxPushSize,
	};
	VkPipelineLayoutCreateInfo layoutInfo {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pushRange,
	};
	VkPipelineLayout layout;
	Check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout), "vkCreatePipelineLayout");

	VkPipelineCreateFlags2CreateInfoKHR flags {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
		.flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR
			| VK_PIPELINE_CREATE_2_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR,
	};
	VkComputePipelineCreateInfo pipelineInfo {
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.pNext = &flags,
		.stage = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT,
			.module = module,
			.pName = "main",
		},
		.layout = layout,
	};
	VkPipeline pipeline;
	Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline),
		"vkCreateComputePipelines");

	VkPipelineBinaryCreateInfoKHR binaryInfo {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,
		.pipeline = pipeline,
	};
	VkPipelineBinaryHandlesInfoKHR handles {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
	Check(createBinaries(device, &binaryInfo, nullptr, &handles), "vkCreatePipelineBinariesKHR");
	if (handles.pipelineBinaryCount != 1) {
		throw std::runtime_error("expected one pipeline binary, got " + std::to_string(handles.pipelineBinaryCount));
	}
	VkPipelineBinaryKHR binary;
	handles.pPipelineBinaries = &binary;
	Check(createBinaries(device, &binaryInfo, nullptr, &handles), "vkCreatePipelineBinariesKHR");

	VkPipelineBinaryDataInfoKHR dataInfo {
		.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR,
		.pipelineBinary = binary,
	};
	VkPipelineBinaryKeyKHR key {.sType = VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
	size_t size = 0;
	Check(getBinaryData(device, &dataInfo, &key, &size, nullptr), "vkGetPipelineBinaryDataKHR");
	std::vector<uint8_t> data(size);
	Check(getBinaryData(device, &dataInfo, &key, &size, data.data()), "vkGetPipelineBinaryDataKHR");

	destroyBinary(device, binary, nullptr);
	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, layout, nullptr);
	vkDestroyShaderModule(device, module, nullptr);

	// vk_shader_serialize() puts its own header in front; the pipeline
	// cache object does not.
	static const char kHeader[] = "MesaVkShaderBin";
	if (data.size() >= 80 && memcmp(data.data(), kHeader, sizeof(kHeader)) == 0) {
		data.erase(data.begin(), data.begin() + 80);
	}
	return data;
}


static void WriteHeader(const std::vector<uint8_t> &blob, const char *spvPath, const std::string &name,
	const char *outPath)
{
	uint32_t stage = Read<uint32_t>(blob, kInfoStage);
	if (stage != kMesaShaderCompute) {
		throw std::runtime_error("not a compute shader (stage " + std::to_string(stage) + ")");
	}
	uint32_t sm = Read<uint8_t>(blob, kInfoSm);
	uint32_t numGprs = Read<uint8_t>(blob, kInfoNumGprs);
	uint32_t numControlBarriers = Read<uint8_t>(blob, kInfoNumControlBarriers);
	uint32_t maxWarpsPerSm = Read<uint32_t>(blob, kInfoMaxWarpsPerSm);
	uint32_t numInstrs = Read<uint32_t>(blob, kInfoNumInstrs);
	uint32_t slmSize = Read<uint32_t>(blob, kInfoSlmSize);
	uint32_t crsSize = Read<uint32_t>(blob, kInfoCrsSize);
	uint16_t localSize[3] = {
		Read<uint16_t>(blob, kInfoLocalSize),
		Read<uint16_t>(blob, kInfoLocalSize + 2),
		Read<uint16_t>(blob, kInfoLocalSize + 4),
	};
	uint32_t smemSize = Read<uint16_t>(blob, kInfoSmemSize);
	if (sm < 70 || sm > 120 || numGprs == 0) {
		throw std::runtime_error("the serialised shader does not look like NAK's nak_shader_info");
	}
	if (slmSize != 0 || crsSize != 0) {
		throw std::runtime_error("the shader needs local memory, which the launch helpers do not set up");
	}

	size_t at = kInfoSize;
	uint32_t cbufCount = Read<uint32_t>(blob, at);
	uint8_t cbufType = Read<uint8_t>(blob, at + 4);
	if (cbufCount != 1 || cbufType != kNvkCbufTypeRootDesc) {
		throw std::runtime_error("the shader uses descriptors or constant data; only push constants are supported");
	}
	at += kCbufMapSize;
	at += 1 + 4 + 1;        // sample_shading_enable, min_sample_shading, version override
	at = (at + 3) & ~(size_t)3;
	uint32_t codeSize = Read<uint32_t>(blob, at);
	uint32_t dataSize = Read<uint32_t>(blob, at + 4);
	at += 8;
	if (dataSize != 0) {
		throw std::runtime_error("the shader has constant data");
	}
	if (codeSize == 0 || codeSize % 16 != 0 || at + codeSize > blob.size()) {
		throw std::runtime_error("the serialised shader's code size is implausible: " + std::to_string(codeSize));
	}
	std::vector<uint32_t> code(codeSize / 4);
	memcpy(code.data(), blob.data() + at, codeSize);
	at += codeSize;
	std::string assembly((const char*)blob.data() + at, strnlen((const char*)blob.data() + at, blob.size() - at));
	if (assembly.empty()) {
		throw std::runtime_error("NVK did not keep the assembly");
	}

	// Every constant the shader reads must be one of its push constants or
	// the dispatch's base and size.
	uint32_t pushEnd = 0;
	std::regex cbufRef(R"(c\[0x([0-9a-f]+)\]\[([^\]]*)\])");
	for (auto it = std::sregex_iterator(assembly.begin(), assembly.end(), cbufRef); it != std::sregex_iterator(); ++it) {
		uint32_t index = (uint32_t)strtoul((*it)[1].str().c_str(), nullptr, 16);
		std::string operand = (*it)[2].str();
		size_t hex = operand.rfind("0x");
		uint32_t offset = (hex == std::string::npos) ? 0 : (uint32_t)strtoul(operand.c_str() + hex + 2, nullptr, 16);
		if (index == 0 && offset < kDispatchSize && operand.find('r') == std::string::npos) {
			continue;
		}
		if (index != 0 || offset < kPushOffset || offset >= kPushOffset + kMaxPushSize
			|| operand.find('r') != std::string::npos) {
			throw std::runtime_error("the shader reads " + (*it)[0].str() + ", which is not a push constant");
		}
		if (offset + 8 - kPushOffset > pushEnd) {
			pushEnd = offset + 8 - kPushOffset;
		}
	}
	if (pushEnd > kMaxPushSize) {
		pushEnd = kMaxPushSize;
	}

	FILE *out = fopen(outPath, "w");
	if (out == nullptr) {
		throw std::runtime_error(std::string("cannot write ") + outPath);
	}
	const char *base = strrchr(spvPath, '/');
	base = (base != nullptr) ? base + 1 : spvPath;
	fprintf(out, "// Generated by NvShaderDump from %s with NVK. Do not edit.\n", base);
	fprintf(out, "//\n// SM %u, %u instructions, %u registers.\n\n", sm, numInstrs, numGprs);
	// The headers sit in shaders/ below NvCompute.h, in the source tree and
	// where they are installed.
	fprintf(out, "#pragma once\n\n#include \"../NvCompute.h\"\n\n");
	fprintf(out, "/* NAK assembly:\n\n");
	for (size_t i = 0; i < assembly.size(); i++) {
		// Keep the comment closed only where it should be.
		if (assembly[i] == '*' && i + 1 < assembly.size() && assembly[i + 1] == '/') {
			fputs("* /", out);
			i++;
			continue;
		}
		fputc(assembly[i], out);
	}
	fprintf(out, "*/\n\n");

	fprintf(out, "static const NvU32 k%sCode[] = {\n", name.c_str());
	for (size_t i = 0; i < code.size(); i++) {
		fprintf(out, "%s0x%08x,%s", (i % 4 == 0) ? "\t" : "", code[i], (i % 4 == 3) ? "\n" : " ");
	}
	if (code.size() % 4 != 0) {
		fprintf(out, "\n");
	}
	fprintf(out, "};\n\n");

	fprintf(out, "static const NvComputeShader k%sShader = {\n", name.c_str());
	fprintf(out, "\t.name = \"%s\",\n", name.c_str());
	fprintf(out, "\t.sm = %u,\n", sm);
	fprintf(out, "\t.numGprs = %u,\n", numGprs);
	fprintf(out, "\t.numControlBarriers = %u,\n", numControlBarriers);
	fprintf(out, "\t.smemSize = %u,\n", smemSize);
	fprintf(out, "\t.maxWarpsPerSm = %u,\n", maxWarpsPerSm);
	fprintf(out, "\t.localSize = {%u, %u, %u},\n", localSize[0], localSize[1], localSize[2]);
	fprintf(out, "\t.pushOffset = 0x%x,\n", kPushOffset);
	fprintf(out, "\t.pushSize = 0x%x,\n", pushEnd);
	fprintf(out, "\t.code = k%sCode,\n", name.c_str());
	fprintf(out, "\t.codeSize = sizeof(k%sCode),\n", name.c_str());
	fprintf(out, "};\n");
	fclose(out);

	printf("%s: SM %u, %u bytes of code, %u registers, workgroup %ux%ux%u, %u bytes of parameters\n",
		name.c_str(), sm, codeSize, numGprs, localSize[0], localSize[1], localSize[2], pushEnd);
}


int main(int argc, char **argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s <shader.spv> <Name> <out.h>\n", argv[0]);
		return 2;
	}
	try {
		std::vector<uint8_t> spirv = ReadFile(argv[1]);
		Vulkan vk;
		std::vector<uint8_t> blob = CompileWithNvk(vk, spirv);
		WriteHeader(blob, argv[1], argv[2], argv[3]);
		return 0;
	} catch (const std::exception &ex) {
		fprintf(stderr, "[!] NvShaderDump: %s\n", ex.what());
		return 1;
	}
}
