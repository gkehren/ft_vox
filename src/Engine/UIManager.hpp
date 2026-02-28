#pragma once

#include <imgui/imgui.h>
#include <imgui/ImGuiFileDialog.h>
#include <imgui/imgui_impl_sdl3.h>
#include <imgui/imgui_impl_opengl3.h>
#include <string>
#include <vector>
#include <future>
#include <atomic>

#include <Engine/EngineDefs.hpp>
#include <Camera/Camera.hpp>
#include <Renderer/TextRenderer.hpp>
#include <Shader/Shader.hpp>
#include <Network/Server.hpp>
#include <Network/Client.hpp>
#include <utils.hpp>

// Forward declarations
class Engine;
struct BiomeMapSettings
{
	GLuint textureID{0};
	int mapSize{256};
	float zoom{0.5f};
	glm::vec2 center{0.0f, 0.0f};
	bool needsUpdate{true};
	std::vector<unsigned char> pixelData;
	bool autoFollowPlayer{true};
	double lastUpdateTime{0.0};
	glm::vec2 lastPlayerPos{0.0f, 0.0f};
};

class UIManager
{
public:
	UIManager(Engine *engineInstance, SDL_Window *window, int &windowWidth, int &windowHeight);
	~UIManager();

	void init(SDL_GLContext glContext);
	void processEvent(SDL_Event *event);
	void update();
	void render();

	// UI specific state & methods
	void handleServerControls();
	void handleShaderParametersWindow();
	void renderBiomeMap();
	void updateBiomeMap();

	RenderSettings &getRenderSettings() { return renderSettings; }
	const RenderSettings &getRenderSettings() const { return renderSettings; }
	RenderTiming &getRenderTiming() { return renderTiming; }
	const RenderTiming &getRenderTiming() const { return renderTiming; }
	ShaderParameters &getShaderParams() { return shaderParams; }
	const ShaderParameters &getShaderParams() const { return shaderParams; }
	PostProcessSettings &getPostProcessSettings() { return postProcessSettings; }
	const PostProcessSettings &getPostProcessSettings() const { return postProcessSettings; }

	TextureType &getSelectedTexture() { return selectedTexture; }
	const TextureType &getSelectedTexture() const { return selectedTexture; }

	const BiomeMapSettings &getBiomeMapSettings() const { return biomeMap; }

private:
	Engine *engine; // Pointer to access Engine members like camera, client, etc.
	SDL_Window *sdlWindow;
	int &winWidth;
	int &winHeight;

	// UI-related state from Engine
	RenderSettings renderSettings;
	RenderTiming renderTiming;
	ShaderParameters shaderParams;
	PostProcessSettings postProcessSettings;
	TextureType selectedTexture; // Assuming TextureType is an enum or similar

	// Network related UI
	char ipInputBuffer[128] = "127.0.0.1";

	BiomeMapSettings biomeMap;

	// Biome map background generation
	std::future<void> m_biomeMapFuture;
	std::vector<unsigned char> m_biomeMapPending; // written by background thread
	std::atomic<bool> m_biomeMapReady{false};
	glm::vec2 m_pendingCenter{0.0f, 0.0f}; // center used for the in-flight texture
	float m_pendingZoom{1.0f};			   // zoom used for the in-flight texture
	int m_pendingGridStartX{0};			   // grid-space origin X used by GenUniformGrid2D
	int m_pendingGridStartZ{0};			   // grid-space origin Z used by GenUniformGrid2D
};
