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

	// const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(0);
	this->windowWidth = 1920;
	this->windowHeight = 1080;

	Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;
	if (FULLSCREEN == 0)
	{
		windowFlags |= SDL_WINDOW_FULLSCREEN;
	}
	else if (FULLSCREEN == 2)
	{
		windowFlags |= SDL_WINDOW_BORDERLESS;
	}

	this->window = SDL_CreateWindow("ft_vox", this->windowWidth, this->windowHeight, windowFlags);
	if (!this->window)
	{
		SDL_Quit();
		throw std::runtime_error("Failed to create SDL window");
	}
	SDL_SetWindowPosition(this->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);

	SDL_GLContext glContext = SDL_GL_CreateContext(this->window);
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

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	(void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	// io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // need to restore context
	ImGui::StyleColorsDark();

	ImGui_ImplSDL3_InitForOpenGL(this->window, glContext);
	ImGui_ImplOpenGL3_Init("#version 460");

	std::string path = RES_PATH + std::string("shaders/");
	this->shader = std::make_unique<Shader>((path + "vertex.glsl").c_str(), (path + "fragment.glsl").c_str());
	this->renderer = std::make_unique<Renderer>(windowWidth, windowHeight, this->renderSettings.maxRenderDistance);
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

	this->selectedTexture = TEXTURE_PLANK;

	uint32_t threadCount = std::thread::hardware_concurrency() / 2;
	this->threadPool = std::make_unique<ThreadPool>(threadCount);

	std::cout << "SDL version: " << SDL_GetVersion() << std::endl;
	std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;
	std::cout << "GLSL version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << std::endl;
	std::cout << "Vendor: " << glGetString(GL_VENDOR) << std::endl;
	std::cout << "Renderer: " << glGetString(GL_RENDERER) << std::endl;
	std::cout << "ImGui version: " << IMGUI_VERSION << std::endl;
	std::cout << "Threads: " << threadCount << std::endl;
}

Engine::~Engine()
{
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();

	SDL_GL_DestroyContext(SDL_GL_GetCurrentContext());
	SDL_DestroyWindow(this->window);
	SDL_Quit();
}

void Engine::perlinNoise(unsigned int seed)
{
	if (seed <= 0)
	{
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(100000, 999999);
		seed = dis(gen);
		this->perlin = std::make_unique<siv::PerlinNoise>(seed);
	}
	else
	{
		this->perlin = std::make_unique<siv::PerlinNoise>(seed);
	}
	this->seed = seed;
	std::cout << "Perlin seed: " << seed << std::endl;
}

void Engine::run()
{
	bool keyTPressed = false;
	this->running = true;
	this->isMousecaptured = true;

	while (this->running)
	{
		double currentFrame = SDL_GetTicks() / 1000.0;
		this->deltaTime = currentFrame - lastFrame;
		this->lastFrame = currentFrame;
		this->frameCount++;
		this->lastTime += this->deltaTime;

		if (this->lastTime >= 1.0f)
		{
			this->fps = this->frameCount / this->lastTime;
			this->frameCount = 0;
			this->lastTime = 0;
		}

		this->handleEvents(keyTPressed);

		const bool *keys = SDL_GetKeyboardState(NULL);
		if (isMousecaptured)
		{
			this->camera.processKeyboard(deltaTime, keys);
		}

		glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		this->render();
		this->renderer->drawSkybox(this->camera);

		this->textRenderer->renderText("FPS: " + std::to_string(this->fps), 10.0f, windowHeight - 40.0f, 0.5f, glm::vec3(1.0f, 1.0f, 1.0f));
		this->updateUI();

		SDL_GL_SwapWindow(this->window);
	}
}

void Engine::updateUI()
{
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();

	ImGui::Begin("ft_vox");

	ImGui::Text("FPS: %.1f (%.1f ms)", ImGui::GetIO().Framerate, this->deltaTime * 1000.0f);
	ImGui::Text("Visible chunks: %d", this->renderSettings.visibleChunksCount);
	ImGui::Text("Voxel count: %d", this->renderSettings.visibleVoxelsCount);
	ImGui::Text("Render distance: %d | %d", this->renderSettings.minRenderDistance, this->renderSettings.maxRenderDistance);
	ImGui::Text("X/Y/Z: (%.1f, %.1f, %.1f)", this->camera.getPosition().x, this->camera.getPosition().y, this->camera.getPosition().z);
	ImGui::Text("Player chunk: (%d, %d)", this->playerChunkPos.x, this->playerChunkPos.y);
	ImGui::Text("Speed: %.1f", this->camera.getMovementSpeed());
	ImGui::Text("Selected texture: %s (%d)", textureTypeString.at(this->selectedTexture).c_str(), this->selectedTexture);
	ImGui::InputInt("Raycast distance", &this->renderSettings.raycastDistance);

	ImGui::Checkbox("Wireframe", &this->renderSettings.wireframeMode);
	if (this->renderSettings.wireframeMode)
	{
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	}
	else
	{
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
	ImGui::Checkbox("Chunk borders", &this->renderSettings.chunkBorders);
	ImGui::InputInt("Chunk loaded max", &this->renderSettings.chunkLoadedMax);
	ImGui::Checkbox("Pause", &this->renderSettings.paused);

	ImGui::Text("Screen size: %d x %d", this->windowWidth, this->windowHeight);

	ImGui::Separator();

	if (ImGui::CollapsingHeader("Performance"))
	{
		if (ImGui::BeginTable("Performance", 2, ImGuiTableFlags_Borders))
		{
			ImGui::TableSetupColumn("Operation");
			ImGui::TableSetupColumn("Time (ms)");
			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Frustum culling");
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", renderTiming.frustumCulling);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Chunk generation");
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", renderTiming.chunkGeneration);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Mesh generation");
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", renderTiming.meshGeneration);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Chunk rendering");
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", renderTiming.chunkRendering);

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("Total frame");
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", renderTiming.totalFrame);

			ImGui::EndTable();
		}
	}

	ImGui::End();

	handleServerControls();
	handleShaderOptions();

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Engine::render()
{
	auto startFrame = std::chrono::high_resolution_clock::now();
	renderSettings.visibleChunksCount = 0;
	renderSettings.visibleVoxelsCount = 0;

	if (client && client->isConnected())
	{
		client->sendPlayerPosition(camera.getPosition().x, camera.getPosition().y, camera.getPosition().z);
		std::lock_guard<std::mutex> lock(client->playerMutex);
		for (const auto &[playerId, position] : client->playerPositions)
		{
			renderer->drawPlayer(camera, glm::vec3(position.x, position.y, position.z), playerId);
		}
	}

	// for debug just create a single chunk at 0,0,0
	// if (chunks.empty()) {
	//	chunks.emplace(glm::ivec3(0, 0, 0), Chunk(glm::vec3(0.0f, 0.0f, 0.0f)));
	//	chunks.begin()->second.generateVoxels(perlin.get());
	//	chunks.begin()->second.generateMesh(chunks);
	//	chunks.begin()->second.setVisible(true);
	//	chunks.begin()->second.setState(ChunkState::MESHED);
	//}

	auto start = std::chrono::high_resolution_clock::now();
	if (!renderSettings.paused)
	{
		const glm::ivec2 newPlayerChunkPos{
			static_cast<int>(std::floor(camera.getPosition().x / Chunk::SIZE)),
			static_cast<int>(std::floor(camera.getPosition().z / Chunk::SIZE))};

		if (newPlayerChunkPos != playerChunkPos)
		{
			playerChunkPos = newPlayerChunkPos;
			updateChunks();
		}
		frustumCulling();
	}
	auto end = std::chrono::high_resolution_clock::now();
	renderTiming.frustumCulling = std::chrono::duration<float, std::milli>(end - start).count();

	processChunkQueue();

	// async chunk generation
	start = std::chrono::high_resolution_clock::now();
	{
		std::vector<std::future<void>> futures;
		for (auto &chunk : chunks)
		{
			if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::UNLOADED)
			{
				auto perlinPtr = perlin.get();
				futures.push_back(threadPool->enqueue([&chunk, perlinPtr]()
													  { chunk.second.generateVoxels(perlinPtr); }));
			}
			// else if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::GENERATED) {
			//	futures.push_back(threadPool->enqueue([&chunk]() {
			//		chunk.second.generateMesh();
			//	}));
			// }
		}

		for (auto &future : futures)
		{
			future.wait();
		}
	}
	end = std::chrono::high_resolution_clock::now();
	renderTiming.chunkGeneration = std::chrono::duration<float, std::milli>(end - start).count();

	// async mesh generation
	start = std::chrono::high_resolution_clock::now();
	{
		std::vector<std::future<void>> futures;
		for (auto &chunk : chunks)
		{
			if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::GENERATED)
			{
				futures.push_back(threadPool->enqueue([&chunk]()
													  { chunk.second.generateMesh(); }));
			}
		}

		for (auto &future : futures)
		{
			future.wait();
		}
	}
	end = std::chrono::high_resolution_clock::now();
	renderTiming.meshGeneration = std::chrono::duration<float, std::milli>(end - start).count();

	// Render of visible chunks
	start = std::chrono::high_resolution_clock::now();
	for (auto &chunk : chunks)
	{
		if (!chunk.second.isVisible() || chunk.second.getState() == ChunkState::UNLOADED)
			continue;

		renderSettings.visibleVoxelsCount += chunk.second.draw(*shader, camera, renderer->getTextureAtlas(), shaderParams);
		renderSettings.visibleChunksCount++;

		if (renderSettings.chunkBorders)
		{
			renderer->drawBoundingBox(chunk.second, camera);
		}
	}
	end = std::chrono::high_resolution_clock::now();
	renderTiming.chunkRendering = std::chrono::duration<float, std::milli>(end - start).count();
}

void Engine::updateChunks()
{
	const glm::ivec3 cameraChunkPos{playerChunkPos.x, 0, playerChunkPos.y};
	const float minDistance = static_cast<float>(renderSettings.minRenderDistance) / Chunk::SIZE - 1.0f;
	const float maxDistance = static_cast<float>(renderSettings.maxRenderDistance) / Chunk::SIZE;

	// Clean up far chunks
	{
		std::lock_guard<std::mutex> lock(chunkMutex);
		std::erase_if(chunks, [&](const auto &chunk)
					  { return !isChunkInRange(chunk.first - cameraChunkPos, maxDistance); });
	}

	// Generate new chunks
	const int radius = static_cast<int>(minDistance);
	for (int x = -radius; x <= radius; x++)
	{
		for (int z = -radius; z <= radius; z++)
		{
			const glm::ivec3 chunkPos = glm::ivec3(cameraChunkPos.x + x, 0, cameraChunkPos.z + z);
			if (isChunkInRange(glm::ivec3{x, 0, z}, minDistance) &&
				chunks.find(chunkPos) == chunks.end())
			{
				chunkGenerationQueue.push(chunkPos);
			}
		}
	}
}

bool Engine::isChunkInRange(const glm::ivec3 &chunkPos, float distance) const
{
	return glm::length(glm::vec2(chunkPos.x, chunkPos.z)) <= distance;
}

void Engine::processChunkQueue()
{
	std::lock_guard<std::mutex> lock(chunkMutex);
	int chunkCount = 0;
	while (!chunkGenerationQueue.empty() && chunkCount < renderSettings.chunkLoadedMax)
	{
		const glm::ivec3 pos = chunkGenerationQueue.front();
		chunkGenerationQueue.pop();
		chunks.emplace(pos, std::move(Chunk(glm::vec3(pos.x * Chunk::SIZE, 0.0f, pos.z * Chunk::SIZE))));
		chunkCount++;
	}
}

void Engine::frustumCulling()
{
	glm::mat4 clipMatrix = this->camera.getProjectionMatrix(static_cast<float>(windowWidth), static_cast<float>(windowHeight), static_cast<float>(renderSettings.maxRenderDistance)) * this->camera.getViewMatrix();
	std::array<glm::vec4, 6> frustumPlanes;

	frustumPlanes[0] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 0); // Left
	frustumPlanes[1] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 0); // Right
	frustumPlanes[2] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 1); // Bottom
	frustumPlanes[3] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 1); // Top
	frustumPlanes[4] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 2); // Far
	frustumPlanes[5] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 2); // Near

	for (auto &plane : frustumPlanes)
	{
		plane /= glm::length(glm::vec3(plane));
	}

	for (auto &chunk : this->chunks)
	{
		glm::vec3 chunkPosition = chunk.second.getPosition();
		glm::vec3 corner = chunkPosition + glm::vec3(Chunk::SIZE, Chunk::HEIGHT, Chunk::SIZE);
		glm::vec3 center = chunkPosition + glm::vec3(Chunk::SIZE / 2.0f, Chunk::HEIGHT / 2.0f, Chunk::SIZE / 2.0f);
		float radius = glm::length(corner - center);
		;

		bool inside = true;
		for (const auto &plane : frustumPlanes)
		{
			float distance = glm::dot(glm::vec3(plane), center) + plane.w;
			if (distance < -radius)
			{
				inside = false;
				break;
			}
		}

		if (inside)
		{
			chunk.second.setVisible(true);
		}
		else
		{
			chunk.second.setVisible(false);
		}
	}
}

bool Engine::isVoxelActive(float x, float y, float z) const
{
	int chunkX = static_cast<int>(std::floor(x / Chunk::SIZE));
	int chunkZ = static_cast<int>(std::floor(z / Chunk::SIZE));
	auto chunkIt = chunks.find(glm::ivec3(chunkX, 0, chunkZ));
	if (chunkIt != chunks.end())
	{
		return chunkIt->second.isVoxelActiveGlobalPos(x, y, z);
	}
	return false;
}

bool Engine::raycast(const glm::vec3 &origin, const glm::vec3 &direction, float maxDistance, glm::vec3 &hitPosition, glm::vec3 &previousPosition)
{
	glm::vec3 pos = origin;
	glm::vec3 dir = glm::normalize(direction);
	float distance = 0.0f;

	glm::vec3 voxelPos = glm::floor(pos);
	glm::vec3 deltaDist = glm::abs(glm::vec3(1.0f) / dir);
	glm::vec3 step = glm::sign(dir);

	glm::vec3 sideDist = (voxelPos + glm::max(step, glm::vec3(0.0f)) - pos) * deltaDist;

	while (distance < maxDistance)
	{
		if (isVoxelActive(voxelPos.x, voxelPos.y, voxelPos.z))
		{
			hitPosition = voxelPos;
			return true;
		}

		previousPosition = voxelPos;

		if (sideDist.x < sideDist.y)
		{
			if (sideDist.x < sideDist.z)
			{
				sideDist.x += deltaDist.x;
				voxelPos.x += step.x;
			}
			else
			{
				sideDist.z += deltaDist.z;
				voxelPos.z += step.z;
			}
		}
		else
		{
			if (sideDist.y < sideDist.z)
			{
				sideDist.y += deltaDist.y;
				voxelPos.y += step.y;
			}
			else
			{
				sideDist.z += deltaDist.z;
				voxelPos.z += step.z;
			}
		}

		distance = glm::length(glm::vec3(voxelPos) - origin);
	}

	return false;
}

void Engine::handleEvents(bool &keyTPressed)
{
	SDL_Event event;
	while (SDL_PollEvent(&event))
	{
		ImGui_ImplSDL3_ProcessEvent(&event);
		switch (event.type)
		{
		case SDL_EVENT_QUIT:
			this->running = false;
			break;

		case SDL_EVENT_KEY_DOWN:
			if (event.key.scancode == SDL_SCANCODE_T)
			{
				if (!keyTPressed)
				{
					selectedTexture = static_cast<TextureType>((selectedTexture + 1) % TEXTURE_COUNT);
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
			if (event.button.button == SDL_BUTTON_LEFT)
			{
				glm::vec3 hitPos, prevPos;
				if (raycast(camera.getPosition(), camera.getFront(), static_cast<float>(renderSettings.raycastDistance), hitPos, prevPos))
				{
					int chunkX = static_cast<int>(std::floor(hitPos.x / Chunk::SIZE));
					int chunkZ = static_cast<int>(std::floor(hitPos.z / Chunk::SIZE));
					auto chunkIt = chunks.find(glm::ivec3(chunkX, 0, chunkZ));
					if (chunkIt != chunks.end())
					{
						if (chunkIt->second.deleteVoxel(hitPos))
						{
							return;
						}
					}
				}
			}
			else if (event.button.button == SDL_BUTTON_RIGHT)
			{
				glm::vec3 hitPos, prevPos;
				if (raycast(camera.getPosition(), camera.getFront(), static_cast<float>(renderSettings.raycastDistance), hitPos, prevPos))
				{
					int chunkX = static_cast<int>(std::floor(prevPos.x / Chunk::SIZE));
					int chunkZ = static_cast<int>(std::floor(prevPos.z / Chunk::SIZE));
					auto chunkIt = chunks.find(glm::ivec3(chunkX, 0, chunkZ));
					if (chunkIt != chunks.end())
					{
						if (chunkIt->second.placeVoxel(prevPos, selectedTexture))
						{
							return;
						}
					}
				}
			}
			break;

		case SDL_EVENT_MOUSE_MOTION:
			// G�rer le d�placement de la souris
			if (this->isMousecaptured)
			{
				this->camera.processMouseMovement(event.motion.xrel, event.motion.yrel);
			}
			break;

		case SDL_EVENT_MOUSE_WHEEL:
			// G�rer la molette de la souris si n�cessaire
			break;

		default:
			break;
		}
	}
}

void Engine::handleServerControls()
{
	ImGui::Begin("Multiplayer");

	// Server controls
	if (ImGui::CollapsingHeader("Server Controls"))
	{
		if (server && server->isRunning())
		{
			if (ImGui::Button("Stop server"))
			{
				server->stop();
			}
			ImGui::Text("Server is running...");
			ImGui::Text("Clients connected: %zu", server->getClientCount());
		}
		else
		{
			if (ImGui::Button("Start Server"))
			{
				server = std::make_unique<Server>(25565, seed); // port
				server->start();
			}
		}
	}

	// Client controls
	if (ImGui::CollapsingHeader("Client Controls"))
	{
		ImGui::InputText("Server IP", ipInputBuffer, IM_ARRAYSIZE(ipInputBuffer));
		if (client && client->isConnected())
		{
			if (ImGui::Button("Disconnect"))
			{
				client->disconnect();
			}
			ImGui::Text("Connected to server at %s", ipInputBuffer);
		}
		else
		{
			if (ImGui::Button("Connect"))
			{
				client = std::make_unique<Client>();
				client->connect(ipInputBuffer, 25565); // ip, port
				this->seed = 0;
			}
		}
	}

	// Display all the player positions
	if (client && client->isConnected())
	{
		if (ImGui::CollapsingHeader("Player Positions"))
		{
			std::lock_guard<std::mutex> lock(client->playerMutex);
			for (const auto &[playerId, position] : client->playerPositions)
			{
				glm::vec3 color = renderer->computeColorFromPlayerId(playerId);
				ImGui::TextColored(ImVec4(color.x, color.y, color.z, 1.0f), "Player %d: (%.1f, %.1f, %.1f)", playerId, position.x, position.y, position.z);
			}
		}
	}

	if (this->seed == 0 && client->getWorldSeed() != 0)
	{
		this->seed = client->getWorldSeed();
		perlinNoise(this->seed);
		// clear chunks
		chunks.clear();
		chunkGenerationQueue = std::queue<glm::ivec3>();
		playerChunkPos = glm::ivec2(42, -42);
	}

	ImGui::End();
}

void Engine::handleShaderOptions()
{
	ImGui::Begin("Graphics Settings");

	// Interface pour les paramètres du fog
	if (ImGui::CollapsingHeader("Fog Settings"))
	{
		ImGui::SliderFloat("Fog Start", &shaderParams.fogStart, 0.0f, 200.0f);
		ImGui::SliderFloat("Fog End", &shaderParams.fogEnd, shaderParams.fogStart + 10.0f, 300.0f);
		float fogColorArray[3] = {
			shaderParams.fogColor.r,
			shaderParams.fogColor.g,
			shaderParams.fogColor.b};
		if (ImGui::ColorEdit3("Fog Color", fogColorArray))
		{
			shaderParams.fogColor = glm::vec3(fogColorArray[0], fogColorArray[1], fogColorArray[2]);
		}
		ImGui::SliderFloat("Fog Density", &shaderParams.fogDensity, 0.1f, 2.0f);
	}

	// Interface pour les paramètres d'éclairage
	if (ImGui::CollapsingHeader("Lighting Settings"))
	{
		ImGui::SliderFloat("Ambient Strength", &shaderParams.ambientStrength, 0.0f, 1.0f);
		ImGui::SliderFloat("Diffuse Intensity", &shaderParams.diffuseIntensity, 0.0f, 1.0f);
		ImGui::SliderFloat("Light Levels", &shaderParams.lightLevels, 1.0f, 10.0f);

		float sunDir[3] = {
			shaderParams.sunDirection.x,
			shaderParams.sunDirection.y,
			shaderParams.sunDirection.z};
		if (ImGui::SliderFloat3("Sun Direction", sunDir, -1.0f, 1.0f))
		{
			// Normaliser la direction
			shaderParams.sunDirection = glm::normalize(glm::vec3(sunDir[0], sunDir[1], sunDir[2]));
		}
	}

	// Interface pour les paramètres visuels
	if (ImGui::CollapsingHeader("Visual Settings"))
	{
		ImGui::SliderFloat("Saturation Level", &shaderParams.saturationLevel, 0.5f, 2.0f);
		ImGui::SliderFloat("Color Boost", &shaderParams.colorBoost, 0.5f, 2.0f);
		ImGui::SliderFloat("Gamma", &shaderParams.gamma, 1.0f, 3.0f);
	}

	// Bouton pour réinitialiser tous les paramètres
	if (ImGui::Button("Reset All Settings"))
	{
		shaderParams = ShaderParameters(); // Réinitialise avec les valeurs par défaut
	}

	ImGui::End();
}