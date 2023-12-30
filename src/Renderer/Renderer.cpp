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

Renderer::Renderer()
{
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

Renderer::~Renderer()
{
	glDeleteVertexArrays(1, &this->VAO);
	glDeleteBuffers(1, &this->VBO);
	glDeleteBuffers(1, &this->EBO);
	glDeleteBuffers(1, &this->instanceVBO);
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
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
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
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glUseProgram(0);
}

void	Renderer::drawBoundingBox(const Chunk& chunk, const Shader& shader, const Camera& camera) const
{
	glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
	glDepthMask(GL_FALSE);
	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glUseProgram(0);
	shader.use();

	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 160));

	glm::vec3 position = chunk.getPosition();

	// Les 8 sommets du cube
	float verticesBoundingbox[] = {
		position.x, position.y, position.z,
		position.x + CHUNK_SIZE, position.y, position.z,
		position.x + CHUNK_SIZE, position.y + CHUNK_SIZE, position.z,
		position.x, position.y + CHUNK_SIZE, position.z,

		position.x, position.y, position.z + CHUNK_SIZE,
		position.x + CHUNK_SIZE, position.y, position.z + CHUNK_SIZE,
		position.x + CHUNK_SIZE, position.y + CHUNK_SIZE, position.z + CHUNK_SIZE,
		position.x, position.y + CHUNK_SIZE, position.z + CHUNK_SIZE
	};

	for (int i = 0; i < 24; i++) {
		verticesBoundingbox[i] -= 0.5f;
	}

	// Les 12 arêtes du cube, chaque arête est un segment de ligne entre deux sommets
	unsigned int indicesBoundingbox[] = {
		0, 1,
		1, 2,
		2, 3,
		3, 0,

		4, 5,
		5, 6,
		6, 7,
		7, 4,

		0, 4,
		1, 5,
		2, 6,
		3, 7
	};

	GLuint vaoBoundingbox, vboBoundingbox, eboBoundingbox;
	glGenVertexArrays(1, &vaoBoundingbox);
	glGenBuffers(1, &vboBoundingbox);
	glGenBuffers(1, &eboBoundingbox);

	glBindVertexArray(vaoBoundingbox);

	glBindBuffer(GL_ARRAY_BUFFER, vboBoundingbox);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verticesBoundingbox), verticesBoundingbox, GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, eboBoundingbox);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indicesBoundingbox), indicesBoundingbox, GL_STATIC_DRAW);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);

	glDrawElements(GL_LINES, 24, GL_UNSIGNED_INT, 0);

	glDeleteVertexArrays(1, &vaoBoundingbox);
	glDeleteBuffers(1, &vboBoundingbox);
	glDeleteBuffers(1, &eboBoundingbox);

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glUseProgram(0);
}
