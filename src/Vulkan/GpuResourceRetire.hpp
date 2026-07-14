#pragma once

#include "Vulkan/VkBuffer.hpp"

#include <cstdint>
#include <vector>

/// Defers destruction of GPU buffers until in-flight frames that may still
/// reference them have completed. Never call vkDeviceWaitIdle for streaming.
class GpuResourceRetire
{
public:
	void init(VmaAllocator allocator, uint32_t framesInFlight);
	void shutdown();

	/// Advance the global frame counter and destroy anything past the delay.
	void beginFrame(uint64_t frameNumber);

	/// Schedule buffer destruction after framesInFlight frames from now.
	void retireBuffer(AllocatedBuffer buffer);

	/// Destroy everything immediately (caller must have waited on the device).
	void flush();

	uint64_t currentFrame() const { return m_frame; }
	size_t pendingCount() const { return m_pending.size(); }

private:
	struct Entry
	{
		AllocatedBuffer buffer{};
		uint64_t destroyAfterFrame{0};
	};

	VmaAllocator m_allocator{VK_NULL_HANDLE};
	uint32_t m_delay{2};
	uint64_t m_frame{0};
	std::vector<Entry> m_pending;
};
