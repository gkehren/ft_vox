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
		Renderer();
		~Renderer();

		int		draw(Chunk& chunk, const Shader& shader, const Camera& camera);
		void	drawBoundingBox(const Chunk& chunk, const Camera& camera) const;

	private:
		Shader*	boundingBoxShader;
		GLuint	boundingBoxVAO;
		GLuint	boundingBoxVBO;
		GLuint	boundingBoxEBO;

		GLuint	VAO;
		GLuint	VBO;
		GLuint	texture[TEXTURE_COUNT];
};
