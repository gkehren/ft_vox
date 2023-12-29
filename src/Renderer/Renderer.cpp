#include "Renderer.hpp"

// Vertices for a cube (a voxel)
float vertices[] = {
    // positions          // colors
    -0.5f, -0.5f, -0.5f,  1.0f, 0.0f, 0.0f, // bottom-left
     0.5f, -0.5f, -0.5f,  0.0f, 1.0f, 0.0f, // bottom-right
     0.5f,  0.5f, -0.5f,  0.0f, 0.0f, 1.0f, // top-right
    -0.5f,  0.5f, -0.5f,  1.0f, 1.0f, 0.0f, // top-left
    -0.5f, -0.5f,  0.5f,  0.0f, 1.0f, 1.0f, // bottom-left
     0.5f, -0.5f,  0.5f,  1.0f, 0.0f, 1.0f, // bottom-right
     0.5f,  0.5f,  0.5f,  1.0f, 1.0f, 1.0f, // top-right
    -0.5f,  0.5f,  0.5f,  0.0f, 0.0f, 0.0f  // top-left
};

// Indices for the triangles of each face of the cube
unsigned int indices[] = {
    0, 1, 2, 2, 3, 0, // front face
    4, 5, 6, 6, 7, 4, // back face
    4, 5, 1, 1, 0, 4, // bottom face
    7, 6, 2, 2, 3, 7, // top face
    4, 0, 3, 3, 7, 4, // left face
    5, 1, 2, 2, 6, 5  // right face
};

Renderer::Renderer() {
	glGenVertexArrays(1, &this->VAO);
	glGenBuffers(1, &this->VBO);
	glGenBuffers(1, &this->EBO);

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

	glBindVertexArray(0);
}

Renderer::~Renderer() {
	glDeleteVertexArrays(1, &this->VAO);
	glDeleteBuffers(1, &this->VBO);
	glDeleteBuffers(1, &this->EBO);
}

void Renderer::draw(const Voxel& voxel, const Shader& shader) const {
	glm::mat4 model = glm::mat4(1.0f);
	model = glm::translate(model, voxel.getPosition());

	shader.setMat4("model", model);

	glBindVertexArray(this->VAO);
	glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
	glBindVertexArray(0);
}
