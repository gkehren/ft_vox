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

Renderer::Renderer(float screenWidth, float screenHeight, float renderDistance) : screenWidth(screenWidth), screenHeight(screenHeight), renderDistance(renderDistance)
{
	std::string path = RES_PATH;
	this->boundingBoxShader = new Shader((path + "shaders/boundingBoxVertex.glsl").c_str(), (path + "shaders/boundingBoxFragment.glsl").c_str());

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

	glGenVertexArrays(1, &this->VAO);
	glGenBuffers(1, &this->VBO);

	glBindVertexArray(this->VAO);
	glBindBuffer(GL_ARRAY_BUFFER, this->VBO);

	// X, Y, Z, NX, NY, NZ, U, V
	// Position
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);

	// Normal
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(3 * sizeof(float)));
	glEnableVertexAttribArray(1);

	// Texture
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void*)(6 * sizeof(float)));
	glEnableVertexAttribArray(2);

	glBindVertexArray(0);

// Textures
	this->texture[TEXTURE_GRASS] = loadTexture((path + "textures/grass.jpg").c_str());
	this->texture[TEXTURE_DIRT] = loadTexture((path + "textures/dirt.jpg").c_str());
	this->texture[TEXTURE_STONE] = loadTexture((path + "textures/stone.jpg").c_str());
	this->texture[TEXTURE_COBBLESTONE] = loadTexture((path + "textures/cobblestone.jpg").c_str());
}

Renderer::~Renderer()
{
	delete this->boundingBoxShader;
	glDeleteTextures(TEXTURE_COUNT, this->texture);
	glDeleteVertexArrays(1, &this->boundingBoxVAO);
	glDeleteBuffers(1, &this->boundingBoxVBO);
}

void	Renderer::setParameters(float screenWidth, float screenHeight, float renderDistance)
{
	this->screenWidth = screenWidth;
	this->screenHeight = screenHeight;
	this->renderDistance = renderDistance;
}

void	Renderer::drawBoundingBox(const Chunk& chunk, const Camera& camera) const
{
	boundingBoxShader->use();

	boundingBoxShader->setMat4("view", camera.getViewMatrix());
	boundingBoxShader->setMat4("projection", camera.getProjectionMatrix(1920, 1080, 160));

	glm::vec3 position = chunk.getPosition();

	float verticesBoundingbox[] = {
		position.x - 0.5f, position.y - 0.5f, position.z - 0.5f,
		position.x - 0.5f + Chunk::SIZE, position.y - 0.5f, position.z - 0.5f,
		position.x - 0.5f + Chunk::SIZE, position.y - 0.5f + Chunk::HEIGHT, position.z - 0.5f,
		position.x - 0.5f, position.y - 0.5f + Chunk::HEIGHT, position.z - 0.5f,

		position.x - 0.5f, position.y - 0.5f, position.z - 0.5f + Chunk::SIZE,
		position.x - 0.5f + Chunk::SIZE, position.y - 0.5f, position.z - 0.5f + Chunk::SIZE,
		position.x - 0.5f + Chunk::SIZE, position.y - 0.5f + Chunk::HEIGHT, position.z - 0.5f + Chunk::SIZE,
		position.x - 0.5f, position.y - 0.5f + Chunk::HEIGHT, position.z - 0.5f + Chunk::SIZE
	};

	glBindVertexArray(this->boundingBoxVAO);

	glBindBuffer(GL_ARRAY_BUFFER, this->boundingBoxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verticesBoundingbox), verticesBoundingbox, GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->boundingBoxEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indicesBoundingbox), indicesBoundingbox, GL_STATIC_DRAW);

	glDrawElements(GL_LINES, 24, GL_UNSIGNED_INT, 0);
	glBindVertexArray(0);
}

int	Renderer::draw(Chunk& chunk, const Shader& shader, const Camera& camera)
{
	shader.use();

	shader.setMat4("view", camera.getViewMatrix());
	shader.setMat4("projection", camera.getProjectionMatrix(1920, 1080, 160));
	shader.setInt("textureSampler", 0);
	glm::mat4 model = glm::mat4(1.0f);
	model = glm::translate(model, chunk.getPosition());
	shader.setMat4("model", model);

	glBindTexture(GL_TEXTURE_2D, this->texture[TEXTURE_STONE]);

	std::vector<float> data = chunk.getData();
	glBindVertexArray(this->VAO);
	glBindBuffer(GL_ARRAY_BUFFER, this->VBO);
	glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), &data[0], GL_STATIC_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, data.size() / 8);

	glBindVertexArray(0);

	return (data.size() / 8);
}
