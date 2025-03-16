#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <map>

#include "TextureManager.hpp"
#include <Chunk/Chunk.hpp>
#include <Shader/Shader.hpp>
#include <Camera/Camera.hpp>
#include <utils.hpp>

class Renderer
{
public:
	Renderer();
	~Renderer();

	glm::vec3 computeColorFromPlayerId(uint32_t playerId) const;
	void drawBoundingBox(const Chunk &chunk, const Camera &camera) const;
	void drawSkybox(const Camera &camera) const;
	void drawPlayer(const Camera &camera, const glm::vec3 &position, uint32_t playerId) const;

	GLuint getTextureArray() const { return textureManager.getTextureArray(); }

private:
	TextureManager textureManager;

	std::unique_ptr<Shader> boundingBoxShader;
	GLuint boundingBoxVAO;
	GLuint boundingBoxVBO;
	GLuint boundingBoxEBO;
	void initBoundingBox();

	std::unique_ptr<Shader> skyboxShader;
	GLuint skyboxVAO;
	GLuint skyboxVBO;
	GLuint skyboxTexture;

	std::unique_ptr<Shader> playerShader;
	GLuint playerVAO;
	GLuint playerVBO;
	GLuint playerEBO;
	void initPlayer();

	void loadSkybox();
	glm::vec3 hsvToRgb(float h, float s, float v) const;
};
