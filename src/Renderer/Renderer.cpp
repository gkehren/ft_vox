#include "Renderer.hpp"

// Vertices for a cube (a voxel)
static float vertices[] = {
	// positions          // colors
	-0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 0.0f,
	 0.5f, -0.5f,  0.5f,  0.0f, 1.0f, 0.0f,
	 0.5f,  0.5f,  0.5f,  0.0f, 0.0f, 1.0f,
	-0.5f,  0.5f,  0.5f,  1.0f, 1.0f, 0.0f,
	-0.5f, -0.5f, -0.5f,  0.0f, 1.0f, 1.0f,
	 0.5f, -0.5f, -0.5f,  1.0f, 0.0f, 1.0f,
	 0.5f,  0.5f, -0.5f,  1.0f, 1.0f, 1.0f,
	-0.5f,  0.5f, -0.5f,  0.0f, 0.0f, 0.0f
};

// Indices for the triangles of each face of the cube (a voxel) in counter-clockwise order
static unsigned int indices[] = {
	0, 1, 2, 2, 3, 0,
	1, 5, 6, 6, 2, 1,
	7, 6, 5, 5, 4, 7,
	4, 0, 3, 3, 7, 4,
	4, 5, 1, 1, 0, 4,
	3, 2, 6, 6, 7, 3
};

Renderer::Renderer() {
	glGenVertexArrays(1, &this->VAO);
	glGenBuffers(1, &this->VBO);
	glGenBuffers(1, &this->EBO);
	glGenBuffers(1, &this->instanceVBO);

	glBindVertexArray(this->VAO);

	glBindBuffer(GL_ARRAY_BUFFER, this->VBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->EBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	// Position attribute
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);

	// Color attribute
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
	glEnableVertexAttribArray(1);

	glBindBuffer(GL_ARRAY_BUFFER, this->instanceVBO);
	for (unsigned int i = 0; i < 4; i++) {
		glEnableVertexAttribArray(2 + i);
		glVertexAttribPointer(2 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(i * sizeof(glm::vec4)));
		glVertexAttribDivisor(2 + i, 1);
	}

	glBindVertexArray(0);
}

Renderer::~Renderer() {
	glDeleteVertexArrays(1, &this->VAO);
	glDeleteBuffers(1, &this->VBO);
	glDeleteBuffers(1, &this->EBO);
	glDeleteBuffers(1, &this->instanceVBO);
}

void Renderer::draw(const std::vector<glm::mat4>& modelMatrices, const Shader& shader) const {
	glBindBuffer(GL_ARRAY_BUFFER, this->instanceVBO);
	glBufferData(GL_ARRAY_BUFFER, modelMatrices.size() * sizeof(glm::mat4), &modelMatrices[0], GL_STATIC_DRAW);

	glBindVertexArray(this->VAO);
	glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0, modelMatrices.size());
	glBindVertexArray(0);
}
