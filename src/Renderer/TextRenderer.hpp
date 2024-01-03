#pragma once

#include <ft2build.h>
#include FT_FREETYPE_H
#include <glad/glad.h>
#include <string>
#include <map>
#include <glm/glm.hpp>
#include <Shader/Shader.hpp>
#include <utils.hpp>

struct Character
{
	unsigned int	textureID;
	glm::ivec2		size;
	glm::ivec2		bearing;
	unsigned int	advance;
};

class TextRenderer
{
	public:
		TextRenderer(const std::string &fontPath, const glm::mat4 &proj);
		~TextRenderer();

		void renderText(std::string text, float x, float y, float scale, glm::vec3 color);

	private:
		FT_Library					ft;
		FT_Face						face;
		Shader						*shader;
		GLuint						VAO;
		GLuint						VBO;
		glm::mat4					projection;
		std::map<char, Character>	characters;

		void loadCharacters();
};
