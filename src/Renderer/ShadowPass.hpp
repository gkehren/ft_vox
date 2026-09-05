#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkImage.hpp"
#include "Renderer/ShadowCascades.hpp"
#include "Chunk/Chunk.hpp"
#include "Vulkan/MeshArena.hpp"

#include <glm/glm.hpp>
#include <array>
#include <vector>
#include <cstdint>

/// Cascaded shadow maps: resources + depth-only record.
class ShadowPass
{
public:
	static constexpr int kCascadeCount = shadow::kCascadeCount;
	static constexpr uint32_t kShadowMapSize = shadow::kShadowMapSize;

	void init(VkContext &context);
	void shutdown();

	void createPipeline(VkPipelineLayout layout, const VkPipelineVertexInputStateCreateInfo &vertexInput);
	void destroyPipeline();

	VkImageView arrayView() const { return m_shadowMap.view; }
	VkSampler sampler() const { return m_shadowSampler; }
	const std::array<VkImageView, kCascadeCount> &layerViews() const { return m_shadowLayerViews; }

	static constexpr uint32_t kMaxIndirectCommands = 65536;
	struct IndirectBatch
	{
		AllocatedBuffer buf{};
		void *mapped{nullptr};
	};
	void record(VkCommandBuffer cmd, uint32_t frameIndex, const std::vector<Chunk *> &shadowChunks,
				const std::array<glm::mat4, kCascadeCount> &cascades, float time,
						const MeshArenas &arenas);

private:
	void createResources();
	void destroyResources();

	VkContext *m_context{nullptr};
	VkFormat m_depthFormat{VK_FORMAT_D32_SFLOAT};
	AllocatedImage m_shadowMap{};
	std::array<VkImageView, kCascadeCount> m_shadowLayerViews{};
	VkSampler m_shadowSampler{VK_NULL_HANDLE};
	VkPipeline m_pipeline{VK_NULL_HANDLE};
	VkPipelineLayout m_layout{VK_NULL_HANDLE};

	void createIndirectBuffers();
	void destroyIndirectBuffers();
	// One indirect storage slot per cascade per frame in flight.
	std::array<std::array<IndirectBatch, kCascadeCount>, 2> m_indirect{};

	std::vector<Chunk::IndirectDraw> m_scratch{};
};
