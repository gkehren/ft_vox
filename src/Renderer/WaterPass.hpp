#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkImage.hpp"
#include "Vulkan/VkCommands.hpp"
#include "Vulkan/VkFrame.hpp"
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
				const MeshArenas &arenas, VoxelDrawData *drawDataOut, AllocatedBuffer &drawDataBuffer);

	VkSampler sceneSampler() const { return m_sceneSampler; }

	// Indirect commands demanded by the last record (pre-truncation);
	// WorldRenderer aggregates the passes for the benchmark's peak.
	uint32_t lastCommands() const { return m_lastCommands; }

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
	std::array<IndirectBatch, VkFrameContext::kMaxFramesInFlight> m_indirect{};

	std::vector<Chunk::IndirectDraw> m_scratch{};
	uint32_t m_lastCommands{0};
};
