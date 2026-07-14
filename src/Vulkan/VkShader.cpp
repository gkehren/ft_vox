#include "Vulkan/VkShader.hpp"

#include <fstream>
#include <stdexcept>

std::vector<uint32_t> readSpirvFile(const std::string &path)
{
	std::ifstream file(path, std::ios::ate | std::ios::binary);
	if (!file)
		throw std::runtime_error("Failed to open SPIR-V file: " + path);

	const auto fileSize = static_cast<size_t>(file.tellg());
	if (fileSize == 0 || (fileSize % 4) != 0)
		throw std::runtime_error("Invalid SPIR-V file size: " + path);

	std::vector<uint32_t> buffer(fileSize / 4);
	file.seekg(0);
	file.read(reinterpret_cast<char *>(buffer.data()), static_cast<std::streamsize>(fileSize));
	if (!file)
		throw std::runtime_error("Failed to read SPIR-V file: " + path);
	return buffer;
}

VkShaderModule createShaderModule(VkDevice device, const uint32_t *code, size_t codeSizeBytes)
{
	if (device == VK_NULL_HANDLE || !code || codeSizeBytes == 0 || (codeSizeBytes % 4) != 0)
		throw std::runtime_error("createShaderModule: invalid arguments");

	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = codeSizeBytes;
	createInfo.pCode = code;

	VkShaderModule module = VK_NULL_HANDLE;
	if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS)
		throw std::runtime_error("vkCreateShaderModule failed");
	return module;
}

VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t> &spirv)
{
	return createShaderModule(device, spirv.data(), spirv.size() * sizeof(uint32_t));
}

VkShaderModule loadShaderModule(VkDevice device, const std::string &path)
{
	const auto spirv = readSpirvFile(path);
	return createShaderModule(device, spirv);
}

void destroyShaderModule(VkDevice device, VkShaderModule module)
{
	if (device != VK_NULL_HANDLE && module != VK_NULL_HANDLE)
		vkDestroyShaderModule(device, module, nullptr);
}
