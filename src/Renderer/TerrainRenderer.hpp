#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkSwapchain.hpp"
#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/VkImage.hpp"
#include "Vulkan/VkCommands.hpp"
#include "Renderer/TextureManager.hpp"
#include "Renderer/PostStack.hpp"
#include "Renderer/OverlayRenderer.hpp"
#include "Camera/Camera.hpp"
#include "Chunk/Chunk.hpp"
#include "Engine/EngineDefs.hpp"

#include <glm/glm.hpp>
#include <vector>
#include <memory>
#include <functional>
#include <array>

/// Terrain + shadows + water + sky + post (PR5).
class TerrainRenderer
{
public:
	static constexpr uint32_t kMaxFramesInFlight = 2;
	static constexpr uint32_t kShadowMapSize = 2048;

	// Must match GLSL FrameUBO (std140)
	struct FrameUBO
	{
		glm::mat4 view;
		glm::mat4 projection;
		glm::mat4 lightSpaceMatrix;
		glm::vec4 viewPos;
		glm::vec4 lightDirection;
		glm::vec4 fogColor;
		glm::vec4 fogParams;
		glm::vec4 lightParams;
		glm::vec4 visualParams;
		glm::vec4 sunDir;
		glm::vec4 moonDir;
		glm::vec4 skyParams;	 // time, day, sunset, night
		glm::vec4 postParams0; // unused by terrain
		glm::vec4 postParams1;
		glm::vec4 postParams2;
		glm::vec4 postParams3;
	};

	TerrainRenderer() = default;
	~TerrainRenderer();

	void init(VkContext &context, VkSwapchain &swapchain, ImmediateCommands &imm);
	void shutdown();
	void onSwapchainRecreate(VkSwapchain &swapchain);

	void updateFrameUBO(uint32_t frameIndex, const Camera &camera, float aspectW, float aspectH, float farPlane,
						float time, const ShaderParameters &params);

	void recordFrame(uint32_t frameIndex,
					 uint32_t imageIndex,
					 VkSwapchain &swapchain,
					 const std::vector<Chunk *> &chunks,
					 const VkClearColorValue &clearColor,
					 const std::function<void(VkCommandBuffer)> &imguiDraw = {});

	PostProcessSettings &postSettings() { return m_postSettings; }
	OverlayRenderer &overlays() { return m_overlays; }
	TextureManager &getTextureManager() { return m_textures; }
	VmaAllocator getAllocator() const { return m_context->getAllocator(); }

	bool beginAcquire(VkSwapchain &swapchain, uint32_t &outImageIndex, uint32_t frameIndex);
	bool submitPresent(VkSwapchain &swapchain, uint32_t imageIndex, uint32_t frameIndex);

private:
	struct FrameData
	{
		VkCommandPool pool{VK_NULL_HANDLE};
		VkCommandBuffer cmd{VK_NULL_HANDLE};
		VkSemaphore imageAvailable{VK_NULL_HANDLE};
		VkSemaphore renderFinished{VK_NULL_HANDLE};
		VkFence inFlight{VK_NULL_HANDLE};
		AllocatedBuffer ubo{};
		void *uboMapped{nullptr};
		VkDescriptorSet descriptorSet0{VK_NULL_HANDLE};
	};

	void createShadowResources();
	void destroyShadowResources();
	void createDescriptors();
	void createPipelines(VkSwapchain &swapchain);
	void destroyPipelines();
	void createFrameResources();
	void destroyFrameResources();
	void writeSet1Descriptors();

	static glm::mat4 computeLightSpaceMatrix(const glm::vec3 &cameraPos, const glm::vec3 &lightDir);

	VkContext *m_context{nullptr};
	TextureManager m_textures;
	ImmediateCommands *m_imm{nullptr};
	PostStack m_post;
	OverlayRenderer m_overlays;
	PostProcessSettings m_postSettings{};

	VkDescriptorSetLayout m_setLayout0{VK_NULL_HANDLE};
	VkDescriptorSetLayout m_setLayout1{VK_NULL_HANDLE};
	VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};

	VkPipeline m_opaquePipeline{VK_NULL_HANDLE};
	VkPipeline m_waterPipeline{VK_NULL_HANDLE};
	VkPipeline m_shadowPipeline{VK_NULL_HANDLE};

	VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
	VkDescriptorSet m_set1{VK_NULL_HANDLE};

	VkFormat m_depthFormat{VK_FORMAT_D32_SFLOAT};

	AllocatedImage m_shadowMap{};
	VkSampler m_shadowSampler{VK_NULL_HANDLE};

	std::array<FrameData, kMaxFramesInFlight> m_frames{};
	std::vector<VkFence> m_imagesInFlight;

	glm::vec3 m_lightDir{0.4f, 1.0f, 0.2f};
	float m_time{0.f};
	glm::vec3 m_lastCamPos{0.f};
};
