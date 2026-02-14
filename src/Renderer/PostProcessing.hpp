#pragma once

#include <Engine/EngineDefs.hpp>
#include <glm/glm.hpp>
#include <glad/glad.h>
#include <Shader/Shader.hpp>
#include <memory>

class PostProcessing
{
public:
	PostProcessing(int width, int height);
	~PostProcessing();

	// Resizes all FBO attachments to match the new viewport dimensions
	void resize(int width, int height);

	// Bind the HDR framebuffer — call before rendering the scene
	void beginScene();

	// Run all post-processing passes and present to the default framebuffer
	// sunScreenPos: sun position in normalized screen coords [0,1], used for god rays
	void endSceneAndRender(const PostProcessSettings &settings, const glm::vec2 &sunScreenPos);

private:
	int viewportWidth;
	int viewportHeight;

	// HDR scene framebuffer
	GLuint hdrFBO;
	GLuint hdrColorTexture;
	GLuint hdrDepthRBO;

	// Bloom: two half-resolution ping-pong FBOs
	GLuint bloomFBO[2];
	GLuint bloomTexture[2];

	// God rays: half-resolution FBO
	GLuint godRaysFBO;
	GLuint godRaysTexture;

	// Fullscreen quad geometry
	GLuint quadVAO;
	GLuint quadVBO;

	// Shaders
	std::unique_ptr<Shader> bloomExtractShader;
	std::unique_ptr<Shader> bloomBlurShader;
	std::unique_ptr<Shader> godRaysShader;
	std::unique_ptr<Shader> compositeShader;

	void initQuad();
	void initFBOs();
	void deleteFBOs();
	void renderQuad();
};
