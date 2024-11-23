#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <map>

#include <Chunk/Chunk.hpp>
#include <Shader/Shader.hpp>
#include <Camera/Camera.hpp>
#include <utils.hpp>

class Renderer {
	public:
		Renderer(int screenWidth, int screenHeight, float renderDistance);
		~Renderer();
		void	setScreenSize(int screenWidth, int screenHeight);

		int		draw(Chunk& chunk, const Shader& shader, const Camera& camera);
		void	drawBoundingBox(const Chunk& chunk, const Camera& camera) const;
		void	drawSkybox(const Camera& camera) const;


	private:
		std::unique_ptr<Shader>	boundingBoxShader;
		GLuint	boundingBoxVAO;
		GLuint	boundingBoxVBO;
		GLuint	boundingBoxEBO;
		void	initBoundingBox();

		GLuint	VAO;
		GLuint	VBO;
		GLuint	textureAtlas;

		std::unique_ptr<Shader>	skyboxShader;
		GLuint	skyboxVAO;
		GLuint	skyboxVBO;
		GLuint	skyboxTexture;

		float	screenWidth;
		float	screenHeight;
		float	renderDistance;
		size_t	currentVBOSize;

		void	loadSkybox();
};
