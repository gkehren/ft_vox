#pragma once

#include "Vulkan/VkBuffer.hpp"

#include <cstdint>

/// Host-visible ring buffer split into per-frame-in-flight slices so a frame
/// never overwrites staging still in use by an earlier submit.
class StagingRing
{
public:
	static constexpr VkDeviceSize kDefaultSize = 48ull * 1024ull * 1024ull; // 48 MiB
	static constexpr VkDeviceSize kAlignment = 256;

	void init(VmaAllocator allocator, uint32_t framesInFlight, VkDeviceSize totalSize = kDefaultSize);
	void shutdown();

	/// Select the exclusive staging slice for this FIF index and reset its cursor.
	void beginFrame(uint32_t frameIndex);

	/// Allocate a contiguous range inside the current frame slice.
	/// Returns false if the slice is full (caller should defer remaining uploads).
	bool alloc(VkDeviceSize size, VkDeviceSize &outOffset, void *&outPtr);

	VkBuffer buffer() const { return m_buffer.buffer; }
	bool isValid() const { return m_buffer.buffer != VK_NULL_HANDLE; }

	VkDeviceSize usedThisFrame() const { return m_head - m_sliceStart; }
	VkDeviceSize sliceCapacity() const { return m_sliceSize; }

private:
	VmaAllocator m_allocator{VK_NULL_HANDLE};
	AllocatedBuffer m_buffer{};
	void *m_mapped{nullptr};
	uint32_t m_framesInFlight{2};
	VkDeviceSize m_totalSize{0};
	VkDeviceSize m_sliceSize{0};
	VkDeviceSize m_sliceStart{0};
	VkDeviceSize m_head{0};
};
