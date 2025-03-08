#include "Renderer.hpp"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image/stb_image.h>

static GLuint loadTexture(const char *path)
{
	GLuint textureID;
	glGenTextures(1, &textureID);

	int width, height, nrComponents;
	unsigned char *data = stbi_load(path, &width, &height, &nrComponents, 0);
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
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
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

Renderer::Renderer(int screenWidth, int screenHeight, float renderDistance) : screenWidth(screenWidth), screenHeight(screenHeight), renderDistance(renderDistance)
{
	std::string path = RES_PATH;

	this->initBoundingBox();
	this->initPlayer();

	// Textures
	this->textureAtlas = loadTexture((path + "textures/terrain.png").c_str());

	this->loadSkybox();
}

Renderer::~Renderer()
{
	glDeleteVertexArrays(1, &this->skyboxVAO);
	glDeleteBuffers(1, &this->skyboxVBO);
	glDeleteTextures(1, &this->skyboxTexture);
	glDeleteTextures(1, &this->textureAtlas);
	glDeleteVertexArrays(1, &this->boundingBoxVAO);
	glDeleteBuffers(1, &this->boundingBoxVBO);
	glDeleteBuffers(1, &this->boundingBoxEBO);
	glDeleteVertexArrays(1, &this->playerVAO);
	glDeleteBuffers(1, &this->playerVBO);
	glDeleteBuffers(1, &this->playerEBO);
}

void Renderer::initBoundingBox()
{
	std::string path = RES_PATH;
	this->boundingBoxShader = std::make_unique<Shader>((path + "shaders/boundingBoxVertex.glsl").c_str(), (path + "shaders/boundingBoxFragment.glsl").c_str());

	// Bounding box
	glGenVertexArrays(1, &this->boundingBoxVAO);
	glGenBuffers(1, &this->boundingBoxVBO);
	glGenBuffers(1, &this->boundingBoxEBO);
	glBindVertexArray(this->boundingBoxVAO);
	glBindBuffer(GL_ARRAY_BUFFER, this->boundingBoxVBO);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->boundingBoxEBO);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(0);

	glBindVertexArray(0);
}

void Renderer::initPlayer()
{
	std::string path = RES_PATH;
	this->playerShader = std::make_unique<Shader>((path + "shaders/playerVertex.glsl").c_str(), (path + "shaders/playerFragment.glsl").c_str());

	float playerVertices[] = {
		// Positions        // Couleurs
		-0.5f, -0.5f, -0.5f, 1.0f, 0.0f, 0.0f,
		0.5f, -0.5f, -0.5f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, -0.5f, 0.0f, 0.0f, 1.0f,
		-0.5f, 0.5f, -0.5f, 1.0f, 1.0f, 0.0f,
		-0.5f, -0.5f, 0.5f, 1.0f, 0.0f, 1.0f,
		0.5f, -0.5f, 0.5f, 0.0f, 1.0f, 1.0f,
		0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f,
		-0.5f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f};

	unsigned int playerIndices[] = {
		0, 3, 1, 1, 3, 2, // Front face
		4, 5, 7, 5, 6, 7, // Back face
		0, 1, 5, 0, 5, 4, // Bottom face
		3, 7, 2, 2, 7, 6, // Top face
		0, 7, 3, 0, 4, 7, // Left face
		1, 2, 6, 1, 6, 5  // Right face
	};

	// Player
	glGenVertexArrays(1, &this->playerVAO);
	glGenBuffers(1, &this->playerVBO);
	glGenBuffers(1, &this->playerEBO);

	glBindVertexArray(this->playerVAO);
	glBindBuffer(GL_ARRAY_BUFFER, this->playerVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(playerVertices), playerVertices, GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->playerEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(playerIndices), playerIndices, GL_STATIC_DRAW);

	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(3 * sizeof(float)));
	glEnableVertexAttribArray(1);

	glBindVertexArray(0);
}

void Renderer::setScreenSize(int screenWidth, int screenHeight)
{
	this->screenWidth = screenWidth;
	this->screenHeight = screenHeight;
}

void Renderer::drawBoundingBox(const Chunk &chunk, const Camera &camera) const
{
	boundingBoxShader->use();

	boundingBoxShader->setMat4("view", camera.getViewMatrix());
	boundingBoxShader->setMat4("projection", camera.getProjectionMatrix(1920, 1080, 320));

	glm::vec3 position = chunk.getPosition();

	float verticesBoundingbox[] = {
		position.x - 1.0f, position.y - 1.0f, position.z - 1.0f,
		position.x - 1.0f + Chunk::SIZE, position.y - 1.0f, position.z - 1.0f,
		position.x - 1.0f + Chunk::SIZE, position.y - 1.0f + Chunk::HEIGHT, position.z - 1.0f,
		position.x - 1.0f, position.y - 1.0f + Chunk::HEIGHT, position.z - 1.0f,

		position.x - 1.0f, position.y - 1.0f, position.z - 1.0f + Chunk::SIZE,
		position.x - 1.0f + Chunk::SIZE, position.y - 1.0f, position.z - 1.0f + Chunk::SIZE,
		position.x - 1.0f + Chunk::SIZE, position.y - 1.0f + Chunk::HEIGHT, position.z - 1.0f + Chunk::SIZE,
		position.x - 1.0f, position.y - 1.0f + Chunk::HEIGHT, position.z - 1.0f + Chunk::SIZE};

	glBindVertexArray(this->boundingBoxVAO);

	glBindBuffer(GL_ARRAY_BUFFER, this->boundingBoxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verticesBoundingbox), verticesBoundingbox, GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->boundingBoxEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indicesBoundingbox), indicesBoundingbox, GL_STATIC_DRAW);

	glDrawElements(GL_LINES, 24, GL_UNSIGNED_INT, 0);
	glBindVertexArray(0);
}

void Renderer::loadSkybox()
{
	std::string path = RES_PATH;
	this->skyboxShader = std::make_unique<Shader>((path + "shaders/skyboxVertex.glsl").c_str(), (path + "shaders/skyboxFragment.glsl").c_str());

	glGenVertexArrays(1, &this->skyboxVAO);
	glGenBuffers(1, &this->skyboxVBO);
	glBindVertexArray(this->skyboxVAO);
	glBindBuffer(GL_ARRAY_BUFFER, this->skyboxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(0);
	glBindVertexArray(0);

	glGenTextures(1, &this->skyboxTexture);
	glBindTexture(GL_TEXTURE_CUBE_MAP, this->skyboxTexture);

	for (GLuint i = 0; i < 6; i++)
	{
		int width, height, nrChannels;
		unsigned char *data = stbi_load((path + skyboxFaces[i]).c_str(), &width, &height, &nrChannels, 0);
		if (data)
		{
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
			stbi_image_free(data);
		}
		else
		{
			std::cout << "Failed to load texture" << std::endl;
		}
	}

	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
}

void Renderer::drawSkybox(const Camera &camera) const
{
	glDepthFunc(GL_LEQUAL);
	this->skyboxShader->use();
	this->skyboxShader->setInt("skybox", 1);
	this->skyboxShader->setMat4("view", glm::mat4(glm::mat3(camera.getViewMatrix())));
	this->skyboxShader->setMat4("projection", camera.getProjectionMatrix(1920, 1080, 320));

	glBindVertexArray(this->skyboxVAO);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_CUBE_MAP, this->skyboxTexture);
	glDrawArrays(GL_TRIANGLES, 0, 36);
	glBindVertexArray(0);
	glDepthFunc(GL_LESS);
}

void Renderer::drawPlayer(const Camera &camera, const glm::vec3 &position, uint32_t playerId) const
{
	playerShader->use();

	glm::mat4 model = glm::mat4(1.0f);
	model = glm::translate(model, position);
	// model = glm::scale(model, glm::vec3(0.1f));
	playerShader->setMat4("model", model);
	playerShader->setMat4("view", camera.getViewMatrix());
	playerShader->setMat4("projection", camera.getProjectionMatrix(1920, 1080, 320));

	glm::vec3 color = computeColorFromPlayerId(playerId);
	playerShader->setVec3("playerColor", color);

	glBindVertexArray(this->playerVAO);
	glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_INT, 0);
	glBindVertexArray(0);
}

glm::vec3 Renderer::computeColorFromPlayerId(uint32_t playerId) const
{
	// Utilisez le playerId pour générer une couleur unique
	float hue = std::fmod(static_cast<float>(playerId) * 0.61803398875f, 1.0f); // Conjugaison du nombre d'or
	return hsvToRgb(hue, 0.5f, 0.95f);
}

glm::vec3 Renderer::hsvToRgb(float h, float s, float v) const
{
	float r, g, b;

	int i = int(h * 6.0f);
	float f = h * 6.0f - i;
	float p = v * (1.0f - s);
	float q = v * (1.0f - f * s);
	float t = v * (1.0f - (1.0f - f) * s);

	switch (i % 6)
	{
	case 0:
		r = v;
		g = t;
		b = p;
		break;
	case 1:
		r = q;
		g = v;
		b = p;
		break;
	case 2:
		r = p;
		g = v;
		b = t;
		break;
	case 3:
		r = p;
		g = q;
		b = v;
		break;
	case 4:
		r = t;
		g = p;
		b = v;
		break;
	case 5:
		r = v;
		g = p;
		b = q;
		break;
	}

	return glm::vec3(r, g, b);
}