#include "Vulkan/StagingRing.hpp"

#include <stdexcept>

void StagingRing::init(VmaAllocator allocator, uint32_t framesInFlight, VkDeviceSize totalSize)
{
	if (allocator == VK_NULL_HANDLE || totalSize == 0)
		throw std::runtime_error("StagingRing::init: invalid args");

	shutdown();

	m_allocator = allocator;
	m_framesInFlight = framesInFlight > 0 ? framesInFlight : 2;
	// Align total size down to a multiple of frames so slices are equal.
	m_totalSize = (totalSize / m_framesInFlight) * m_framesInFlight;
	if (m_totalSize < m_framesInFlight * 1024 * 1024)
		m_totalSize = m_framesInFlight * 4ull * 1024ull * 1024ull; // at least 4 MiB / frame
	m_sliceSize = m_totalSize / m_framesInFlight;

	m_buffer = createBuffer(
		allocator,
		m_totalSize,
		VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		VMA_MEMORY_USAGE_AUTO,
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);

	m_mapped = m_buffer.info.pMappedData;
	if (!m_mapped)
		m_mapped = mapBuffer(allocator, m_buffer);

	m_sliceStart = 0;
	m_head = 0;
}

void StagingRing::shutdown()
{
	telemetry::registry().set(telemetry::StagingUsed, 0);
	if (m_allocator != VK_NULL_HANDLE && m_buffer.buffer != VK_NULL_HANDLE)
	{
		// Only unmap if we mapped explicitly (VMA MAPPED keeps it mapped).
		if (!m_buffer.info.pMappedData && m_mapped)
			unmapBuffer(m_allocator, m_buffer);
		destroyBuffer(m_allocator, m_buffer);
	}
	m_mapped = nullptr;
	m_allocator = VK_NULL_HANDLE;
	m_totalSize = 0;
	m_sliceSize = 0;
	m_sliceStart = 0;
	m_head = 0;
}

void StagingRing::beginFrame(uint32_t frameIndex)
{
	if (!isValid())
		return;
	const uint32_t idx = m_framesInFlight > 0 ? (frameIndex % m_framesInFlight) : 0;
	m_sliceStart = static_cast<VkDeviceSize>(idx) * m_sliceSize;
	m_head = m_sliceStart;
	telemetry::registry().set(telemetry::StagingUsed, 0);
}

bool StagingRing::alloc(VkDeviceSize size, VkDeviceSize &outOffset, void *&outPtr)
{
	if (!isValid() || !m_mapped || size == 0)
		return false;

	const VkDeviceSize aligned = (size + kAlignment - 1) & ~(kAlignment - 1);
	const VkDeviceSize sliceEnd = m_sliceStart + m_sliceSize;
	if (m_head + aligned > sliceEnd) {
		telemetry::registry().add(telemetry::StagingFailures);
		return false;
	}

	outOffset = m_head;
	outPtr = static_cast<char *>(m_mapped) + m_head;
	m_head += aligned;
	telemetry::registry().set(telemetry::StagingUsed, usedThisFrame());
	return true;
}
