#pragma once

#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/VkCommands.hpp"

#include <cstddef>

/// Stage CPU data into a DEVICE_LOCAL (or AUTO_PREFER_DEVICE) buffer via a host-visible staging buffer.
void uploadBuffer(VmaAllocator allocator,
				  ImmediateCommands &imm,
				  AllocatedBuffer &dst,
				  const void *data,
				  VkDeviceSize size,
				  VkDeviceSize dstOffset = 0);
