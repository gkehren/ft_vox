#pragma once

#include <glad/glad.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <utils.hpp>

class TextureManager
{
public:
	TextureManager();
	~TextureManager();

	void initialize();
	GLuint getTextureArray() const { return textureArray; }
	bool hasTransparency(TextureType type) const;
	bool hasBiomeColoring(TextureType type) const;

	static bool isTransparent(TextureType type)
	{
		return type == TextureType::GLASS || type == TextureType::OAK_LEAVES ||
			   type == TextureType::WATER;
	}

private:
	GLuint textureArray;
	std::vector<TextureInfo> textures;
	void loadTexture(const std::string &path, TextureType type, bool hasTransparency, bool hasBiomeColoring);
	void loadWaterTexture(const std::string &path, TextureType type, bool hasTransparency, bool hasBiomeColoring);
};