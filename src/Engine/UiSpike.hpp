#pragma once

#include <SDL3/SDL_events.h>
#include <Vulkan/VkFrame.hpp>
#include <Vulkan/VkImage.hpp>
#include <glm/vec4.hpp>

#include <array>

class VkContext;

/// Opt-in architecture proof for #299. Engine owns event and frame boundaries.
class UiSpike
{
public:
	UiSpike() = default;
	~UiSpike();
	UiSpike(const UiSpike &) = delete;
	UiSpike &operator=(const UiSpike &) = delete;

	void init(VkContext &context, ImmediateCommands &immediate, VkFormat target);
	// Called at the existing swapchain idle barrier.
	void setTargetFormat(VkFormat target);
	// Returns true for input owned by this modal preview. Window events pass through.
	bool handleEvent(SDL_Window *window, const SDL_Event &event);
	void record(VkCommandBuffer cmd, uint32_t frameIndex, SDL_Window *window, VkExtent2D extent);

private:
	struct Glyph
	{
		glm::vec4 rect{};
		glm::vec4 uv{};
		float advance{};
		bool visible{};
	};

	void createAtlas(ImmediateCommands &immediate);
	void activate(const char *source);

	VkDevice m_device{};
	VmaAllocator m_allocator{};
	VkFormat m_target{VK_FORMAT_UNDEFINED};
	VkDescriptorSetLayout m_setLayout{};
	VkDescriptorPool m_pool{};
	VkDescriptorSet m_atlasSet{};
	VkPipelineLayout m_layout{};
	VkPipeline m_pipeline{};
	VkSampler m_sampler{};
	AllocatedImage m_atlas{};
	std::array<AllocatedBuffer, VkFrameContext::kMaxFramesInFlight> m_instances{};
	std::array<Glyph, 128> m_glyphs{};
	glm::vec4 m_whiteUv{};
	glm::vec4 m_sampleUv{};
	unsigned m_activations{};
	bool m_focused{};
	bool m_armed{};
};
