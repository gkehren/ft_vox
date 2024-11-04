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
	this->textureAtlas = loadTexture((path + "textures/terrain.png").c_str());

	this->loadSkybox();
}

Renderer::~Renderer()
{
	delete this->boundingBoxShader;
	delete this->skyboxShader;
	glDeleteVertexArrays(1, &this->skyboxVAO);
	glDeleteBuffers(1, &this->skyboxVBO);
	glDeleteTextures(1, &this->skyboxTexture);
	glDeleteVertexArrays(1, &this->VAO);
	glDeleteBuffers(1, &this->VBO);
	glDeleteTextures(1, &this->textureAtlas);
	glDeleteVertexArrays(1, &this->boundingBoxVAO);
	glDeleteBuffers(1, &this->boundingBoxVBO);
}

void	Renderer::setScreenSize(int screenWidth, int screenHeight)
{
	this->screenWidth = screenWidth;
	this->screenHeight = screenHeight;
	this->renderDistance = renderDistance;
}

void	Renderer::drawBoundingBox(const Chunk& chunk, const Camera& camera) const
{
	boundingBoxShader->use();

	boundingBoxShader->setMat4("view", camera.getViewMatrix());
	boundingBoxShader->setMat4("projection", camera.getProjectionMatrix(1920, 1080, 320));

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

	const auto& viewMatrix = camera.getViewMatrix();
	const auto& projectionMatrix = camera.getProjectionMatrix(1920, 1080, 320);

	shader.setMat4("view", viewMatrix);
	shader.setMat4("projection", projectionMatrix);
	shader.setInt("textureSampler", 0);
	shader.setVec3("lightPos", camera.getPosition());

	glBindTexture(GL_TEXTURE_2D, this->textureAtlas);

	const auto& data = chunk.getMeshData();
	if (data.empty())
		return (0);

	glBindVertexArray(this->VAO);
	if (data.size() * sizeof(float) != this->currentVBOSize) {
		glBindBuffer(GL_ARRAY_BUFFER, this->VBO);
		glBufferData(GL_ARRAY_BUFFER, data.size() * sizeof(float), data.data(), GL_DYNAMIC_DRAW);
		this->currentVBOSize = data.size() * sizeof(float);
	} else {
		glBindBuffer(GL_ARRAY_BUFFER, this->VBO);
		glBufferSubData(GL_ARRAY_BUFFER, 0, data.size() * sizeof(float), data.data());
	}

	const size_t vertexCount = data.size() / 8;
	glDrawArrays(GL_TRIANGLES, 0, vertexCount);

	glBindVertexArray(0);
	return (vertexCount);
}

void	Renderer::loadSkybox()
{
	std::string path = RES_PATH;
	// Skybox
	this->skyboxShader = new Shader((path + "shaders/skyboxVertex.glsl").c_str(), (path + "shaders/skyboxFragment.glsl").c_str());

	glGenVertexArrays(1, &this->skyboxVAO);
	glGenBuffers(1, &this->skyboxVBO);
	glBindVertexArray(this->skyboxVAO);
	glBindBuffer(GL_ARRAY_BUFFER, this->skyboxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
	glEnableVertexAttribArray(0);
	glBindVertexArray(0);

	glGenTextures(1, &this->skyboxTexture);
	glBindTexture(GL_TEXTURE_CUBE_MAP, this->skyboxTexture);

	for (GLuint i = 0; i < 6; i++) {
		int width, height, nrChannels;
		unsigned char *data = stbi_load((path + skyboxFaces[i]).c_str(), &width, &height, &nrChannels, 0);
		if (data) {
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
			stbi_image_free(data);
		} else {
			std::cout << "Failed to load texture" << std::endl;
		}
	}

	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
}

void	Renderer::drawSkybox(const Camera& camera) const
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
