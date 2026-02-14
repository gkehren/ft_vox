#include "PostProcessing.hpp"
#include <utils.hpp>
#include <iostream>

PostProcessing::PostProcessing(int width, int height)
	: viewportWidth(width), viewportHeight(height),
	  hdrFBO(0), hdrColorTexture(0), hdrDepthRBO(0),
	  quadVAO(0), quadVBO(0)
{
	bloomFBO[0] = bloomFBO[1] = 0;
	bloomTexture[0] = bloomTexture[1] = 0;

	initQuad();
	initFBOs();

	std::string path = RES_PATH + std::string("shaders/");
	bloomExtractShader = std::make_unique<Shader>(
		(path + "postProcessVertex.glsl").c_str(),
		(path + "bloomExtract.glsl").c_str());
	bloomBlurShader = std::make_unique<Shader>(
		(path + "postProcessVertex.glsl").c_str(),
		(path + "bloomBlur.glsl").c_str());
	compositeShader = std::make_unique<Shader>(
		(path + "postProcessVertex.glsl").c_str(),
		(path + "postProcessFragment.glsl").c_str());
}

PostProcessing::~PostProcessing()
{
	deleteFBOs();
	glDeleteVertexArrays(1, &quadVAO);
	glDeleteBuffers(1, &quadVBO);
}

// ----------------------------------------------------------------------------
// Fullscreen quad (two triangles covering NDC [-1,1])
// ----------------------------------------------------------------------------

void PostProcessing::initQuad()
{
	float quadVertices[] = {
		// pos        // uv
		-1.0f, -1.0f, 0.0f, 0.0f,
		 1.0f, -1.0f, 1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f, 1.0f,

		 1.0f, -1.0f, 1.0f, 0.0f,
		 1.0f,  1.0f, 1.0f, 1.0f,
		-1.0f,  1.0f, 0.0f, 1.0f,
	};

	glGenVertexArrays(1, &quadVAO);
	glGenBuffers(1, &quadVBO);
	glBindVertexArray(quadVAO);
	glBindBuffer(GL_ARRAY_BUFFER, quadVBO);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));
	glEnableVertexAttribArray(1);
	glBindVertexArray(0);
}

// ----------------------------------------------------------------------------
// FBO creation / destruction / resize
// ----------------------------------------------------------------------------

void PostProcessing::initFBOs()
{
	// ---- HDR framebuffer ----
	glGenFramebuffers(1, &hdrFBO);
	glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);

	// Color attachment — RGBA16F for HDR range
	glGenTextures(1, &hdrColorTexture);
	glBindTexture(GL_TEXTURE_2D, hdrColorTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, viewportWidth, viewportHeight, 0, GL_RGBA, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdrColorTexture, 0);

	// Depth + stencil renderbuffer
	glGenRenderbuffers(1, &hdrDepthRBO);
	glBindRenderbuffer(GL_RENDERBUFFER, hdrDepthRBO);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, viewportWidth, viewportHeight);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, hdrDepthRBO);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		std::cerr << "[PostProcessing] HDR FBO is not complete!" << std::endl;

	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	// ---- Bloom ping-pong framebuffers (half resolution) ----
	int bloomW = viewportWidth / 2;
	int bloomH = viewportHeight / 2;
	if (bloomW < 1) bloomW = 1;
	if (bloomH < 1) bloomH = 1;

	for (int i = 0; i < 2; ++i)
	{
		glGenFramebuffers(1, &bloomFBO[i]);
		glGenTextures(1, &bloomTexture[i]);

		glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO[i]);
		glBindTexture(GL_TEXTURE_2D, bloomTexture[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, bloomW, bloomH, 0, GL_RGBA, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomTexture[i], 0);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			std::cerr << "[PostProcessing] Bloom FBO " << i << " is not complete!" << std::endl;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void PostProcessing::deleteFBOs()
{
	glDeleteFramebuffers(1, &hdrFBO);
	glDeleteTextures(1, &hdrColorTexture);
	glDeleteRenderbuffers(1, &hdrDepthRBO);

	glDeleteFramebuffers(2, bloomFBO);
	glDeleteTextures(2, bloomTexture);
}

void PostProcessing::resize(int width, int height)
{
	if (width == viewportWidth && height == viewportHeight)
		return;

	viewportWidth = width;
	viewportHeight = height;

	deleteFBOs();
	initFBOs();
}

// ----------------------------------------------------------------------------
// Rendering helpers
// ----------------------------------------------------------------------------

void PostProcessing::renderQuad()
{
	glBindVertexArray(quadVAO);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glBindVertexArray(0);
}

void PostProcessing::beginScene()
{
	glBindFramebuffer(GL_FRAMEBUFFER, hdrFBO);
	glViewport(0, 0, viewportWidth, viewportHeight);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void PostProcessing::endSceneAndRender(const PostProcessSettings &settings)
{
	int bloomW = viewportWidth / 2;
	int bloomH = viewportHeight / 2;
	if (bloomW < 1) bloomW = 1;
	if (bloomH < 1) bloomH = 1;

	// Save and disable depth/culling for screen-space passes
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);

	// ---------- 1. Bloom Extract ----------
	if (settings.bloomEnabled)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO[0]);
		glViewport(0, 0, bloomW, bloomH);
		glClear(GL_COLOR_BUFFER_BIT);

		bloomExtractShader->use();
		bloomExtractShader->setFloat("bloomThreshold", settings.bloomThreshold);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, hdrColorTexture);
		bloomExtractShader->setInt("hdrBuffer", 0);
		renderQuad();

		// ---------- 2. Bloom Blur (ping-pong) ----------
		bool horizontal = true;
		int blurIterations = 10; // 5 H+V passes
		bloomBlurShader->use();
		bloomBlurShader->setInt("image", 0);

		for (int i = 0; i < blurIterations; ++i)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO[horizontal ? 1 : 0]);
			glClear(GL_COLOR_BUFFER_BIT);
			bloomBlurShader->setInt("horizontal", horizontal ? 1 : 0);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, bloomTexture[horizontal ? 0 : 1]);
			renderQuad();
			horizontal = !horizontal;
		}
	}

	// ---------- 3. Composite (HDR + Bloom -> LDR, tone map, FXAA) ----------
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, viewportWidth, viewportHeight);
	glClear(GL_COLOR_BUFFER_BIT);

	compositeShader->use();

	// HDR scene on unit 0
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, hdrColorTexture);
	compositeShader->setInt("hdrBuffer", 0);

	// Bloom on unit 1 (result sits in bloomTexture[0] after even iterations)
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, bloomTexture[0]);
	compositeShader->setInt("bloomBuffer", 1);

	compositeShader->setFloat("exposure", settings.exposure);
	compositeShader->setFloat("bloomIntensity", settings.bloomIntensity);
	compositeShader->setFloat("gamma", settings.gamma);
	compositeShader->setInt("bloomEnabled", settings.bloomEnabled ? 1 : 0);
	compositeShader->setInt("fxaaEnabled", settings.fxaaEnabled ? 1 : 0);
	compositeShader->setInt("toneMapper", settings.toneMapper);

	renderQuad();

	// Restore state for subsequent rendering (ImGui, etc.)
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
}
