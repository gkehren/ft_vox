#include "TextureManager.hpp"
#include <stb_image/stb_image.h>
#include <iostream>

TextureManager::TextureManager() : textureArray(0)
{
	textures.resize(static_cast<int>(TextureType::COUNT));
}

TextureManager::~TextureManager()
{
	if (textureArray != 0)
		glDeleteTextures(1, &textureArray);
}

void TextureManager::initialize()
{
	std::string path = RES_PATH;

	// Create a 2D texture array
	glGenTextures(1, &textureArray);
	glBindTexture(GL_TEXTURE_2D_ARRAY, textureArray);

	// Initial allocation for texture array (width, height, number of textures)
	int width = 64, height = 64; // Typical Minecraft texture size
	glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, width, height,
				 static_cast<int>(TextureType::COUNT), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

	// Set texture parameters
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	// Load individual textures into array slices
	loadTexture(path + "textures/bedrock.png", TextureType::BEDROCK, false, false);
	loadTexture(path + "textures/bricks.png", TextureType::BRICKS, false, false);
	loadTexture(path + "textures/cobblestone.png", TextureType::COBBLESTONE, false, false);
	loadTexture(path + "textures/dirt.png", TextureType::DIRT, false, false);
	loadTexture(path + "textures/glass.png", TextureType::GLASS, true, false);
	loadTexture(path + "textures/grass_block_top.png", TextureType::GRASS_TOP, false, true);
	loadTexture(path + "textures/grass_block_side.png", TextureType::GRASS_SIDE, false, true);
	loadTexture(path + "textures/gravel.png", TextureType::GRAVEL, false, false);
	loadTexture(path + "textures/oak_leaves.png", TextureType::OAK_LEAVES, true, true);
	loadTexture(path + "textures/oak_log_top.png", TextureType::OAK_LOG_TOP, false, false);
	loadTexture(path + "textures/oak_log.png", TextureType::OAK_LOG, false, false);
	loadTexture(path + "textures/oak_planks.png", TextureType::OAK_PLANKS, false, false);
	loadTexture(path + "textures/sand.png", TextureType::SAND, false, false);
	loadTexture(path + "textures/snow.png", TextureType::SNOW, false, false);
	loadTexture(path + "textures/stone_bricks.png", TextureType::STONE_BRICKS, false, false);
	loadTexture(path + "textures/stone.png", TextureType::STONE, false, false);
	loadTexture(path + "textures/coal_ore.png", TextureType::COAL_ORE, false, false);
	loadTexture(path + "textures/copper_ore.png", TextureType::COPPER_ORE, false, false);
	loadTexture(path + "textures/diamond_ore.png", TextureType::DIAMOND_ORE, false, false);
	loadTexture(path + "textures/emerald_ore.png", TextureType::EMERALD_ORE, false, false);
	loadTexture(path + "textures/gold_ore.png", TextureType::GOLD_ORE, false, false);
	loadTexture(path + "textures/iron_ore.png", TextureType::IRON_ORE, false, false);
	loadTexture(path + "textures/lapis_ore.png", TextureType::LAPIS_ORE, false, false);
	loadTexture(path + "textures/redstone_ore.png", TextureType::REDSTONE_ORE, false, false);

	glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

	GLenum err;
	if ((err = glGetError()) != GL_NO_ERROR)
		std::cerr << "OpenGL error during texture array initialization: " << err << std::endl;

	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
}

void TextureManager::loadTexture(const std::string &path, TextureType type, bool hasTransparency, bool hasBiomeColoring)
{
	int width, height, channels;
	unsigned char *data = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);

	if (data)
	{
		// Store the texture info
		textures[static_cast<int>(type)].hasTransparency = hasTransparency;
		textures[static_cast<int>(type)].hasBiomeColoring = hasBiomeColoring;

		// Upload texture data to array slice
		glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, static_cast<int>(type),
						width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, data);

		stbi_image_free(data);
	}
	else
	{
		std::cout << "Failed to load texture: " << path << std::endl;
	}
}

bool TextureManager::hasTransparency(TextureType type) const
{
	return textures[static_cast<int>(type)].hasTransparency;
}

bool TextureManager::hasBiomeColoring(TextureType type) const
{
	return textures[static_cast<int>(type)].hasBiomeColoring;
}