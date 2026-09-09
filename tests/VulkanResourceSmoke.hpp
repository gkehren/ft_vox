#pragma once

#include "Vulkan/VkContext.hpp"

#include <string>

/// Run create/upload/destroy smokes for buffers, images, and a SPIR-V module.
/// Returns true on success. Prints PASS/FAIL lines to stdout.
/// `spvVertPath` / `spvFragPath` must point at real SPIR-V files on disk.
bool runVulkanResourceSmoke(VkContext &context,
							const std::string &spvVertPath,
							const std::string &spvFragPath);
