#pragma once

#include <Vulkan/VkImage.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/VkContext.hpp>
#include <Renderer/MinecraftTextures.hpp>

#include <string>
#include <utils.hpp>

/// Vulkan 2D texture array atlas (one layer per TextureType, size from pack / assets).
/// Hot-reload is failure-atomic: a failed initialize leaves the previous atlas live.
class TextureManager
{
public:
	TextureManager() = default;
	~TextureManager();

	TextureManager(const TextureManager &) = delete;
	TextureManager &operator=(const TextureManager &) = delete;

	/// Build (or rebuild) the atlas from pack root (.zip or directory).
	/// Empty pack root → default pack `{RES_PATH}default-resource-pack.zip`.
	/// On failure after a successful prior init, the previous image/sampler remain valid.
	/// Returns pack hit/miss stats (atlas is still valid even if pack is invalid — default pack used).
	TextureAtlasLoadReport initialize(VkContext &context, ImmediateCommands &imm,
									  const std::string &resourcePackRoot = {});
	void shutdown();

	VkImageView getImageView() const { return m_image.view; }
	VkSampler getSampler() const { return m_sampler; }
	bool isValid() const { return m_image.image != VK_NULL_HANDLE; }
	uint32_t layerSize() const { return m_layerSize; }
	const TextureAtlasLoadReport &lastLoadReport() const { return m_lastReport; }

	/// Transparency policy from kBlockLayers (single source of truth).
	static bool isTransparent(TextureType type) { return blockLayerIsTransparent(type); }

private:
	static void nearestNeighborScale(const unsigned char *src, int srcW, int srcH,
									 uint8_t *dst, uint32_t dstW, uint32_t dstH);
	void destroyGpuResources();

	VkContext *m_context{nullptr};
	AllocatedImage m_image{};
	VkSampler m_sampler{VK_NULL_HANDLE};
	uint32_t m_layerSize{0};
	TextureAtlasLoadReport m_lastReport{};
};
