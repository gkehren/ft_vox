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

static unsigned int indicesBoundingbox[] = {
		0, 1, 1, 2, 2, 3, 3, 0,
		4, 5, 5, 6, 6, 7, 7, 4,
		0, 4, 1, 5, 2, 6, 3, 7
};

Renderer::Renderer()
{
	glGenVertexArrays(1, &this->VAO);
	glGenVertexArrays(1, &this->boundingBoxVAO);
	glGenBuffers(1, &this->VBO);
	glGenBuffers(1, &this->EBO);
	glGenBuffers(1, &this->instanceVBO);
	glGenBuffers(1, &this->boundingBoxVBO);
	glGenBuffers(1, &this->boundingBoxEBO);

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

	// Bounding box
	glBindVertexArray(this->boundingBoxVAO);
	glBindBuffer(GL_ARRAY_BUFFER, this->boundingBoxVBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->boundingBoxEBO);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);

	glBindVertexArray(0);
}

Renderer::~Renderer()
{
	glDeleteVertexArrays(1, &this->VAO);
	glDeleteVertexArrays(1, &this->boundingBoxVAO);
	glDeleteBuffers(1, &this->VBO);
	glDeleteBuffers(1, &this->EBO);
	glDeleteBuffers(1, &this->instanceVBO);
	glDeleteBuffers(1, &this->boundingBoxVBO);
	glDeleteBuffers(1, &this->boundingBoxEBO);
}

void	Renderer::draw(const std::vector<Chunk>& chunks, const Shader& shader, const Camera& camera) const
{
	std::vector<glm::mat4>	modelMatrices;

	for (const Chunk& chunk : chunks) {
		const std::vector<glm::mat4>& chunkModelMatrices = chunk.getModelMatrices();
		modelMatrices.insert(modelMatrices.end(), chunkModelMatrices.begin(), chunkModelMatrices.end());
	}

	shader.use();

	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 160));

	glBindBuffer(GL_ARRAY_BUFFER, this->instanceVBO);
	glBufferData(GL_ARRAY_BUFFER, modelMatrices.size() * sizeof(glm::mat4), &modelMatrices[0], GL_STATIC_DRAW);

	glBindVertexArray(this->VAO);
	glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0, modelMatrices.size());
	glBindVertexArray(0);
}

void	Renderer::draw(const Chunk& chunk, const Shader& shader, const Camera& camera) const
{
	std::vector<glm::mat4>	modelMatrices = chunk.getModelMatrices();

	shader.use();

	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 160));

	glBindBuffer(GL_ARRAY_BUFFER, this->instanceVBO);
	glBufferData(GL_ARRAY_BUFFER, modelMatrices.size() * sizeof(glm::mat4), &modelMatrices[0], GL_STATIC_DRAW);

	glBindVertexArray(this->VAO);
	glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0, modelMatrices.size());
	glBindVertexArray(0);
}

void	Renderer::drawBoundingBox(const Chunk& chunk, const Shader& shader, const Camera& camera) const
{
	shader.use();

	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 160));

	glm::vec3 position = chunk.getPosition();

	float verticesBoundingbox[] = {
		position.x - 0.5f, position.y - 0.5f, position.z - 0.5f,
		position.x - 0.5f + CHUNK_SIZE, position.y - 0.5f, position.z - 0.5f,
		position.x - 0.5f + CHUNK_SIZE, position.y - 0.5f + CHUNK_SIZE, position.z - 0.5f,
		position.x - 0.5f, position.y - 0.5f + CHUNK_SIZE, position.z - 0.5f,

		position.x - 0.5f, position.y - 0.5f, position.z - 0.5f + CHUNK_SIZE,
		position.x - 0.5f + CHUNK_SIZE, position.y - 0.5f, position.z - 0.5f + CHUNK_SIZE,
		position.x - 0.5f + CHUNK_SIZE, position.y - 0.5f + CHUNK_SIZE, position.z - 0.5f + CHUNK_SIZE,
		position.x - 0.5f, position.y - 0.5f + CHUNK_SIZE, position.z - 0.5f + CHUNK_SIZE
	};

	glBindVertexArray(this->boundingBoxVAO);

	glBindBuffer(GL_ARRAY_BUFFER, this->boundingBoxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verticesBoundingbox), verticesBoundingbox, GL_DYNAMIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->boundingBoxEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indicesBoundingbox), indicesBoundingbox, GL_DYNAMIC_DRAW);

	glDrawElements(GL_LINES, 24, GL_UNSIGNED_INT, 0);
	glBindVertexArray(0);
}
