#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkGpuProfiler.hpp"
#include "Vulkan/VkSwapchain.hpp"
#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/VkImage.hpp"
#include "Vulkan/VkCommands.hpp"
#include "Vulkan/MeshArena.hpp"
#include "Renderer/TextureManager.hpp"
#include "Renderer/PostStack.hpp"
#include "Renderer/OverlayRenderer.hpp"
#include "Renderer/ShadowCascades.hpp"
#include "Renderer/Lighting.hpp"
#include "Renderer/FrameUBO.hpp"
#include "Renderer/ShadowPass.hpp"
#include "Renderer/OpaquePass.hpp"
#include "Renderer/WaterPass.hpp"
#include "Renderer/SkyPass.hpp"
#include "Camera/Camera.hpp"
#include "Chunk/Chunk.hpp"
#include "Engine/EngineDefs.hpp"

#include <glm/glm.hpp>
#include <vector>
#include <functional>
#include <array>
#include <string>

/// Thin world-frame orchestrator: sequences Shadow → Opaque → Water → Sky → Post.
class WorldRenderer
{
public:
	static constexpr uint32_t kMaxFramesInFlight = 2;
	static constexpr int kCascadeCount = shadow::kCascadeCount;

	WorldRenderer() = default;
	~WorldRenderer();

	/// @param resourcePackRoot Optional Minecraft pack root (see TextureManager).
	void init(VkContext &context, VkSwapchain &swapchain, ImmediateCommands &imm,
			  GpuResourceRetire &retire, const std::string &resourcePackRoot = {});
	void shutdown();
	void onSwapchainRecreate(VkSwapchain &swapchain);

	/// Rebuild block texture array from pack root (empty = bundled). Caller must device-idle first.
	/// Does not store pack path — Engine owns the authoritative path.
	/// Returns pack hit/miss stats (atlas still valid when pack is invalid — bundled used).
	TextureAtlasLoadReport reloadResourcePack(const std::string &resourcePackRoot);
	uint32_t textureLayerSize() const { return m_textures.layerSize(); }
	const TextureAtlasLoadReport &lastTextureLoadReport() const { return m_textures.lastLoadReport(); }

	void updateFrameUBO(uint32_t frameIndex, const Camera &camera, float aspectW, float aspectH, float farPlane,
						float time, const ShaderParameters &params, float shadowCascadeFar = 280.f,
						bool underwater = false);

	/// Record full frame into an already-reset command buffer (from VkFrameContext).
	void recordFrame(VkCommandBuffer cmd, uint32_t frameIndex, uint32_t imageIndex, VkSwapchain &swapchain,
					 const std::vector<Chunk *> &chunks, const std::vector<Chunk *> &shadowChunks,
					 const VkClearColorValue &clearColor,
					 const std::function<void(VkCommandBuffer)> &preRecord = {},
					 const std::function<void(VkCommandBuffer)> &imguiDraw = {},
                     VkGpuProfiler *gpu = nullptr, uint64_t benchmarkTag = 0);

	PostProcessSettings &postSettings() { return m_postSettings; }
	OverlayRenderer &overlays() { return m_overlays; }
	TextureManager &getTextureManager() { return m_textures; }
	/// Shared device-local mesh arenas every chunk suballocates from (issue #109).
	MeshArenas &arenas() { return m_arenas; }
	VmaAllocator getAllocator() const { return m_context->getAllocator(); }

private:
	struct FrameUboSlot
	{
		AllocatedBuffer ubo{};
		void *uboMapped{nullptr};
		VkDescriptorSet descriptorSet0{VK_NULL_HANDLE};
	};

	void createDescriptors();
	void createPipelineLayouts();
	void createPipelines();
	void destroyPipelines();
	void createFrameUbos();
	void destroyFrameUbos();
	void writeSet1Descriptors();
	void writeMaterialDescriptors();

	VkContext *m_context{nullptr};
	ImmediateCommands *m_imm{nullptr};
	TextureManager m_textures;
	PostStack m_post;
	OverlayRenderer m_overlays;
	PostProcessSettings m_postSettings{};
	MeshArenas m_arenas{};
	ShadowPass m_shadow;
	OpaquePass m_opaque;
	WaterPass m_water;
	SkyPass m_sky;

	VkDescriptorSetLayout m_setLayout0{VK_NULL_HANDLE};
	VkDescriptorSetLayout m_setLayout1{VK_NULL_HANDLE};
	VkDescriptorSetLayout m_setLayout2{VK_NULL_HANDLE};
	VkPipelineLayout m_pipelineLayout{VK_NULL_HANDLE};
	VkPipelineLayout m_shadowPipelineLayout{VK_NULL_HANDLE};
	VkPipelineLayout m_waterPipelineLayout{VK_NULL_HANDLE};
	VkDescriptorPool m_descriptorPool{VK_NULL_HANDLE};
	VkDescriptorSet m_set1{VK_NULL_HANDLE};
	VkDescriptorSet m_set2Water{VK_NULL_HANDLE};

	AllocatedBuffer m_materialUbo{};
	void *m_materialMapped{nullptr};

	std::array<FrameUboSlot, kMaxFramesInFlight> m_frameUbos{};
	glm::vec3 m_lightDir{0.4f, 1.0f, 0.2f};
	float m_time{0.f};
	glm::vec3 m_lastCamPos{0.f};
	std::array<glm::mat4, kCascadeCount> m_cascadeMatrices{};
};
