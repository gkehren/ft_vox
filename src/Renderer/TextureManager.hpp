#pragma once

#include <Vulkan/VkImage.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/VkContext.hpp>

#include <string>
#include <vector>
#include <utils.hpp>

/// Vulkan 2D texture array atlas (64x64 layers for each TextureType).
class TextureManager
{
public:
	TextureManager() = default;
	~TextureManager();

	TextureManager(const TextureManager &) = delete;
	TextureManager &operator=(const TextureManager &) = delete;

	void initialize(VkContext &context, ImmediateCommands &imm);
	void shutdown();

	VkImageView getImageView() const { return m_image.view; }
	VkSampler getSampler() const { return m_sampler; }
	bool isValid() const { return m_image.image != VK_NULL_HANDLE; }

	bool hasTransparency(TextureType type) const;
	bool hasBiomeColoring(TextureType type) const;

	static bool isTransparent(TextureType type)
	{
		return type == TextureType::GLASS || type == TextureType::OAK_LEAVES ||
			   type == TextureType::WATER;
	}

private:
	void loadLayer(const std::string &path, TextureType type, bool hasTransparency, bool hasBiomeColoring,
				   std::vector<uint8_t> &atlasPixels, uint32_t layerWidth, uint32_t layerHeight);
	void loadWaterLayer(const std::string &path, TextureType type, bool hasTransparency, bool hasBiomeColoring,
						std::vector<uint8_t> &atlasPixels, uint32_t layerWidth, uint32_t layerHeight);

	VkContext *m_context{nullptr};
	AllocatedImage m_image{};
	VkSampler m_sampler{VK_NULL_HANDLE};
	std::vector<TextureInfo> textures;
};
