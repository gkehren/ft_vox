#include "Engine.hpp"

#define FULLSCREEN 1 // 0 = fullscreen, 1 = windowed, 2 = borderless

Engine::Engine()
{
	if (!glfwInit()) {
		throw std::runtime_error("Failed to initialize GLFW");
	}

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
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
	glfwSwapInterval(0); // vsync
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
	this->shader = new Shader((path + "vertex.glsl").c_str(), (path + "fragment.glsl").c_str());
	this->minRenderDistance = 224;
	this->maxRenderDistance = 320;
	this->renderer = new Renderer(windowWidth, windowHeight, this->maxRenderDistance);
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

	textRenderer = new TextRenderer(RES_PATH + std::string("fonts/FiraCode.ttf"), glm::ortho(0.0f, static_cast<float>(windowWidth), 0.0f, static_cast<float>(windowHeight)));

	this->wireframeMode = false;
	this->chunkBorders = false;
	this->visibleChunksCount = 0;
	this->visibleVoxelsCount = 0;
	this->chunkLoadedMax = 5;
	this->selectedTexture = TEXTURE_PLANK;
	this->paused = false;
	this->perfMode = false;

	this->threadPool = new ThreadPool(std::thread::hardware_concurrency());

	std::cout << "GLFW version: " << glfwGetVersionString() << std::endl;
	std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;
	std::cout << "GLSL version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << std::endl;
	std::cout << "Vendor: " << glGetString(GL_VENDOR) << std::endl;
	std::cout << "Renderer: " << glGetString(GL_RENDERER) << std::endl;
	std::cout << "ImGui version: " << IMGUI_VERSION << std::endl;
	std::cout << "Threads: " << std::thread::hardware_concurrency() << std::endl;
}

Engine::~Engine()
{
	delete this->shader;
	delete this->renderer;
	delete this->textRenderer;
	delete this->perlin;

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
		this->perlin = new siv::PerlinNoise(seed);
	} else {
		this->perlin = new siv::PerlinNoise(seed);
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
	ImGui::Text("Visible chunks: %d", this->visibleChunksCount);
	ImGui::Text("Voxel count: %d",  this->visibleVoxelsCount);
	ImGui::Text("Render distance: %d | %d", this->minRenderDistance, this->maxRenderDistance);
	ImGui::Text("X/Y/Z: (%.1f, %.1f, %.1f)", this->camera.getPosition().x, this->camera.getPosition().y, this->camera.getPosition().z);
	ImGui::Text("Player chunk: (%d, %d)", this->playerChunkPos.x, this->playerChunkPos.y);
	ImGui::Text("Speed: %.1f", this->camera.getMovementSpeed());
	ImGui::Text("Selected texture: %s (%d)", textureTypeString.at(this->selectedTexture).c_str(), this->selectedTexture);

	ImGui::Checkbox("Wireframe", &this->wireframeMode);
	if (this->wireframeMode) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	} else {
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
	ImGui::Checkbox("Chunk borders", &this->chunkBorders);
	ImGui::InputInt("Chunk loaded max", &this->chunkLoadedMax);
	ImGui::Checkbox("Pause", &this->paused);
	ImGui::Checkbox("Performance mode", &this->perfMode);

	ImGui::Text("Screen size: %d x %d", this->windowWidth, this->windowHeight);

	ImGui::End();
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void	Engine::render()
{
	this->visibleChunksCount = 0;
	this->visibleVoxelsCount = 0;

	if (!this->paused) {
		glm::ivec2 newPlayerChunkPos = glm::ivec2(std::floor(this->camera.getPosition().x / Chunk::SIZE), std::floor(this->camera.getPosition().z / Chunk::SIZE));
		if (newPlayerChunkPos != this->playerChunkPos) {
			this->playerChunkPos = newPlayerChunkPos;
			this->chunkManagement();
		}
		this->frustumCulling();
	}

	for (const auto& pos : this->chunksToCreate) {
		threadPool->enqueue([&, pos]() {
			Chunk chunk(glm::vec3(pos.x * Chunk::SIZE, 0, pos.z * Chunk::SIZE));
			std::lock_guard<std::mutex> lock(mutex);
			this->chunks.insert(std::make_pair(pos, std::move(chunk)));
		});
	}
	this->chunksToCreate.clear();

	for (auto& chunk : this->chunks) {
		threadPool->enqueue([this, &chunk]() {
			if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::UNLOADED) {
				chunk.second.generateVoxel(this->perlin);
			}
		});
	}

	int chunkLoaded2 = 0;
	for (auto& chunk : this->chunks) {
		if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::REMESHED) {
			chunk.second.generateMesh(this->chunks, this->perlin);
			chunkLoaded2++;
		}
		if (chunkLoaded2 > 4) {
			break;
		}
	}

	int chunkLoaded = 0;
	for (auto& chunk : this->chunks) {
		if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::GENERATED) {
			chunk.second.generateMesh(this->chunks, perlin);
			chunkLoaded++;
		}
		if (chunkLoaded >= this->chunkLoadedMax) {
			break;
		}
	}

	for (auto& chunk : this->chunks) {
		if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::MESHED || chunk.second.getState() == ChunkState::REMESHED) {
			this->visibleVoxelsCount += this->renderer->draw(chunk.second, *this->shader, this->camera);
			this->visibleChunksCount++;
			if (chunkBorders)
				this->renderer->drawBoundingBox(chunk.second, this->camera);
		}
	}
}

void	Engine::chunkManagement()
{
	glm::ivec3 cameraChunkPos = glm::ivec3(this->playerChunkPos.x, 0, this->playerChunkPos.y);
	int minChunkRenderDistance = (this->minRenderDistance / Chunk::SIZE) - 1;
	int maxChunkRenderDistance;
	if (perfMode) {
		maxChunkRenderDistance = minChunkRenderDistance;
	} else {
		maxChunkRenderDistance = (this->maxRenderDistance / Chunk::SIZE);
	}

	if (static_cast<int>(this->frameCount) % 2 == 0) {
		for (auto it = this->chunks.begin(); it != this->chunks.end();) {
			glm::ivec3 chunkPos = it->first;
			glm::vec3 diff = glm::vec3(chunkPos - cameraChunkPos);

			if (glm::length(diff) > maxChunkRenderDistance) {
				it = this->chunks.erase(it);
			} else {
				it++;
			}
		}
	} else {
		for (int x = -minChunkRenderDistance; x <= minChunkRenderDistance; x++) {
			for (int z = -minChunkRenderDistance; z <= minChunkRenderDistance; z++) {
				glm::ivec3 chunkPos = glm::ivec3(cameraChunkPos.x + x, 0, cameraChunkPos.z + z);
				if (this->chunks.find(chunkPos) == this->chunks.end() && glm::length(glm::vec3(chunkPos - cameraChunkPos)) <= minChunkRenderDistance) {
					this->chunksToCreate.push_back(chunkPos);
				}
			}
		}
	}
}

void	Engine::frustumCulling()
{
	glm::mat4	clipMatrix = this->camera.getProjectionMatrix(windowWidth, windowHeight, this->maxRenderDistance) * this->camera.getViewMatrix();
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
		for (auto& chunk : this->chunks) {
			if (chunk.second.deleteVoxel(this->camera.getPosition(), this->camera.getFront())) {
				break;
			}
		}
	}
	if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
		for (auto& chunk : this->chunks) {
			if (chunk.second.placeVoxel(this->camera.getPosition(), this->camera.getFront(), selectedTexture)) {
				break;
			}
		}
	}
}
