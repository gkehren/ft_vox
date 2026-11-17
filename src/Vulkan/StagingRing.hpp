#pragma once

#include "Vulkan/VkBuffer.hpp"

#include <cstdint>

/// Host-visible staging buffer split into FIXED per-frame-in-flight slices
/// so a frame never overwrites staging still in use by an earlier submit.
/// Semantics (review §4.7 — the "ring" name is historical): each FIF index
/// owns one exclusive slice for the whole frame; alloc() is a bump cursor
/// inside that slice and HARD-REJECTS when the slice is full (the caller
/// defers the upload). There is no wrap-around within a slice. init()
/// clamps the requested total up to kMinSliceBytes per frame and logs the
/// clamp instead of silently substituting a different size.
class StagingRing
{
public:
	static constexpr VkDeviceSize kDefaultSize = 48ull * 1024ull * 1024ull; // 48 MiB
	static constexpr VkDeviceSize kMinSliceBytes = 4ull * 1024ull * 1024ull; // 4 MiB / frame
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
	/// Consumption of the frame that just completed, captured in beginFrame()
	/// before the cursor reset. Debug/UI readers run mid-frame (after the
	/// reset, before this frame's uploads) and must read this instead of
	/// usedThisFrame() to see the last completed frame's staging traffic.
	VkDeviceSize lastFrameUsed() const { return m_lastFrameUsed; }
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
	VkDeviceSize m_lastFrameUsed{0};
};
