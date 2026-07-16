#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkSwapchain.hpp"
#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/VkImage.hpp"
#include "Vulkan/VkCommands.hpp"
#include "Engine/EngineDefs.hpp"
#include "Renderer/PostDefaults.hpp"

#include <glm/glm.hpp>
#include <volk.h>
#include <cstdint>

/// HDR targets + SSAO / bloom / god rays / composite.
/// Sky is owned by SkyPass; this stack only owns fullscreen post + scene HDR/depth.
class PostStack
{
public:
	PostStack() = default;
	~PostStack();

	void init(VkContext &context, ImmediateCommands &imm, VkDescriptorSetLayout frameSetLayout,
			  VkFormat swapchainFormat, uint32_t width, uint32_t height);
	void shutdown();
	void resize(uint32_t width, uint32_t height, VkFormat swapchainFormat);

	AllocatedImage &hdrColor() { return m_hdr; }
	AllocatedImage &godSource() { return m_godSource; }
	AllocatedImage &sceneDepth() { return m_sceneDepth; }
	VkFormat depthFormat() const { return m_depthFormat; }
	VkFormat hdrFormat() const { return m_hdrFormat; }

	/// Fullscreen post: SSAO → bloom → depth-aware god rays → composite.
	void recordPost(VkCommandBuffer cmd,
					VkImage swapchainImage,
					VkImageView swapchainView,
					VkExtent2D extent,
					VkDescriptorSet frameSet0,
					const PostProcessSettings &settings,
					const glm::vec2 &sunScreen,
					float sunVisibility,
					float time,
					const glm::mat4 &projection);

private:
	void createTargets(uint32_t w, uint32_t h);
	void destroyTargets();
	void createDefaultImages(ImmediateCommands &imm);
	void destroyDefaultImages();
	void createPipelines(VkFormat swapchainFormat);
	void destroyPipelines();
	void createFullscreenQuad(ImmediateCommands &imm);
	void createSamplers();
	void writeEffectDescriptors();
	void writeCompositeDescriptors(const PostCompositeSources &src);

	VkContext *m_context{nullptr};
	VkDescriptorSetLayout m_frameSetLayout{VK_NULL_HANDLE};

	VkFormat m_hdrFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
	VkFormat m_depthFormat{VK_FORMAT_D32_SFLOAT};
	uint32_t m_width{0}, m_height{0};

	AllocatedImage m_hdr{};
	AllocatedImage m_godSource{};
	AllocatedImage m_sceneDepth{};
	AllocatedImage m_bloom[2]{};
	AllocatedImage m_godRays{};
	AllocatedImage m_ssao{};

	/// Always-valid 1×1 fallbacks for disabled effects (SHADER_READ_ONLY).
	AllocatedImage m_defaultBlack{};	// HDR black — bloom / god rays off
	AllocatedImage m_defaultWhiteR8{}; // R8 white — SSAO off (ao=1)

	VkSampler m_linearSampler{VK_NULL_HANDLE};
	VkSampler m_nearestSampler{VK_NULL_HANDLE};

	AllocatedBuffer m_quadVBO{};

	VkDescriptorSetLayout m_postSetLayout{VK_NULL_HANDLE};
	VkDescriptorSetLayout m_godSetLayout{VK_NULL_HANDLE};
	VkDescriptorSetLayout m_compositeSetLayout{VK_NULL_HANDLE};
	VkDescriptorPool m_postPool{VK_NULL_HANDLE};
	VkDescriptorSet m_setExtract{VK_NULL_HANDLE};
	VkDescriptorSet m_setBlur[2]{};
	VkDescriptorSet m_setGodRays{VK_NULL_HANDLE};
	VkDescriptorSet m_setSsao{VK_NULL_HANDLE};
	VkDescriptorSet m_setComposite{VK_NULL_HANDLE};

	VkPipelineLayout m_postLayout1{VK_NULL_HANDLE};
	VkPipelineLayout m_godLayout{VK_NULL_HANDLE};
	VkPipelineLayout m_ssaoLayout{VK_NULL_HANDLE};
	VkPipelineLayout m_compositeLayout{VK_NULL_HANDLE};

	VkPipeline m_extractPipe{VK_NULL_HANDLE};
	VkPipeline m_blurPipe{VK_NULL_HANDLE};
	VkPipeline m_godRaysPipe{VK_NULL_HANDLE};
	VkPipeline m_ssaoPipe{VK_NULL_HANDLE};
	VkPipeline m_compositePipe{VK_NULL_HANDLE};

	VkFormat m_swapchainFormat{VK_FORMAT_UNDEFINED};
	PostCompositeSources m_lastCompositeSrc{true, true, true}; // force first write
};
