#pragma once

#include <volk.h>

#include <cstdint>
#include <string>
#include <vector>

/// Resolve a compiled shader path (tries CWD, build-vk/, and SDL base path).
std::string resolveSpvPath(const char *fileName);

/// Load SPIR-V words from a file (must be multiple of 4 bytes).
std::vector<uint32_t> readSpirvFile(const std::string &path);

/// Create a shader module from SPIR-V words.
VkShaderModule createShaderModule(VkDevice device, const uint32_t *code, size_t codeSizeBytes);
VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t> &spirv);

/// Load a `.spv` file and create a module.
VkShaderModule loadShaderModule(VkDevice device, const std::string &path);

void destroyShaderModule(VkDevice device, VkShaderModule module);
