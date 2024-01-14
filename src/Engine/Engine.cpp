#include "Engine.hpp"

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

	this->window = glfwCreateWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "ft_vox", nullptr, nullptr);
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
	ImGui_ImplOpenGL3_Init("#version 410");
	ImGui::StyleColorsDark();

	std::string path = RES_PATH + std::string("shaders/");
	this->shader = new Shader((path + "vertex.glsl").c_str(), (path + "fragment.glsl").c_str());
	this->renderDistance = 160;
	this->renderer = new Renderer(WINDOW_WIDTH, WINDOW_HEIGHT, this->renderDistance);
	this->camera.setWindow(this->window);

	glActiveTexture(GL_TEXTURE0);
	this->shader->setInt("textureSampler", 0);

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	textRenderer = new TextRenderer(RES_PATH + std::string("fonts/FiraCode.ttf"), glm::ortho(0.0f, static_cast<float>(WINDOW_WIDTH), 0.0f, static_cast<float>(WINDOW_HEIGHT)));

	this->wireframeMode = false;
	this->chunkBorders = false;
	this->visibleChunksCount = 0;
	this->visibleVoxelsCount = 0;
	this->chunkLoadedMax = 1;

	std::cout << "GLFW version: " << glfwGetVersionString() << std::endl;
	std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;
	std::cout << "GLSL version: " << glGetString(GL_SHADING_LANGUAGE_VERSION) << std::endl;
	std::cout << "Vendor: " << glGetString(GL_VENDOR) << std::endl;
	std::cout << "Renderer: " << glGetString(GL_RENDERER) << std::endl;
	std::cout << "ImGui version: " << IMGUI_VERSION << std::endl;
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
		srand(time(NULL));
		seed = rand();
		this->perlin = new siv::PerlinNoise(seed);
	} else {
		this->perlin = new siv::PerlinNoise(seed);
	}
	std::cout << "Perlin seed: " << seed << std::endl;
}

void	Engine::run()
{
	this->threads.resize(4);
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

		if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
			for (auto& chunk : this->chunks) {
				if (chunk.second.deleteVoxel(this->camera.getPosition(), this->camera.getFront())) {
					break;
				}
			}
		}
		if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS) {
			for (auto& chunk : this->chunks) {
				if (chunk.second.placeVoxel(this->camera.getPosition(), this->camera.getFront())) {
					break;
				}
			}
		}

		glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		this->render();
		this->renderer->drawSkybox(this->camera);

		this->textRenderer->renderText("FPS: " + std::to_string(this->fps), 10.0f, WINDOW_HEIGHT - 40.0f, 0.5f, glm::vec3(1.0f, 1.0f, 1.0f));
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
	ImGui::Text("X/Y/Z: (%.1f, %.1f, %.1f)", this->camera.getPosition().x, this->camera.getPosition().y, this->camera.getPosition().z);
	ImGui::Text("Speed: %.1f", this->camera.getMovementSpeed());

	ImGui::Checkbox("Wireframe", &this->wireframeMode);
	if (this->wireframeMode) {
		glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
	} else {
		glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	}
	ImGui::Checkbox("Chunk borders", &this->chunkBorders);
	ImGui::InputInt("Render distance", &this->renderDistance);
	ImGui::InputInt("Chunk loaded max", &this->chunkLoadedMax);

	ImGui::End();
	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void	Engine::render()
{
	this->visibleChunksCount = 0;
	this->visibleVoxelsCount = 0;

	this->chunkManagement();
	this->frustumCulling();

	int chunkLoaded = 0;
	for (auto& chunk : this->chunks) {
		if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::UNLOADED) {
			chunk.second.generateVoxel(this->perlin);
			chunkLoaded++;
		}
		if (chunkLoaded > this->chunkLoadedMax) {
			break;
		}
	}

	for (auto& chunk : this->chunks) {
		if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::GENERATED) {
			chunk.second.generateMesh(this->chunks);
			chunkLoaded++;
		}
		if (chunkLoaded >= this->chunkLoadedMax) {
			break;
		}
	}

	for (auto& chunk : this->chunks) {
		if (chunk.second.isVisible() && chunk.second.getState() == ChunkState::MESHED) {
			this->visibleVoxelsCount += this->renderer->draw(chunk.second, *this->shader, this->camera);
			this->visibleChunksCount++;
			if (chunkBorders)
				this->renderer->drawBoundingBox(chunk.second, this->camera);
		}
	}
}

void	Engine::chunkManagement()
{
	glm::vec3 cameraPos = this->camera.getPosition();
	glm::ivec3 cameraChunkPos3D = glm::ivec3(cameraPos.x / Chunk::SIZE, cameraPos.y / Chunk::HEIGHT, cameraPos.z / Chunk::SIZE);
	glm::ivec2 cameraChunkPos2D = glm::ivec2(cameraPos.x / Chunk::SIZE, cameraPos.z / Chunk::SIZE);
	int chunkRadius = (this->renderDistance / Chunk::SIZE) - 1;
	int squaredRenderDistance = this->renderDistance * this->renderDistance;
	int squaredChunkRadius = chunkRadius * chunkRadius;
	int	chunksLoadedThisFrame = 0;

	for (auto it = this->chunks.begin(); it != this->chunks.end();) {
		glm::ivec3 chunkPos3D = it->first;
		glm::vec2 diff = glm::ivec2(chunkPos3D.x, chunkPos3D.z) - cameraChunkPos2D;

		if (glm::dot(diff, diff) > squaredRenderDistance) {
			it = this->chunks.erase(it);
			chunksLoadedThisFrame++;
			if (chunksLoadedThisFrame >= this->chunkLoadedMax) {
				break;
			}
		} else {
			it++;
		}
	}


	chunksLoadedThisFrame = 0;
	for (int x = -chunkRadius; x <= chunkRadius; x++) {
		for (int z = -chunkRadius; z <= chunkRadius; z++) {
			glm::ivec2 chunkPos2D = cameraChunkPos2D + glm::ivec2(x, z);
			glm::ivec3 chunkPos3D = glm::ivec3(chunkPos2D.x, 0, chunkPos2D.y);
			glm::vec2 diff = chunkPos2D - cameraChunkPos2D;

			if (glm::dot(diff, diff) <= squaredChunkRadius && this->chunks.find(chunkPos3D) == this->chunks.end()) {
				this->chunks.insert(std::make_pair(chunkPos3D, Chunk(glm::vec3(chunkPos2D.x * Chunk::SIZE, 0, chunkPos2D.y * Chunk::SIZE))));
				chunksLoadedThisFrame++;
				if (chunksLoadedThisFrame >= this->chunkLoadedMax) {
					break;
				}
			}
		}
	}
}

void	Engine::frustumCulling()
{
	glm::mat4	clipMatrix = this->camera.getProjectionMatrix(WINDOW_WIDTH, WINDOW_HEIGHT, this->renderDistance) * this->camera.getViewMatrix();
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
		glm::vec3 center = chunk.second.getPosition() + glm::vec3(Chunk::SIZE, Chunk::HEIGHT, Chunk::SIZE) / 2.0f;
		glm::vec3 corner = chunk.second.getPosition() + glm::vec3(Chunk::SIZE, Chunk::HEIGHT, Chunk::SIZE);
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
