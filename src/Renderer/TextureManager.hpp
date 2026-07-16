#pragma once

#include <Vulkan/VkImage.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/VkContext.hpp>
#include <Renderer/MinecraftTextures.hpp>

#include <string>
#include <vector>
#include <utils.hpp>

/// Vulkan 2D texture array atlas (one layer per TextureType, size from pack / assets).
class TextureManager
{
public:
	TextureManager() = default;
	~TextureManager();

	TextureManager(const TextureManager &) = delete;
	TextureManager &operator=(const TextureManager &) = delete;

	/// @param resourcePackRoot Explicit Minecraft pack root (already resolved at process entry).
	///   Empty → bundled `{RES_PATH}textures/`. Non-empty → pack files under
	///   `{root}/assets/minecraft/textures/block/`, with warned fallback to bundled on miss.
	void initialize(VkContext &context, ImmediateCommands &imm,
					const std::string &resourcePackRoot = {});
	void shutdown();

	VkImageView getImageView() const { return m_image.view; }
	VkSampler getSampler() const { return m_sampler; }
	bool isValid() const { return m_image.image != VK_NULL_HANDLE; }

	/// Transparency policy from kBlockLayers (single source of truth).
	static bool isTransparent(TextureType type) { return blockLayerIsTransparent(type); }

private:
	static void nearestNeighborScale(const unsigned char *src, int srcW, int srcH,
									 uint8_t *dst, uint32_t dstW, uint32_t dstH);

	VkContext *m_context{nullptr};
	AllocatedImage m_image{};
	VkSampler m_sampler{VK_NULL_HANDLE};
	uint32_t m_layerSize{0};
};
