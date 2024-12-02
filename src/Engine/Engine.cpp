#include "Engine.hpp"

#define FULLSCREEN 1 // 0 = fullscreen, 1 = windowed, 2 = borderless

Engine::Engine()
{
	if (!glfwInit()) {
		throw std::runtime_error("Failed to initialize GLFW");
	}

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	#ifdef __APPLE__
		glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	#endif

	this->monitor = glfwGetPrimaryMonitor();
	this->mode = glfwGetVideoMode(this->monitor);
	this->windowWidth = this->mode->width;
	this->windowHeight = this->mode->height;
	if (FULLSCREEN == 1) {
		this->windowWidth = 1920;
		this->windowHeight = 1080;
		this->window = glfwCreateWindow(windowWidth, windowHeight, "ft_vox", nullptr, nullptr);
	} else if (FULLSCREEN == 2) {
		this->window = glfwCreateWindow(windowWidth, windowHeight, "ft_vox", nullptr, nullptr);
	} else {
		this->window = glfwCreateWindow(windowWidth, windowHeight, "ft_vox", this->monitor, nullptr);
	}
	if (!this->window) {
		glfwTerminate();
		throw std::runtime_error("Failed to create GLFW window");
	}

	glfwMakeContextCurrent(this->window);
	glfwSwapInterval(1); // vsync
	glfwSetWindowUserPointer(this->window, &this->camera);
	glfwSetInputMode(this->window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
	glfwSetCursorPosCallback(this->window, mouse_callback);
	glfwSetKeyCallback(this->window, key_callback);

	if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
		glfwTerminate();
		throw std::runtime_error("Failed to initialize GLAD");
	}

	ImGui::CreateContext();
	ImGui_ImplGlfw_InitForOpenGL(this->window, true);
	ImGui_ImplOpenGL3_Init("#version 460");
	ImGui::StyleColorsDark();

	std::string path = RES_PATH + std::string("shaders/");
	this->shader = std::make_unique<Shader>((path + "vertex.glsl").c_str(), (path + "fragment.glsl").c_str());
	this->renderSettings.minRenderDistance = 320; // 224
	this->renderSettings.maxRenderDistance = 320; // 320
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

	this->renderSettings.wireframeMode = false;
	this->renderSettings.chunkBorders = false;
	this->renderSettings.visibleChunksCount = 0;
	this->renderSettings.visibleVoxelsCount = 0;
	this->renderSettings.chunkLoadedMax = 3;
	this->selectedTexture = TEXTURE_PLANK;
	this->renderSettings.paused = false;
	this->renderSettings.perfMode = false;

	uint32_t threadCount = std::thread::hardware_concurrency() / 2;
	this->threadPool = std::make_unique<ThreadPool>(threadCount);

	std::cout << "GLFW version: " << glfwGetVersionString() << std::endl;
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
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(this->window);
	glfwTerminate();
}

void	Engine::perlinNoise(unsigned int seed)
{
	if (seed <= 0) {
		std::random_device rd;
		std::mt19937 gen(rd());
		std::uniform_int_distribution<> dis(100000, 999999);
		seed = dis(gen);
		this->perlin = std::make_unique<siv::PerlinNoise>(seed);
	} else {
		this->perlin = std::make_unique<siv::PerlinNoise>(seed);
	}
	std::cout << "Perlin seed: " << seed << std::endl;
}

void	Engine::run()
{
	bool keyTPressed = false;

	while (!glfwWindowShouldClose(this->window)) {
		float currentFrame = glfwGetTime();
		this->deltaTime = currentFrame - lastFrame;
		this->lastFrame = currentFrame;
		this->frameCount++;
		this->lastTime += this->deltaTime;

		if (this->lastTime >= 1.0f) {
			this->fps = this->frameCount / this->lastTime;
			this->frameCount = 0;
			this->lastTime = 0;
		}

		this->camera.processKeyboard(deltaTime);
		this->handleInput(keyTPressed);

		glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		this->render();
		this->renderer->drawSkybox(this->camera);

		this->textRenderer->renderText("FPS: " + std::to_string(this->fps), 10.0f, windowHeight - 40.0f, 0.5f, glm::vec3(1.0f, 1.0f, 1.0f));
		this->updateUI();

		glfwSwapBuffers(this->window);
		glfwPollEvents();
	}
}

void	Engine::updateUI()
{
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	ImGui::Begin("ft_vox");

	ImGui::Text("FPS: %.1f (%.1f ms)", ImGui::GetIO().Framerate, this->deltaTime * 1000.0f);
	ImGui::Text("Visible chunks: %d", this->renderSettings.visibleChunksCount);
	ImGui::Text("Voxel count: %d",  this->renderSettings.visibleVoxelsCount);
	ImGui::Text("Render distance: %d | %d", this->renderSettings.minRenderDistance, this->renderSettings.maxRenderDistance);
	ImGui::Text("X/Y/Z: (%.1f, %.1f, %.1f)", this->camera.getPosition().x, this->camera.getPosition().y, this->camera.getPosition().z);
	ImGui::Text("Player chunk: (%d, %d)", this->playerChunkPos.x, this->playerChunkPos.y);
	ImGui::Text("Speed: %.1f", this->camera.getMovementSpeed());
	ImGui::Text("Selected texture: %s (%d)", textureTypeString.at(this->selectedTexture).c_str(), this->selectedTexture);

	ImGui::Checkbox("Wireframe", &this->renderSettings.wireframeMode);
	if (this->renderSettings.wireframeMode) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	} else {
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
	ImGui::Checkbox("Chunk borders", &this->renderSettings.chunkBorders);
	ImGui::InputInt("Chunk loaded max", &this->renderSettings.chunkLoadedMax);
	ImGui::Checkbox("Pause", &this->renderSettings.paused);
	ImGui::Checkbox("Performance mode", &this->renderSettings.perfMode);

	ImGui::Text("Screen size: %d x %d", this->windowWidth, this->windowHeight);

	ImGui::Separator();

	if (ImGui::CollapsingHeader("Performance")) {
		if (ImGui::BeginTable("Performance", 2, ImGuiTableFlags_Borders)) {
			ImGui::TableSetupColumn("Operation");
			ImGui::TableSetupColumn("Time (ms)");
			ImGui::TableHeadersRow();

			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::Text("Frustum culling");
			ImGui::TableNextColumn(); ImGui::Text("%.2f", renderTiming.frustumCulling);

			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::Text("Chunk generation");
			ImGui::TableNextColumn(); ImGui::Text("%.2f", renderTiming.chunkGeneration);

			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::Text("Mesh generation");
			ImGui::TableNextColumn(); ImGui::Text("%.2f", renderTiming.meshGeneration);

			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::Text("Chunk rendering");
			ImGui::TableNextColumn(); ImGui::Text("%.2f", renderTiming.chunkRendering);

			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::Text("Total frame");
			ImGui::TableNextColumn(); ImGui::Text("%.2f", renderTiming.totalFrame);

			ImGui::EndTable();
		}
	}

	ImGui::End();

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void	Engine::render()
{
	auto startFrame = std::chrono::high_resolution_clock::now();
	renderSettings.visibleChunksCount = 0;
	renderSettings.visibleVoxelsCount = 0;

	// for debug just create a single chunk at 0,0,0
	//if (chunks.empty()) {
	//	chunks.emplace(glm::ivec3(0, 0, 0), Chunk(glm::vec3(0.0f, 0.0f, 0.0f)));
	//	chunks.begin()->second.generateVoxels(perlin.get());
	//	chunks.begin()->second.generateMesh(chunks);
	//	chunks.begin()->second.setVisible(true);
	//	chunks.begin()->second.setState(ChunkState::MESHED);
	//}

	auto start = std::chrono::high_resolution_clock::now();
	if (!renderSettings.paused) {
		const glm::ivec2 newPlayerChunkPos{
			static_cast<int>(std::floor(camera.getPosition().x / Chunk::SIZE)),
			static_cast<int>(std::floor(camera.getPosition().z / Chunk::SIZE))
		};

		if (newPlayerChunkPos != playerChunkPos) {
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
		for (auto& chunk : chunks) {
			if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::UNLOADED) {
				auto perlinPtr = perlin.get();
				futures.push_back(threadPool->enqueue([&chunk, perlinPtr]() {
					chunk.second.generateVoxels(perlinPtr);
				}));
			}
			//else if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::GENERATED) {
			//	futures.push_back(threadPool->enqueue([&chunk]() {
			//		chunk.second.generateMesh();
			//	}));
			//}
		}

		for (auto& future : futures) {
			future.wait();
		}
	}
	end = std::chrono::high_resolution_clock::now();
	renderTiming.chunkGeneration = std::chrono::duration<float, std::milli>(end - start).count();

	// async mesh generation
	start = std::chrono::high_resolution_clock::now();
	{
		std::vector<std::future<void>> futures;
		for (auto& chunk : chunks) {
			if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::GENERATED) {
				futures.push_back(threadPool->enqueue([&chunk]() {
					chunk.second.generateMesh();
				}));
			}
		}

		for (auto& future : futures) {
			future.wait();
		}
	}
	end = std::chrono::high_resolution_clock::now();
	renderTiming.meshGeneration = std::chrono::duration<float, std::milli>(end - start).count();

	// Render of visible chunks
	start = std::chrono::high_resolution_clock::now();
	for (auto& chunk : chunks) {
		if (!chunk.second.isVisible() || chunk.second.getState() == ChunkState::UNLOADED) continue;

		renderSettings.visibleVoxelsCount += chunk.second.draw(*shader, camera, renderer->getTextureAtlas());
		renderSettings.visibleChunksCount++;

		if (renderSettings.chunkBorders) {
			renderer->drawBoundingBox(chunk.second, camera);
		}
	}
	end = std::chrono::high_resolution_clock::now();
	renderTiming.chunkRendering = std::chrono::duration<float, std::milli>(end - start).count();
}

void	Engine::updateChunks()
{
	const glm::ivec3 cameraChunkPos{playerChunkPos.x, 0, playerChunkPos.y};
	const float minDistance = static_cast<float>(renderSettings.minRenderDistance) / Chunk::SIZE - 1.0f;
	const float maxDistance = renderSettings.perfMode ? minDistance : static_cast<float>(renderSettings.maxRenderDistance) / Chunk::SIZE;

	// Clean up far chunks
	{
		std::lock_guard<std::mutex> lock(chunkMutex);
		std::erase_if(chunks, [&](const auto& chunk) {
			return !isChunkInRange(chunk.first - cameraChunkPos, maxDistance);
		});
	}

	// Generate new chunks
	const int radius = static_cast<int>(minDistance);
	for (int x = -radius; x <= radius; x++) {
		for (int z = -radius; z <= radius; z++) {
			const glm::ivec3 chunkPos = glm::ivec3(cameraChunkPos.x + x, 0, cameraChunkPos.z + z);
			if (isChunkInRange(glm::ivec3{x, 0, z}, minDistance) &&
				chunks.find(chunkPos) == chunks.end()) {
				chunkGenerationQueue.push(chunkPos);
			}
		}
	}
}

bool	Engine::isChunkInRange(const glm::ivec3& chunkPos, float distance) const
{
	return glm::length(glm::vec2(chunkPos.x, chunkPos.z)) <= distance;
}

void	Engine::processChunkQueue()
{
	std::lock_guard<std::mutex> lock(chunkMutex);
	int chunkCount = 0;
	while (!chunkGenerationQueue.empty() && chunkCount < renderSettings.chunkLoadedMax) {
		const auto pos = chunkGenerationQueue.front();
		chunkGenerationQueue.pop();

		Chunk chunk(glm::vec3(pos.x * Chunk::SIZE, 0.0f, pos.z * Chunk::SIZE));
		chunks.emplace(pos, std::move(chunk));
		chunkCount++;
	}
}

void	Engine::frustumCulling()
{
	glm::mat4	clipMatrix = this->camera.getProjectionMatrix(windowWidth, windowHeight, renderSettings.maxRenderDistance) * this->camera.getViewMatrix();
	std::array<glm::vec4, 6>	frustumPlanes;

	frustumPlanes[0] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 0); // Left
	frustumPlanes[1] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 0); // Right
	frustumPlanes[2] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 1); // Bottom
	frustumPlanes[3] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 1); // Top
	frustumPlanes[4] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 2); // Far
	frustumPlanes[5] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 2); // Near

	for (auto& plane : frustumPlanes) {
		plane /= glm::length(glm::vec3(plane));
	}

	for (auto& chunk : this->chunks) {
		glm::vec3 chunkPosition = chunk.second.getPosition();
		glm::vec3 corner = chunkPosition + glm::vec3(Chunk::SIZE, Chunk::HEIGHT, Chunk::SIZE);
		glm::vec3 center = chunkPosition + glm::vec3(Chunk::SIZE / 2.0f, Chunk::HEIGHT / 2.0f, Chunk::SIZE / 2.0f);
		float radius = glm::length(corner - center);;

		bool inside = true;
		for (const auto& plane : frustumPlanes) {
			float distance = glm::dot(glm::vec3(plane), center) + plane.w;
			if (distance < -radius) {
				inside = false;
				break;
			}
		}

		if (inside) {
			chunk.second.setVisible(true);
		} else {
			chunk.second.setVisible(false);
		}
	}
}

void	Engine::handleInput(bool& keyTPressed)
{
	if (glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS) {
		if (!keyTPressed) {
			selectedTexture = static_cast<TextureType>((selectedTexture + 1) % TEXTURE_COUNT);
			keyTPressed = true;
		}
	} else {
		keyTPressed = false;
	}

	if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
		auto chunk = this->chunks.find(glm::ivec3(this->playerChunkPos.x, 0, this->playerChunkPos.y));
		if (chunk != this->chunks.end()) {
			if (chunk->second.deleteVoxel(this->camera.getPosition(), this->camera.getFront())) {
				return;
			}
		}
	}
	if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
		auto chunk = this->chunks.find(glm::ivec3(this->playerChunkPos.x, 0, this->playerChunkPos.y));
		if (chunk != this->chunks.end()) {
			if (chunk->second.placeVoxel(this->camera.getPosition(), this->camera.getFront(), selectedTexture)) {
				return;
			}
		}
	}
}
