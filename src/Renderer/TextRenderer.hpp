#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H
#include <glad/glad.h>
#include <string>
#include <string_view>
#include <array>
#include <memory>
#include <glm/glm.hpp>
#include <Shader/Shader.hpp>
#include <utils.hpp>

struct Character
{
	unsigned int textureID{0};
	glm::ivec2 size{0, 0};
	glm::ivec2 bearing{0, 0};
	unsigned int advance{0};
};

class TextRenderer
{
public:
	TextRenderer(const std::string &fontPath, const glm::mat4 &proj);
	~TextRenderer();

	void renderText(std::string_view text, float x, float y, float scale, glm::vec3 color);
	void setProjection(const glm::mat4 &proj);

private:
	FT_Library ft;
	FT_Face face;
	std::unique_ptr<Shader> shader;
	GLuint VAO;
	GLuint VBO;
	glm::mat4 projection;
	// ⚡ Bolt: Replaced std::map with std::array for O(1) contiguous memory access in hot paths.
	std::array<Character, 128> characters;

	void loadCharacters();
};
