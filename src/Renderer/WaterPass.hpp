#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkImage.hpp"
#include "Vulkan/VkCommands.hpp"
#include "Chunk/Chunk.hpp"
#include "Vulkan/MeshArena.hpp"

#include <glm/glm.hpp>
#include <vector>
#include <cstdint>

/// Opaque scene history + transparent water pass.
class WaterPass
{
public:
	void init(VkContext &context, ImmediateCommands &imm, uint32_t width, uint32_t height, VkFormat depthFmt);
	void shutdown();
	void resize(uint32_t width, uint32_t height, VkFormat depthFmt);

	void createPipeline(VkPipelineLayout layout, const VkPipelineVertexInputStateCreateInfo &vertexInput,
						VkFormat colorFmt, VkFormat depthFmt);
	void destroyPipeline();

	void writeSceneDescriptors(VkDescriptorSet set2, VkSampler sampler);

	void record(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent, VkDescriptorSet set0, VkDescriptorSet set1,
				VkDescriptorSet set2, VkPipelineLayout layout, AllocatedImage &hdr, AllocatedImage &liveDepth,
				const std::vector<Chunk *> &chunks, const glm::vec3 &camPos,
						const MeshArenas &arenas);

	VkSampler sceneSampler() const { return m_sceneSampler; }

private:
	void createHistory(uint32_t w, uint32_t h, VkFormat depthFmt);
	void destroyHistory();

	VkContext *m_context{nullptr};
	VkFormat m_depthFormat{VK_FORMAT_D32_SFLOAT};
	AllocatedImage m_sceneHistory{};
	AllocatedImage m_depthHistory{};
	VkSampler m_sceneSampler{VK_NULL_HANDLE};
	VkPipeline m_pipeline{VK_NULL_HANDLE};

	static constexpr uint32_t kMaxIndirectCommands = 65536;
	struct IndirectBatch
	{
		AllocatedBuffer buf{};
		void *mapped{nullptr};
	};
	void createIndirectBuffers();
	void destroyIndirectBuffers();
	std::array<IndirectBatch, 2> m_indirect{};

	std::vector<Chunk::IndirectDraw> m_scratch{};
};
