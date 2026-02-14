#include "PostProcessing.hpp"
#include <utils.hpp>
#include <iostream>

PostProcessing::PostProcessing(int width, int height)
	: viewportWidth(width), viewportHeight(height),
	  hdrFBO(0), hdrColorTexture(0), hdrDepthRBO(0),
	  godRaysFBO(0), godRaysTexture(0),
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
	godRaysShader = std::make_unique<Shader>(
		(path + "postProcessVertex.glsl").c_str(),
		(path + "godRays.glsl").c_str());
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
	int halfW = std::max(1, viewportWidth / 2);
	int halfH = std::max(1, viewportHeight / 2);

	for (int i = 0; i < 2; ++i)
	{
		glGenFramebuffers(1, &bloomFBO[i]);
		glGenTextures(1, &bloomTexture[i]);

		glBindFramebuffer(GL_FRAMEBUFFER, bloomFBO[i]);
		glBindTexture(GL_TEXTURE_2D, bloomTexture[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, halfW, halfH, 0, GL_RGBA, GL_FLOAT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, bloomTexture[i], 0);

		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			std::cerr << "[PostProcessing] Bloom FBO " << i << " is not complete!" << std::endl;
	}

	// ---- God rays FBO (half resolution, R11F_G11F_B10F) ----
	glGenFramebuffers(1, &godRaysFBO);
	glGenTextures(1, &godRaysTexture);

	glBindFramebuffer(GL_FRAMEBUFFER, godRaysFBO);
	glBindTexture(GL_TEXTURE_2D, godRaysTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R11F_G11F_B10F, halfW, halfH, 0, GL_RGB, GL_FLOAT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, godRaysTexture, 0);

	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		std::cerr << "[PostProcessing] God Rays FBO is not complete!" << std::endl;

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void PostProcessing::deleteFBOs()
{
	glDeleteFramebuffers(1, &hdrFBO);
	glDeleteTextures(1, &hdrColorTexture);
	glDeleteRenderbuffers(1, &hdrDepthRBO);

	glDeleteFramebuffers(2, bloomFBO);
	glDeleteTextures(2, bloomTexture);

	glDeleteFramebuffers(1, &godRaysFBO);
	glDeleteTextures(1, &godRaysTexture);
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
}

void PostProcessing::endSceneAndRender(const PostProcessSettings &settings, const glm::vec2 &sunScreenPos)
{
	int bloomW = viewportWidth / 2;
	int bloomH = viewportHeight / 2;
	if (bloomW < 1) bloomW = 1;
	if (bloomH < 1) bloomH = 1;

	int grW = viewportWidth / 2;
	int grH = viewportHeight / 2;
	if (grW < 1) grW = 1;
	if (grH < 1) grH = 1;

	// Save and disable depth/culling/blending for screen-space passes
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);

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

	// ---------- 3. God Rays ----------
	// Always clear to avoid stale VRAM data when toggling the effect
	glBindFramebuffer(GL_FRAMEBUFFER, godRaysFBO);
	glViewport(0, 0, grW, grH);
	glClear(GL_COLOR_BUFFER_BIT);

	if (settings.godRaysEnabled)
	{
		godRaysShader->use();
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, hdrColorTexture);
		godRaysShader->setInt("hdrBuffer", 0);
		godRaysShader->setVec2("sunScreenPos", sunScreenPos);
		godRaysShader->setFloat("density", settings.godRaysDensity);
		godRaysShader->setFloat("weight", settings.godRaysWeight);
		godRaysShader->setFloat("decay", settings.godRaysDecay);
		godRaysShader->setFloat("godRaysExposure", settings.godRaysExposure);

		renderQuad();
	}

	// ---------- 4. Composite (HDR + Bloom + God Rays -> LDR, tone map, FXAA) ----------
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

	// God rays on unit 2 — only bind when enabled
	if (settings.godRaysEnabled)
	{
		glActiveTexture(GL_TEXTURE2);
		glBindTexture(GL_TEXTURE_2D, godRaysTexture);
		compositeShader->setInt("godRaysBuffer", 2);
	}

	compositeShader->setFloat("exposure", settings.exposure);
	compositeShader->setFloat("bloomIntensity", settings.bloomIntensity);
	compositeShader->setFloat("gamma", settings.gamma);
	compositeShader->setInt("bloomEnabled", settings.bloomEnabled ? 1 : 0);
	compositeShader->setInt("fxaaEnabled", settings.fxaaEnabled ? 1 : 0);
	compositeShader->setInt("toneMapper", settings.toneMapper);
	compositeShader->setInt("godRaysEnabled", settings.godRaysEnabled ? 1 : 0);

	renderQuad();

	// Restore state for subsequent rendering (ImGui, etc.)
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glEnable(GL_BLEND);
}
