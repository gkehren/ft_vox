#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <Chunk/Chunk.hpp>
#include <Shader/Shader.hpp>
#include <Camera/Camera.hpp>

#include <iostream>

class Renderer {
	public:
		Renderer();
		~Renderer();

		void	draw(const std::vector<Chunk>& chunks, const Shader& shader, const Camera& camera) const;
		void	draw(const Chunk& chunk, const Shader& shader, const Camera& camera) const;
		void	drawBoundingBox(const Chunk& chunk, const Shader& shader, const Camera& camera) const;

	private:
		GLuint	VAO;
		GLuint	VBO;
		GLuint	EBO;
		GLuint	instanceVBO;

		GLuint	boundingBoxVAO;
		GLuint	boundingBoxVBO;
		GLuint	boundingBoxEBO;
};
