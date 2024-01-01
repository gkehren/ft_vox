#include "Renderer.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image/stb_image.h>

static float vertices[] = {
	// Front
	-0.5f, -0.5f,  0.5f, 0.0f, 0.0f,
	 0.5f, -0.5f,  0.5f, 1.0f, 0.0f,
	 0.5f,  0.5f,  0.5f, 1.0f, 1.0f,
	-0.5f,  0.5f,  0.5f, 0.0f, 1.0f,

	// Back
	0.5f, -0.5f, -0.5f, 0.0f, 0.0f,
   -0.5f, -0.5f, -0.5f, 1.0f, 0.0f,
   -0.5f,  0.5f, -0.5f, 1.0f, 1.0f,
	0.5f,  0.5f, -0.5f, 0.0f, 1.0f,

	// Left
   -0.5f, -0.5f, -0.5f, 0.0f, 0.0f,
   -0.5f, -0.5f,  0.5f, 1.0f, 0.0f,
   -0.5f,  0.5f,  0.5f, 1.0f, 1.0f,
   -0.5f,  0.5f, -0.5f, 0.0f, 1.0f,

	// Right
	0.5f, -0.5f,  0.5f, 0.0f, 0.0f,
	0.5f, -0.5f, -0.5f, 1.0f, 0.0f,
	0.5f,  0.5f, -0.5f, 1.0f, 1.0f,
	0.5f,  0.5f,  0.5f, 0.0f, 1.0f,

	// Top
   -0.5f,  0.5f,  0.5f, 0.0f, 0.0f,
	0.5f,  0.5f,  0.5f, 1.0f, 0.0f,
	0.5f,  0.5f, -0.5f, 1.0f, 1.0f,
   -0.5f,  0.5f, -0.5f, 0.0f, 1.0f,

	// Bottom
   -0.5f, -0.5f, -0.5f, 0.0f, 0.0f,
	0.5f, -0.5f, -0.5f, 1.0f, 0.0f,
	0.5f, -0.5f,  0.5f, 1.0f, 1.0f,
   -0.5f, -0.5f,  0.5f, 0.0f, 1.0f,
};

static unsigned int indices[] = {
	// Front
	0, 1, 2, 2, 3, 0,

	// Back
	4, 5, 6, 6, 7, 4,

	// Left
	8, 9, 10, 10, 11, 8,

	// Right
	12, 13, 14, 14, 15, 12,

	// Top
	16, 17, 18, 18, 19, 16,

	// Bottom
	20, 21, 22, 22, 23, 20
};

static unsigned int indicesBoundingbox[] = {
	0, 1, 1, 2, 2, 3, 3, 0,
	4, 5, 5, 6, 6, 7, 7, 4,
	0, 4, 1, 5, 2, 6, 3, 7
};

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
	glDeleteVertexArrays(1, &this->boundingBoxVAO);
	glDeleteBuffers(1, &this->boundingBoxVBO);
	glDeleteBuffers(1, &this->boundingBoxEBO);
}

void	Renderer::draw(const Voxel& voxel, const Shader& shader, const Camera& camera) const
{
	short type = voxel.getType();
	glBindTexture(GL_TEXTURE_2D, this->texture[type]);

	shader.use();

	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 160));
	shader.setMat4("model", voxel.getModelMatrix());
	shader.setInt("textureSampler", 0);

	glBindVertexArray(this->VAO);
	glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
	glBindVertexArray(0);
	glUseProgram(0);
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
