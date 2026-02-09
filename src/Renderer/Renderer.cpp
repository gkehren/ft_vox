#include "Renderer.hpp"
#include <Chunk/ChunkManager.hpp>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image/stb_image.h>

Renderer::Renderer(int screenWidth, int screenHeight, float renderDistance) : screenWidth(screenWidth), screenHeight(screenHeight), renderDistance(renderDistance)
{
	this->textureManager.initialize();
	this->textureAtlas = this->textureManager.getTextureArray(); // Initialize textureAtlas
	this->initBoundingBox();
	this->initPlayer();
	this->loadSkybox();
	this->initShadowMap();
}

Renderer::~Renderer()
{
	glDeleteVertexArrays(1, &this->skyboxVAO);
	glDeleteBuffers(1, &this->skyboxVBO);
	glDeleteTextures(1, &this->skyboxTexture);
	glDeleteVertexArrays(1, &this->boundingBoxVAO);
	glDeleteBuffers(1, &this->boundingBoxVBO);
	glDeleteBuffers(1, &this->boundingBoxEBO);
	glDeleteVertexArrays(1, &this->playerVAO);
	glDeleteBuffers(1, &this->playerVBO);
	glDeleteBuffers(1, &this->playerEBO);
	glDeleteFramebuffers(1, &this->shadowMapFBO);
	glDeleteTextures(1, &this->shadowMapTexture);
}

void Renderer::initShadowMap()
{
	glGenFramebuffers(1, &shadowMapFBO);
	glGenTextures(1, &shadowMapTexture);
	glBindTexture(GL_TEXTURE_2D, shadowMapTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, SHADOW_WIDTH, SHADOW_HEIGHT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
	float borderColor[] = {1.0, 1.0, 1.0, 1.0};
	glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);

	glBindFramebuffer(GL_FRAMEBUFFER, shadowMapFBO);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shadowMapTexture, 0);
	glDrawBuffer(GL_NONE);
	glReadBuffer(GL_NONE);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	std::string path = RES_PATH;
	shadowShader = std::make_unique<Shader>((path + "shaders/shadowVertex.glsl").c_str(), (path + "shaders/shadowFragment.glsl").c_str());
}

void Renderer::renderShadowMap(const Camera &camera, const glm::vec3 &lightDir, const ChunkManager &chunkManager)
{
	// Save current state
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	GLint cullFace;
	glGetIntegerv(GL_CULL_FACE_MODE, &cullFace);

	glEnable(GL_DEPTH_TEST);
	glViewport(0, 0, SHADOW_WIDTH, SHADOW_HEIGHT);
	glBindFramebuffer(GL_FRAMEBUFFER, shadowMapFBO);
	glClear(GL_DEPTH_BUFFER_BIT);

	shadowShader->use();

	float near_plane = 1.0f, far_plane = 1000.0f;
	float boxSize = 512.0f; // Increased coverage area
	glm::mat4 lightProjection = glm::ortho(-boxSize, boxSize, -boxSize, boxSize, near_plane, far_plane);

	// lightDir points TOWARDS the sun. So lightPos is in that direction.
	glm::vec3 lightPos = camera.getPosition() + glm::normalize(lightDir) * 500.0f;
	glm::mat4 lightView = glm::lookAt(lightPos, camera.getPosition(), glm::vec3(0.0, 1.0, 0.0));

	lightSpaceMatrix = lightProjection * lightView;
	shadowShader->setMat4("lightSpaceMatrix", lightSpaceMatrix);

	glCullFace(GL_FRONT); // Avoid Peter Panning
	chunkManager.drawShadows(*shadowShader);

	// Restore state
	glCullFace(cullFace);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
}


void Renderer::setScreenSize(int screenWidth, int screenHeight)
{
	this->screenWidth = static_cast<float>(screenWidth);
	this->screenHeight = static_cast<float>(screenHeight);
}

void Renderer::initBoundingBox()
{
	std::string path = RES_PATH;
	this->boundingBoxShader = std::make_unique<Shader>((path + "shaders/boundingBoxVertex.glsl").c_str(), (path + "shaders/boundingBoxFragment.glsl").c_str());

	// Unit cube for bounding boxes and highlights
	float unitCubeVertices[] = {
		0.0f, 0.0f, 0.0f, // 0
		1.0f, 0.0f, 0.0f, // 1
		1.0f, 1.0f, 0.0f, // 2
		0.0f, 1.0f, 0.0f, // 3
		0.0f, 0.0f, 1.0f, // 4
		1.0f, 0.0f, 1.0f, // 5
		1.0f, 1.0f, 1.0f, // 6
		0.0f, 1.0f, 1.0f  // 7
	};

	unsigned int unitCubeIndices[] = {
		0, 1, 1, 2, 2, 3, 3, 0, // Bottom
		4, 5, 5, 6, 6, 7, 7, 4, // Top
		0, 4, 1, 5, 2, 6, 3, 7	// Sides
	};

	glGenVertexArrays(1, &this->boundingBoxVAO);
	glGenBuffers(1, &this->boundingBoxVBO);
	glGenBuffers(1, &this->boundingBoxEBO);

	glBindVertexArray(this->boundingBoxVAO);

	glBindBuffer(GL_ARRAY_BUFFER, this->boundingBoxVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(unitCubeVertices), unitCubeVertices, GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->boundingBoxEBO);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unitCubeIndices), unitCubeIndices, GL_STATIC_DRAW);

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

void Renderer::drawBoundingBox(const Chunk &chunk, const Camera &camera) const
{
	boundingBoxShader->use();

	boundingBoxShader->setMat4("view", camera.getViewMatrix());
	boundingBoxShader->setMat4("projection", camera.getProjectionMatrix(screenWidth, screenHeight, renderDistance));

	glm::mat4 model = glm::mat4(1.0f);
	model = glm::translate(model, chunk.getPosition());
	model = glm::scale(model, glm::vec3(CHUNK_SIZE, CHUNK_HEIGHT, CHUNK_SIZE));
	boundingBoxShader->setMat4("model", model);
	boundingBoxShader->setVec3("color", glm::vec3(1.0f, 0.0f, 0.0f)); // Red for chunk borders

	glBindVertexArray(this->boundingBoxVAO);
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
	this->skyboxShader->setMat4("projection", camera.getProjectionMatrix(screenWidth, screenHeight, renderDistance));

	glBindVertexArray(this->skyboxVAO);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_CUBE_MAP, this->skyboxTexture);
	glDrawArrays(GL_TRIANGLES, 0, 36);
	glBindVertexArray(0);
	glActiveTexture(GL_TEXTURE0);
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
	playerShader->setMat4("projection", camera.getProjectionMatrix(screenWidth, screenHeight, renderDistance));

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

void Renderer::drawVoxelHighlight(const glm::vec3 &position, const glm::vec3 &color, const Camera &camera) const
{
	boundingBoxShader->use();

	boundingBoxShader->setMat4("view", camera.getViewMatrix());
	boundingBoxShader->setMat4("projection", camera.getProjectionMatrix(screenWidth, screenHeight, renderDistance));
	
	glm::mat4 model = glm::mat4(1.0f);
	model = glm::translate(model, position);
	boundingBoxShader->setMat4("model", model);
	boundingBoxShader->setVec3("color", color); // Set highlight color

	glBindVertexArray(boundingBoxVAO);

	// Enable line width (if supported)
	GLfloat lineWidth;
	glGetFloatv(GL_LINE_WIDTH, &lineWidth);
	glLineWidth(2.0f); // Make highlight lines thicker

	// Draw the box
	glDrawElements(GL_LINES, 24, GL_UNSIGNED_INT, 0);

	// Restore line width
	glLineWidth(lineWidth);

	glBindVertexArray(0);
}