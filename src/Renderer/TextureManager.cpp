#include "TextureManager.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image/stb_image.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

TextureManager::~TextureManager()
{
	shutdown();
}

void TextureManager::shutdown()
{
	if (!m_context)
		return;
	if (m_sampler != VK_NULL_HANDLE)
	{
		vkDestroySampler(m_context->getDevice(), m_sampler, nullptr);
		m_sampler = VK_NULL_HANDLE;
	}
	if (m_image.image != VK_NULL_HANDLE)
		destroyImage(m_context->getAllocator(), m_context->getDevice(), m_image);
	m_context = nullptr;
}

void TextureManager::initialize(VkContext &context, ImmediateCommands &imm)
{
	shutdown();
	m_context = &context;
	textures.assign(static_cast<size_t>(TextureType::COUNT), TextureInfo{});

	constexpr uint32_t kW = 64, kH = 64;
	const uint32_t layers = static_cast<uint32_t>(TextureType::COUNT);
	const VkDeviceSize layerBytes = kW * kH * 4;
	std::vector<uint8_t> atlas(static_cast<size_t>(layerBytes) * layers, 255);

	const std::string path = RES_PATH;

	auto put = [&](TextureType type, const std::string &file, bool tr, bool biome) {
		loadLayer(path + file, type, tr, biome, atlas, kW, kH);
	};

	put(BEDROCK, "textures/bedrock.png", false, false);
	put(BRICKS, "textures/bricks.png", false, false);
	put(COBBLESTONE, "textures/cobblestone.png", false, false);
	put(DIRT, "textures/dirt.png", false, false);
	put(GLASS, "textures/glass.png", true, false);
	put(GRASS_TOP, "textures/grass_block_top.png", false, true);
	put(GRASS_SIDE, "textures/grass_block_side.png", false, true);
	put(GRAVEL, "textures/gravel.png", false, false);
	put(OAK_LEAVES, "textures/oak_leaves.png", true, true);
	put(OAK_LOG_TOP, "textures/oak_log_top.png", false, false);
	put(OAK_LOG, "textures/oak_log.png", false, false);
	put(OAK_PLANKS, "textures/oak_planks.png", false, false);
	put(SAND, "textures/sand.png", false, false);
	put(SNOW, "textures/snow.png", false, false);
	put(STONE_BRICKS, "textures/stone_bricks.png", false, false);
	put(STONE, "textures/stone.png", false, false);
	put(COAL_ORE, "textures/coal_ore.png", false, false);
	put(COPPER_ORE, "textures/copper_ore.png", false, false);
	put(DIAMOND_ORE, "textures/diamond_ore.png", false, false);
	put(EMERALD_ORE, "textures/emerald_ore.png", false, false);
	put(GOLD_ORE, "textures/gold_ore.png", false, false);
	put(IRON_ORE, "textures/iron_ore.png", false, false);
	put(LAPIS_ORE, "textures/lapis_ore.png", false, false);
	put(REDSTONE_ORE, "textures/redstone_ore.png", false, false);
	loadWaterLayer(path + "textures/water_still.png", WATER, true, true, atlas, kW, kH);

	// Create 2D array image
	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.extent = {kW, kH, 1};
	imageInfo.mipLevels = 1;
	imageInfo.arrayLayers = layers;
	imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

	m_image.format = VK_FORMAT_R8G8B8A8_UNORM;
	m_image.width = kW;
	m_image.height = kH;
	m_image.mipLevels = 1;
	m_image.arrayLayers = layers;

	if (vmaCreateImage(context.getAllocator(), &imageInfo, &allocInfo, &m_image.image, &m_image.allocation, nullptr) != VK_SUCCESS)
		throw std::runtime_error("Failed to create texture array image");

	VkImageViewCreateInfo viewInfo{};
	viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewInfo.image = m_image.image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
	viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	viewInfo.subresourceRange.baseMipLevel = 0;
	viewInfo.subresourceRange.levelCount = 1;
	viewInfo.subresourceRange.baseArrayLayer = 0;
	viewInfo.subresourceRange.layerCount = layers;

	if (vkCreateImageView(context.getDevice(), &viewInfo, nullptr, &m_image.view) != VK_SUCCESS)
		throw std::runtime_error("Failed to create texture array view");

	// Staging upload all layers
	const VkDeviceSize totalSize = layerBytes * layers;
	AllocatedBuffer staging = createBuffer(
		context.getAllocator(), totalSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
	writeBuffer(context.getAllocator(), staging, atlas.data(), totalSize);

	imm.submitAndWait([&](VkCommandBuffer cmd) {
		cmdTransitionImageLayout(cmd, m_image.image, VK_IMAGE_LAYOUT_UNDEFINED,
								 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, layers);

		std::vector<VkBufferImageCopy> regions(layers);
		for (uint32_t layer = 0; layer < layers; ++layer)
		{
			regions[layer] = {};
			regions[layer].bufferOffset = layerBytes * layer;
			regions[layer].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			regions[layer].imageSubresource.mipLevel = 0;
			regions[layer].imageSubresource.baseArrayLayer = layer;
			regions[layer].imageSubresource.layerCount = 1;
			regions[layer].imageExtent = {kW, kH, 1};
		}
		vkCmdCopyBufferToImage(cmd, staging.buffer, m_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							   static_cast<uint32_t>(regions.size()), regions.data());

		cmdTransitionImageLayout(cmd, m_image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
								 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT, 1, layers);
	});

	destroyBuffer(context.getAllocator(), staging);

	VkSamplerCreateInfo samplerInfo{};
	samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	samplerInfo.magFilter = VK_FILTER_NEAREST;
	samplerInfo.minFilter = VK_FILTER_NEAREST;
	samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerInfo.anisotropyEnable = VK_FALSE;
	samplerInfo.maxAnisotropy = 1.0f;
	samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
	samplerInfo.unnormalizedCoordinates = VK_FALSE;
	samplerInfo.compareEnable = VK_FALSE;
	samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	if (vkCreateSampler(context.getDevice(), &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS)
		throw std::runtime_error("Failed to create texture sampler");

	std::cout << "Texture array: " << layers << " layers (" << kW << "x" << kH << ")\n";
}

void TextureManager::loadLayer(const std::string &path, TextureType type, bool hasTransparency, bool hasBiomeColoring,
							   std::vector<uint8_t> &atlasPixels, uint32_t layerWidth, uint32_t layerHeight)
{
	textures[static_cast<int>(type)].hasTransparency = hasTransparency;
	textures[static_cast<int>(type)].hasBiomeColoring = hasBiomeColoring;
	textures[static_cast<int>(type)].id = static_cast<unsigned int>(type);

	int width = 0, height = 0, channels = 0;
	unsigned char *data = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
	if (!data)
	{
		std::cerr << "Failed to load texture: " << path << "\n";
		return;
	}

	const size_t layerOffset = static_cast<size_t>(type) * layerWidth * layerHeight * 4;
	const uint32_t copyW = std::min(static_cast<uint32_t>(width), layerWidth);
	const uint32_t copyH = std::min(static_cast<uint32_t>(height), layerHeight);
	for (uint32_t y = 0; y < copyH; ++y)
	{
		std::memcpy(atlasPixels.data() + layerOffset + (y * layerWidth) * 4,
					data + (y * static_cast<uint32_t>(width)) * 4,
					copyW * 4);
	}
	stbi_image_free(data);
}

void TextureManager::loadWaterLayer(const std::string &path, TextureType type, bool hasTransparency, bool hasBiomeColoring,
									std::vector<uint8_t> &atlasPixels, uint32_t layerWidth, uint32_t layerHeight)
{
	textures[static_cast<int>(type)].hasTransparency = hasTransparency;
	textures[static_cast<int>(type)].hasBiomeColoring = hasBiomeColoring;
	textures[static_cast<int>(type)].id = static_cast<unsigned int>(type);

	int width = 0, height = 0, channels = 0;
	unsigned char *data = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
	if (!data)
	{
		std::cerr << "Failed to load water texture: " << path << "\n";
		return;
	}

	const size_t layerOffset = static_cast<size_t>(type) * layerWidth * layerHeight * 4;
	for (uint32_t y = 0; y < layerHeight; ++y)
	{
		for (uint32_t x = 0; x < layerWidth; ++x)
		{
			const size_t src = (static_cast<size_t>(y) * static_cast<uint32_t>(width) + x) * 4;
			const size_t dst = layerOffset + (y * layerWidth + x) * 4;
			if (x < static_cast<uint32_t>(width) && y < static_cast<uint32_t>(height))
				std::memcpy(atlasPixels.data() + dst, data + src, 4);
		}
	}
	stbi_image_free(data);
}

bool TextureManager::hasTransparency(TextureType type) const
{
	return textures[static_cast<int>(type)].hasTransparency;
}

bool TextureManager::hasBiomeColoring(TextureType type) const
{
	return textures[static_cast<int>(type)].hasBiomeColoring;
}
