#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <Voxel/Voxel.hpp>
#include <Shader/Shader.hpp>
#include <Camera/Camera.hpp>

class Renderer {
	public:
		Renderer();
		~Renderer();

		void draw(const Voxel& voxel, const Shader& shader) const;

	private:
		GLuint	VAO;
		GLuint	VBO;
		GLuint	EBO;
};
