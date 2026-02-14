#pragma once

#include <glad/glad.h>
#include <Shader/Shader.hpp>
#include <Engine/EngineDefs.hpp>
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
	void endSceneAndRender(const PostProcessSettings &settings);

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

	// Fullscreen quad geometry
	GLuint quadVAO;
	GLuint quadVBO;

	// Shaders
	std::unique_ptr<Shader> bloomExtractShader;
	std::unique_ptr<Shader> bloomBlurShader;
	std::unique_ptr<Shader> compositeShader;

	void initQuad();
	void initFBOs();
	void deleteFBOs();
	void renderQuad();
};
