#include "Engine.hpp"

#define FULLSCREEN 1 // 0 = fullscreen, 1 = windowed, 2 = borderless

Engine::Engine() : deltaTime(0.0f), fps(0.0f), lastFrame(0.0f), frameCount(0.0f), lastTime(0.0f), seed(0)
{
	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		throw std::runtime_error("Failed to initialize SDL");
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif

	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

	this->windowWidth = 1920;
	this->windowHeight = 1080;

	Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;
	if (FULLSCREEN == 0)
	{
		const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(0);
		if (mode)
		{
			this->windowWidth = mode->w;
			this->windowHeight = mode->h;
			windowFlags |= SDL_WINDOW_FULLSCREEN;
		}
	}
	else if (FULLSCREEN == 2)
	{
		windowFlags |= SDL_WINDOW_BORDERLESS;
	}

	this->window = SDL_CreateWindow("ft_vox", this->windowWidth, this->windowHeight, windowFlags);
	if (!this->window)
	{
		throw std::runtime_error("Failed to create SDL window");
	}
	SDL_SetWindowPosition(this->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

	glContext = SDL_GL_CreateContext(this->window); // Store glContext
	if (!glContext)
	{
		SDL_DestroyWindow(this->window);
		SDL_Quit();
		throw std::runtime_error("Failed to create OpenGL context");
	}

	SDL_GL_MakeCurrent(this->window, glContext);
	SDL_GL_SetSwapInterval(0); // vsync
	SDL_ShowWindow(window);

	if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress)))
	{
		SDL_GL_DestroyContext(glContext);
		SDL_DestroyWindow(this->window);
		SDL_Quit();
		throw std::runtime_error("Failed to initialize GLAD");
	}

	// Initialize UIManager
	uiManager = std::make_unique<UIManager>(this, window, windowWidth, windowHeight);
	uiManager->init(glContext); // Pass GL context to UIManager for ImGui init

	std::string path = RES_PATH + std::string("shaders/");
	this->shader = std::make_unique<Shader>((path + "vertex.glsl").c_str(), (path + "fragment.glsl").c_str());
	// Access render settings from UIManager for Renderer initialization
	this->renderer = std::make_unique<Renderer>(windowWidth, windowHeight, uiManager->getRenderSettings().maxRenderDistance);
	this->camera.setWindow(this->window);
	this->playerChunkPos = glm::ivec2(-1, -1);

	glActiveTexture(GL_TEXTURE0);
	this->shader->setInt("textureSampler", 0);

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	this->textRenderer = std::make_unique<TextRenderer>(RES_PATH + std::string("fonts/FiraCode.ttf"), glm::ortho(0.0f, static_cast<float>(windowWidth), 0.0f, static_cast<float>(windowHeight)));

	this->selectedTexture = OAK_LEAVES;				   // Default selected texture
	uiManager->getSelectedTexture() = selectedTexture; // Sync with UIManager

	uint32_t threadCount = std::thread::hardware_concurrency() / 2;
	this->threadPool = std::make_unique<ThreadPool>(threadCount > 0 ? threadCount : 1);

	initializeNoiseGenerator(0);

	this->chunkManager = std::make_unique<ChunkManager>(terrainGenerator.get(), threadPool.get(), uiManager->getRenderTiming());

	std::cout << "SDL version: " << SDL_GetVersion() << std::endl;
	std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;
	std::cout << "GLSL version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << std::endl;
	std::cout << "Vendor: " << glGetString(GL_VENDOR) << std::endl;
	std::cout << "Renderer: " << glGetString(GL_RENDERER) << std::endl;
	std::cout << "ImGui version: " << IMGUI_VERSION << std::endl;
	std::cout << "Threads: " << (threadCount > 0 ? threadCount : 1) << std::endl;
}

Engine::~Engine()
{
	SDL_GL_DestroyContext(glContext);
	SDL_DestroyWindow(this->window);
	SDL_Quit();
}

void Engine::initializeNoiseGenerator(int seed_val)
{
	// Set the seed - generate random if not provided
	if (seed_val <= 0)
	{
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(100000, 999999);
		this->seed = dis(gen);
	}
	else
	{
		this->seed = seed_val;
	}
	this->terrainGenerator = std::make_unique<TerrainGenerator>(this->seed);

	std::cout << "Terrain generation initialized with seed: " << this->seed << std::endl;
}

void Engine::run()
{
	bool keyTPressed = false;
	this->running = true;
	this->isMousecaptured = true;

	while (this->running)
	{
		double currentFrame = SDL_GetTicks() / 1000.0;
		this->deltaTime = currentFrame - this->lastFrame;
		this->lastFrame = currentFrame;

		// FPS calculation
		frameCount++;
		if (currentFrame - lastTime >= 1.0)
		{
			fps = frameCount;
			frameCount = 0;
			lastTime = currentFrame;
		}

		handleEvents(keyTPressed);

		const bool *keys = SDL_GetKeyboardState(NULL);
		if (isMousecaptured && !ImGui::GetIO().WantCaptureKeyboard) // Check ImGui focus
		{
			this->camera.processKeyboard(deltaTime, keys);
		}

		// Update game state (including chunk management)
		updateWorldState();

		// Start ImGui frame
		uiManager->update(); // This calls ImGui::NewFrame and prepares UI data

		// Rendering
		glClearColor(uiManager->getShaderParams().fogColor.x, uiManager->getShaderParams().fogColor.y, uiManager->getShaderParams().fogColor.z, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		renderScene(); // Render the 3D game world

		if (renderer) // Ensure renderer is valid
		{
			renderer->drawSkybox(this->camera); // Draw skybox after 3D scene
		}

		uiManager->render(); // Render ImGui UI on top

		SDL_GL_SwapWindow(this->window);
	}
}

void Engine::updateWorldState()
{
	RenderSettings &currentRenderSettings = uiManager->getRenderSettings();

	if (!currentRenderSettings.paused)
	{
		const glm::ivec2 newPlayerChunkPos{
			static_cast<int>(std::floor(camera.getPosition().x / CHUNK_SIZE)),
			static_cast<int>(std::floor(camera.getPosition().z / CHUNK_SIZE))};

		if (newPlayerChunkPos != playerChunkPos)
		{
			playerChunkPos = newPlayerChunkPos;
			// Pass currentRenderSettings by const reference
			chunkManager->updatePlayerPosition(playerChunkPos, camera, currentRenderSettings);
		}
		// Pass currentRenderSettings by const reference
		chunkManager->performFrustumCulling(camera, windowWidth, windowHeight, currentRenderSettings);
	}
	// Pass currentRenderSettings by const reference
	chunkManager->processChunkLoading(currentRenderSettings);
	chunkManager->processFinishedJobs();
	chunkManager->generatePendingVoxels(camera, currentRenderSettings, seed);
	chunkManager->meshPendingChunks(camera, currentRenderSettings);
}

void Engine::renderScene() // Renamed from render
{
	auto startFrame = std::chrono::high_resolution_clock::now(); // Keep for total frame timing if needed outside ChunkManager
	RenderSettings &currentRenderSettings = uiManager->getRenderSettings();

	if (client && client->isConnected())
	{
		client->sendPlayerPosition(camera.getPosition().x, camera.getPosition().y, camera.getPosition().z);
		std::lock_guard<std::mutex> lock(client->playerMutex);
		for (const auto &[playerId, position] : client->playerPositions)
		{
			if (renderer)
				renderer->drawPlayer(camera, glm::vec3(position.x, position.y, position.z), playerId);
		}
	}

	// Shader parameters update before drawing chunks
	shader->use();
	shader->setFloat("fogStart", uiManager->getShaderParams().fogStart);
	shader->setFloat("fogEnd", uiManager->getShaderParams().fogEnd);
	shader->setFloat("fogDensity", uiManager->getShaderParams().fogDensity);
	shader->setVec3("fogColor", uiManager->getShaderParams().fogColor);
	shader->setVec3("sunDirection", uiManager->getShaderParams().sunDirection);
	shader->setFloat("ambientStrength", uiManager->getShaderParams().ambientStrength);
	shader->setFloat("diffuseIntensity", uiManager->getShaderParams().diffuseIntensity);
	shader->setFloat("lightLevels", uiManager->getShaderParams().lightLevels);
	shader->setFloat("saturationLevel", uiManager->getShaderParams().saturationLevel);
	shader->setFloat("colorBoost", uiManager->getShaderParams().colorBoost);
	shader->setFloat("gamma", uiManager->getShaderParams().gamma);

	if (renderer && shader && renderer->getTextureAtlas() && chunkManager)
	{
		// Pass currentRenderSettings by non-const reference if drawVisibleChunks updates it
		chunkManager->drawVisibleChunks(*shader, camera, renderer->getTextureAtlas(), uiManager->getShaderParams(), renderer.get(), currentRenderSettings, windowWidth, windowHeight);
	}

	updateVoxelHighlights();
	drawVoxelHighlight(destructionHighlight);
	drawVoxelHighlight(placementHighlight);

	auto endFrame = std::chrono::high_resolution_clock::now();
	// Total frame time is now more encompassing, including UI and other logic in run()
	uiManager->getRenderTiming().totalFrame = std::chrono::duration<float, std::milli>(endFrame - startFrame).count();
}

void Engine::handleEvents(bool &keyTPressed)
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		uiManager->processEvent(&event); // Pass event to UIManager for ImGui

		switch (event.type)
		{
		case SDL_EVENT_QUIT:
			this->running = false;
			break;
		case SDL_EVENT_WINDOW_RESIZED:
			windowWidth = event.window.data1;
			windowHeight = event.window.data2;
			glViewport(0, 0, windowWidth, windowHeight);
			if (renderer)
			{
				renderer->setScreenSize(windowWidth, windowHeight);
			}
			if (textRenderer)
			{
				textRenderer->setProjection(glm::ortho(0.0f, static_cast<float>(windowWidth), 0.0f, static_cast<float>(windowHeight)));
			}
			break;
		case SDL_EVENT_KEY_DOWN:
			if (event.key.scancode == SDL_SCANCODE_T)
			{
				if (!keyTPressed)
				{
					selectedTexture = static_cast<TextureType>((selectedTexture + 1) % COUNT);
					keyTPressed = true;
				}
			}
			if (event.key.scancode == SDL_SCANCODE_C)
			{
				this->isMousecaptured = !this->isMousecaptured;
				SDL_SetWindowRelativeMouseMode(this->window, this->isMousecaptured ? true : false);
			}
			break;

		case SDL_EVENT_KEY_UP:
			if (event.key.scancode == SDL_SCANCODE_T)
			{
				keyTPressed = false;
			}
			if (event.key.scancode == SDL_SCANCODE_ESCAPE)
			{
				this->running = false;
				break;
			}
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (!ImGui::GetIO().WantCaptureMouse) // Check ImGui focus
			{
				if (event.button.button == SDL_BUTTON_LEFT)
				{
					glm::vec3 hitPos, prevPos;
					if (raycast(camera.getPosition(), camera.getFront(), static_cast<float>(uiManager->getRenderSettings().raycastDistance), hitPos, prevPos))
					{
						if (chunkManager)
							chunkManager->deleteVoxel(hitPos);
					}
				}
				else if (event.button.button == SDL_BUTTON_RIGHT)
				{
					glm::vec3 hitPos, prevPos;
					if (raycast(camera.getPosition(), camera.getFront(), static_cast<float>(uiManager->getRenderSettings().raycastDistance), hitPos, prevPos))
					{
						if (chunkManager)
							chunkManager->placeVoxel(prevPos, selectedTexture);
					}
				}
			}
			break;

		case SDL_EVENT_MOUSE_MOTION:
			if (this->isMousecaptured && !ImGui::GetIO().WantCaptureMouse) // Check ImGui focus
			{
				this->camera.processMouseMovement(event.motion.xrel, event.motion.yrel);
			}
			break;

		default:
			break;
		}
	}
}

void Engine::updateVoxelHighlights()
{
	glm::vec3 hitPosition, previousPosition;
	bool hit = raycast(camera.getPosition(), camera.getFront(), static_cast<float>(uiManager->getRenderSettings().raycastDistance), hitPosition, previousPosition);

	if (hit)
	{
		destructionHighlight.active = true;
		destructionHighlight.position = glm::floor(hitPosition) + glm::vec3(0.5f); // Center of the voxel

		placementHighlight.active = true;
		placementHighlight.position = glm::floor(previousPosition) + glm::vec3(0.5f);
		placementHighlight.color = glm::vec3(0.2f, 0.8f, 0.2f); // Green for placement
	}
	else
	{
		destructionHighlight.active = false;
		placementHighlight.active = false;
	}
}

void Engine::drawVoxelHighlight(const VoxelHighlight &highlight)
{
	if (highlight.active && renderer)
	{
		renderer->drawVoxelHighlight(highlight.position, highlight.color, camera);
	}
}

RenderTiming &Engine::getRenderTiming()
{
	return uiManager->getRenderTiming();
}

void Engine::setWireframeMode(bool enabled)
{
	if (uiManager)
	{
		uiManager->getRenderSettings().wireframeMode = enabled;
	}
	if (enabled)
	{
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}
	else
	{
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
}

void Engine::startServer()
{
	if (!server && !client)
	{
		try
		{
			server = std::make_unique<Server>(25565, this->seed);
			std::cout << "Server started." << std::endl;
		}
		catch (const std::exception &e)
		{
			std::cerr << "Failed to start server: " << e.what() << std::endl;
			server.reset();
		}
	}
}

void Engine::stopServer()
{
	if (server)
	{
		server.reset();
		std::cout << "Server stopped." << std::endl;
	}
}

void Engine::connectToServer(const std::string &ip)
{
	if (!client && !server)
	{
		try
		{
			client = std::make_unique<Client>();
			client->connect(ip, 25565); // Assuming default port 25565
			this->seed = 0;
			std::cout << "Successfully connected to server at " << ip << std::endl;
			// Seed will be received from server
		}
		catch (const std::exception &e)
		{
			std::cerr << "Failed to connect to server: " << e.what() << std::endl;
			client.reset();
		}
	}
}

void Engine::disconnectClient()
{
	if (client)
	{
		client.reset();
		std::cout << "Disconnected from server." << std::endl;
	}
}

bool Engine::raycast(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, glm::vec3 &hitPosition, glm::vec3 &previousPosition)
{
	glm::vec3 pos = origin;
	glm::vec3 dir = glm::normalize(direction);
	float distanceTraveled = 0.0f;

	// Handle cases where direction components are zero to avoid division by zero in deltaDist
	// Initialize deltaDist with a large value if a direction component is zero.
	glm::vec3 deltaDist = glm::vec3(std::numeric_limits<float>::max());
	if (std::abs(dir.x) > 1e-6f)
		deltaDist.x = std::abs(1.0f / dir.x);
	if (std::abs(dir.y) > 1e-6f)
		deltaDist.y = std::abs(1.0f / dir.y);
	if (std::abs(dir.z) > 1e-6f)
		deltaDist.z = std::abs(1.0f / dir.z);

	glm::ivec3 currentVoxel = glm::floor(pos);
	glm::vec3 step = glm::sign(dir);
	glm::vec3 sideDist;

	sideDist.x = (step.x > 0) ? (currentVoxel.x + 1 - pos.x) * deltaDist.x : (pos.x - currentVoxel.x) * deltaDist.x;
	sideDist.y = (step.y > 0) ? (currentVoxel.y + 1 - pos.y) * deltaDist.y : (pos.y - currentVoxel.y) * deltaDist.y;
	sideDist.z = (step.z > 0) ? (currentVoxel.z + 1 - pos.z) * deltaDist.z : (pos.z - currentVoxel.z) * deltaDist.z;

	while (distanceTraveled < maxDistance)
	{
		previousPosition = glm::vec3(currentVoxel);

		int advanceAxis = 0;
		if (sideDist.x < sideDist.y)
		{
			if (sideDist.x < sideDist.z)
			{
				advanceAxis = 0; // X is smallest
			}
			else
			{
				advanceAxis = 2; // Z is smallest
			}
		}
		else
		{ // sideDist.y <= sideDist.x
			if (sideDist.y < sideDist.z)
			{
				advanceAxis = 1; // Y is smallest
			}
			else
			{
				advanceAxis = 2; // Z is smallest
			}
		}

		if (advanceAxis == 0)
		{
			sideDist.x += deltaDist.x;
			currentVoxel.x += static_cast<int>(step.x);
			distanceTraveled = sideDist.x - deltaDist.x; // Approximate distance
		}
		else if (advanceAxis == 1)
		{
			sideDist.y += deltaDist.y;
			currentVoxel.y += static_cast<int>(step.y);
			distanceTraveled = sideDist.y - deltaDist.y;
		}
		else
		{
			sideDist.z += deltaDist.z;
			currentVoxel.z += static_cast<int>(step.z);
			distanceTraveled = sideDist.z - deltaDist.z;
		}

		if (currentVoxel.y < 0 || currentVoxel.y >= CHUNK_HEIGHT)
			continue;

		if (chunkManager && chunkManager->isVoxelActive(glm::vec3(currentVoxel))) // Check chunkManager pointer
		{
			hitPosition = glm::vec3(currentVoxel);
			return true;
		}
	}
	return false;
}