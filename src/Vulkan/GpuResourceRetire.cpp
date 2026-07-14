#include "Vulkan/GpuResourceRetire.hpp"

void GpuResourceRetire::init(VmaAllocator allocator, uint32_t framesInFlight)
{
	m_allocator = allocator;
	m_delay = framesInFlight > 0 ? framesInFlight : 2;
	m_frame = 0;
	m_pending.clear();
	m_pending.reserve(256);
}

void GpuResourceRetire::shutdown()
{
	flush();
	m_allocator = VK_NULL_HANDLE;
}

void GpuResourceRetire::beginFrame(uint64_t frameNumber)
{
	m_frame = frameNumber;
	if (m_allocator == VK_NULL_HANDLE || m_pending.empty())
		return;

	size_t write = 0;
	for (size_t i = 0; i < m_pending.size(); ++i)
	{
		if (m_frame >= m_pending[i].destroyAfterFrame)
		{
			destroyBuffer(m_allocator, m_pending[i].buffer);
		}
		else
		{
			if (write != i)
				m_pending[write] = std::move(m_pending[i]);
			++write;
		}
	}
	m_pending.resize(write);
}

void GpuResourceRetire::retireBuffer(AllocatedBuffer buffer)
{
	if (buffer.buffer == VK_NULL_HANDLE)
		return;
	Entry e;
	e.buffer = buffer;
	e.destroyAfterFrame = m_frame + m_delay;
	m_pending.push_back(e);
}

void GpuResourceRetire::flush()
{
	if (m_allocator == VK_NULL_HANDLE)
	{
		m_pending.clear();
		return;
	}
	for (auto &e : m_pending)
		destroyBuffer(m_allocator, e.buffer);
	m_pending.clear();
}
