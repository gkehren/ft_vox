#pragma once

#include <Renderer/GameUiDrawList.hpp>
#include <Vulkan/VkFrame.hpp>
#include <Vulkan/VkImage.hpp>

class TextureManager;

namespace gameui
{
inline constexpr VkFormat kLinearTargetFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct RenderStats
{
	uint32_t vertices{}, draws{}, dropped{}, descriptorSets{};
	VkDeviceSize instanceBytes{}, fontBytes{}, compositionBytes{};
};

struct RecordedStats
{
	uint64_t frameNumber{};
	RenderStats metrics{};
};

class Renderer
{
public:
	Renderer() = default;
	~Renderer();
	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;

	void init(VkContext &context, ImmediateCommands &immediate, VkExtent2D fontExtent,
			  std::span<const uint8_t> coverage, VkFormat format, VkColorSpaceKHR colorSpace);
	// Use the host's swapchain recreation barrier for format-dependent pipelines.
	void setOutput(VkFormat format, VkColorSpaceKHR colorSpace);
	// Prepare only after waiting on this slot's fence. Atlas is borrowed.
	void prepare(uint32_t slot, VkExtent2D extent, const DrawList &list,
				 const TextureManager &atlas, uint64_t currentGeneration);
	bool hasDraws(uint32_t slot) const { return m_slots[slot].batchCount != 0; }
	const AllocatedImage *linearTarget(uint32_t slot) const;
	void record(VkCommandBuffer cmd, uint32_t slot, VkExtent2D extent,
				VkImage output, VkImageView outputView, VkGpuProfiler *gpu);
	RenderStats stats(uint32_t slot) const;

private:
	struct DrawBatch
	{
		VkRect2D scissor;
		uint32_t first;
		uint32_t count;
	};
	struct Slot
	{
		AllocatedBuffer instances{};
		VkDescriptorSet images{};
		VkDescriptorSet encode{};
		AllocatedImage linear{};
		uint64_t atlasGeneration{};
		bool atlasBound{};
		glm::vec2 ndc{};
		std::array<DrawBatch, DrawList::kMaxBatches> batches{};
		uint32_t batchCount{}, instancesUsed{}, dropped{};
	};

	void createDescriptors();
	void ensureLinearTarget(Slot &slot, VkExtent2D extent);

	VkContext *m_context{};
	AllocatedImage m_font{};
	VkSampler m_fontSampler{};
	VkDescriptorPool m_pool{};
	VkDescriptorSetLayout m_imageLayout{}, m_encodeSetLayout{};
	VkPipelineLayout m_drawLayout{}, m_encodeLayout{};
	VkPipeline m_drawPipe{}, m_encodePipe{};
	VkFormat m_outputFormat{VK_FORMAT_UNDEFINED};
	bool m_linear{};
	std::array<Slot, VkFrameContext::kMaxFramesInFlight> m_slots{};
};
} // namespace gameui
