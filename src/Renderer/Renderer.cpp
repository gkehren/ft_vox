#include "Renderer.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image/stb_image.h>

static GLuint loadTexture(const char* path)
{
	GLuint textureID;
	glGenTextures(1, &textureID);

	int width, height, nrComponents;
	unsigned char* data = stbi_load(path, &width, &height, &nrComponents, 0);
	if (data)
	{
		GLenum format;
		if (nrComponents == 1)
			format = GL_RED;
		else if (nrComponents == 3)
			format = GL_RGB;
		else if (nrComponents == 4)
			format = GL_RGBA;

		glBindTexture(GL_TEXTURE_2D, textureID);
		glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
		glGenerateMipmap(GL_TEXTURE_2D);

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, 0.4f);

		stbi_image_free(data);
	}
	else
	{
		std::cout << "Texture failed to load at path: " << path << std::endl;
		stbi_image_free(data);
	}

	return textureID;
}

Renderer::Renderer()
{
	// Instancing
	glGenVertexArrays(1, &this->VAO);
	glBindVertexArray(this->VAO);

	glGenBuffers(1, &this->VBO);
	glBindBuffer(GL_ARRAY_BUFFER, this->VBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
	glEnableVertexAttribArray(1);

	glGenBuffers(1, &this->EBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->EBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

	glGenBuffers(1, &this->instanceVBO);
	glBindBuffer(GL_ARRAY_BUFFER, this->instanceVBO);
	for (unsigned int i = 0; i < 4; i++) {
		glEnableVertexAttribArray(2 + i);
		glVertexAttribPointer(2 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(sizeof(glm::vec4) * i));
		glVertexAttribDivisor(2 + i, 1);
	}

	glBindVertexArray(0);

	// Bounding box
	glGenVertexArrays(1, &this->boundingBoxVAO);
	glGenBuffers(1, &this->boundingBoxVBO);
	glGenBuffers(1, &this->boundingBoxEBO);
	glBindVertexArray(this->boundingBoxVAO);
	glBindBuffer(GL_ARRAY_BUFFER, this->boundingBoxVBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->boundingBoxEBO);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);

	glBindVertexArray(0);

	// Textures
	std::string path = BASE_PATH;
	this->texture[TEXTURE_GRASS] = loadTexture((path + "textures/grass.jpg").c_str());
	this->texture[TEXTURE_DIRT] = loadTexture((path + "textures/dirt.jpg").c_str());
	this->texture[TEXTURE_STONE] = loadTexture((path + "textures/stone.jpg").c_str());
	this->texture[TEXTURE_COBBLESTONE] = loadTexture((path + "textures/cobblestone.jpg").c_str());
}

Renderer::~Renderer()
{
	glDeleteTextures(TEXTURE_COUNT, this->texture);
	glDeleteVertexArrays(1, &this->VAO);
	glDeleteBuffers(1, &this->VBO);
	glDeleteBuffers(1, &this->EBO);
	glDeleteBuffers(1, &this->instanceVBO);
	glDeleteVertexArrays(1, &this->boundingBoxVAO);
	glDeleteBuffers(1, &this->boundingBoxVBO);
	glDeleteBuffers(1, &this->boundingBoxEBO);
}

void	Renderer::draw(const Chunk& chunk, const Shader& shader, const Camera& camera) const
{
	shader.use();

	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 160));
	shader.setInt("textureSampler", 0);

	glBindTexture(GL_TEXTURE_2D, this->texture[TEXTURE_COBBLESTONE]);
	glBindBuffer(GL_ARRAY_BUFFER, this->instanceVBO);
	glBufferData(GL_ARRAY_BUFFER, chunk.getVoxels().size() * sizeof(glm::mat4), NULL, GL_STATIC_DRAW);

	glm::mat4* modelMatrices = (glm::mat4*)glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
	for (unsigned int i = 0; i < chunk.getVoxels().size(); i++) {
		glm::mat4 model = glm::mat4(1.0f);
		model = glm::translate(model, chunk.getVoxels()[i].getPosition());
		modelMatrices[i] = model;
	}
	glUnmapBuffer(GL_ARRAY_BUFFER);

	glBindVertexArray(this->VAO);
	glDrawElementsInstanced(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0, chunk.getVoxels().size());
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
		position.x - 0.5f + Chunk::WIDTH, position.y - 0.5f, position.z - 0.5f,
		position.x - 0.5f + Chunk::WIDTH, position.y - 0.5f + Chunk::HEIGHT, position.z - 0.5f,
		position.x - 0.5f, position.y - 0.5f + Chunk::HEIGHT, position.z - 0.5f,

		position.x - 0.5f, position.y - 0.5f, position.z - 0.5f + Chunk::DEPTH,
		position.x - 0.5f + Chunk::WIDTH, position.y - 0.5f, position.z - 0.5f + Chunk::DEPTH,
		position.x - 0.5f + Chunk::WIDTH, position.y - 0.5f + Chunk::HEIGHT, position.z - 0.5f + Chunk::DEPTH,
		position.x - 0.5f, position.y - 0.5f + Chunk::HEIGHT, position.z - 0.5f + Chunk::DEPTH
	};

	glBindVertexArray(this->boundingBoxVAO);

	glBindBuffer(GL_ARRAY_BUFFER, this->boundingBoxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verticesBoundingbox), verticesBoundingbox, GL_DYNAMIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->boundingBoxEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indicesBoundingbox), indicesBoundingbox, GL_DYNAMIC_DRAW);

	glDrawElements(GL_LINES, 24, GL_UNSIGNED_INT, 0);
	glBindVertexArray(0);
}

void	Renderer::drawBoundingBox(const Voxel& voxel, const Shader& shader, const Camera& camera) const
{
	shader.use();

	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 160));

	glm::vec3 position = voxel.getPosition();

	float voxelSize = voxel.getSize();

	float verticesBoundingbox[] = {
		position.x - 0.5f * voxelSize, position.y - 0.5f * voxelSize, position.z - 0.5f * voxelSize,
		position.x + 0.5f * voxelSize, position.y - 0.5f * voxelSize, position.z - 0.5f * voxelSize,
		position.x + 0.5f * voxelSize, position.y + 0.5f * voxelSize, position.z - 0.5f * voxelSize,
		position.x - 0.5f * voxelSize, position.y + 0.5f * voxelSize, position.z - 0.5f * voxelSize,

		position.x - 0.5f * voxelSize, position.y - 0.5f * voxelSize, position.z + 0.5f * voxelSize,
		position.x + 0.5f * voxelSize, position.y - 0.5f * voxelSize, position.z + 0.5f * voxelSize,
		position.x + 0.5f * voxelSize, position.y + 0.5f * voxelSize, position.z + 0.5f * voxelSize,
		position.x - 0.5f * voxelSize, position.y + 0.5f * voxelSize, position.z + 0.5f * voxelSize
	};

	glBindVertexArray(this->boundingBoxVAO);

	glBindBuffer(GL_ARRAY_BUFFER, this->boundingBoxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verticesBoundingbox), verticesBoundingbox, GL_DYNAMIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->boundingBoxEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indicesBoundingbox), indicesBoundingbox, GL_DYNAMIC_DRAW);

	glDrawElements(GL_LINES, 24, GL_UNSIGNED_INT, 0);
	glBindVertexArray(0);
}
