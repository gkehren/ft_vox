#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkGpuProfiler.hpp"
#include "Vulkan/VkImage.hpp"
#include "Chunk/Chunk.hpp"
#include "Renderer/OverlayRenderer.hpp"

#include <volk.h>
#include <array>
#include <vector>
#include <cstdint>

/// Opaque HDR terrain + overlays into color/depth targets.
class OpaquePass
{
public:
	void init(VkContext &context);
	void shutdown();

	void createPipeline(VkPipelineLayout layout, const VkPipelineVertexInputStateCreateInfo &vertexInput,
						VkFormat colorFmt, VkFormat depthFmt);
	void destroyPipeline();

	void record(VkCommandBuffer cmd, VkExtent2D extent, VkDescriptorSet set0, VkDescriptorSet set1,
				VkPipelineLayout layout, AllocatedImage &hdr, AllocatedImage &depth,
				const std::vector<Chunk *> &chunks, OverlayRenderer &overlays,
				const VkClearColorValue &clearColor, uint32_t frameIndex, const MeshArenas &arenas,
				VkGpuProfiler *gpu = nullptr);

private:
	void createIndirectBuffers();
	void destroyIndirectBuffers();

	// Host-written indirect command storage, one slot per frame in flight
	// (issue #109): commands change every frame, so the CPU rewrites its
	// slot while the previous frame's copy is still in flight elsewhere.
	static constexpr uint32_t kMaxIndirectCommands = 65536;
	struct IndirectBatch
	{
		AllocatedBuffer buf{};
		void *mapped{nullptr};
	};
	VkContext *m_context{nullptr};
	VkPipeline m_pipeline{VK_NULL_HANDLE};
	std::array<IndirectBatch, 2> m_indirect{};
	// Reused per-frame scratch: collecting ~15k commands must not
	// reallocate every frame (issue #109).
	std::vector<Chunk::IndirectDraw> m_scratch;
};
