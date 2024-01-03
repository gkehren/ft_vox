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
	glfwSwapInterval(0); // Enable vsync
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
	this->boundingBoxShader = new Shader((path + "boundingBoxVertex.glsl").c_str(), (path + "boundingBoxFragment.glsl").c_str());
	this->renderer = new Renderer();
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
	delete this->boundingBoxShader;
	delete this->renderer;
	delete this->textRenderer;

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(this->window);
	glfwTerminate();
}

void	Engine::run()
{
	this->frustumDistance = 160.0f;
	this->renderDistance = 160;
	this->chunkX = 4;
	this->chunkZ = 4;
	this->generateChunks();

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

		glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		this->render();

		//this->textRenderer->renderText("FPS: " + std::to_string(this->fps), 25.0f, 25.0f, 1.0f, glm::vec3(1.0f, 1.0f, 1.0f));
		this->updateUI();
		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

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
	ImGui::Text("Visible chunks: %d (%ld)", this->visibleChunksCount, this->chunks.size());
	ImGui::Text("Voxel count: %d (%ld)",  this->visibleVoxelsCount, this->chunks.size() * Chunk::WIDTH * 1 * Chunk::DEPTH);
	ImGui::Text("X/Y/Z: (%.1f, %.1f, %.1f)", this->camera.getPosition().x, this->camera.getPosition().y, this->camera.getPosition().z);
	ImGui::Text("Speed: %.1f", this->camera.getMovementSpeed());
	ImGui::InputInt("Render distance", &this->renderDistance);

	ImGui::InputInt("Chunk X", &this->chunkX);
	ImGui::InputInt("Chunk Z", &this->chunkZ);

	if (ImGui::Button("Regenerate chunks")) {
		this->generateChunks();
	}

	ImGui::End();
}

void	Engine::generateChunks()
{
	this->chunks.clear();
	this->chunkPositions.clear();
	for (int x = 0; x < chunkX; x++) {
		for (int y = 0; y < WORLD_HEIGHT; y++) {
			for (int z = 0; z < chunkZ; z++) {
				this->chunkPositions.insert(glm::ivec2(x, z));
				this->chunks.push_back(Chunk(glm::vec3(x * Chunk::WIDTH, y * Chunk::HEIGHT, z * Chunk::DEPTH)));
			}
		}
	}
}

void	Engine::render()
{
	this->visibleChunksCount = 0;
	this->visibleVoxelsCount = 0;

	this->chunkManagement();
	this->frustumCulling();
}

void	Engine::chunkManagement()
{
	glm::vec3 cameraPos = this->camera.getPosition();
	glm::ivec2 cameraPos2D = glm::ivec2(cameraPos.x, cameraPos.z);
	glm::ivec2 cameraChunkPos2D = glm::ivec2(cameraPos.x / Chunk::FWIDTH, cameraPos.z / Chunk::FDEPTH);
	int chunkRadius = (this->renderDistance / Chunk::WIDTH) - 1;
	int squaredRenderDistance = this->renderDistance * this->renderDistance;
	int squaredChunkRadius = chunkRadius * chunkRadius;


	for (auto it = this->chunks.begin(); it != this->chunks.end();) {
		glm::ivec2 chunkPos2D = it->getPosition2D();
		glm::vec3 chunkPos = glm::vec3(chunkPos2D.x, 0, chunkPos2D.y);
		glm::vec2 diff = cameraPos2D - chunkPos2D;

		if (glm::dot(diff, diff) > squaredRenderDistance) {
			this->chunkPositions.erase(chunkPos2D / Chunk::WIDTH);
			std::swap(*it, this->chunks.back());
			this->chunks.pop_back();
		} else {
			it++;
		}
	}

	this->chunks.reserve(chunkRadius * chunkRadius);
	for (int x = -chunkRadius; x <= chunkRadius; x++) {
		for (int z = -chunkRadius; z <= chunkRadius; z++) {
			glm::ivec2 chunkPos2D = cameraChunkPos2D + glm::ivec2(x, z);
			glm::vec2 diff = chunkPos2D - cameraChunkPos2D;

			if (glm::dot(diff, diff) <= squaredChunkRadius && this->chunkPositions.find(chunkPos2D) == this->chunkPositions.end()) {
				this->chunkPositions.insert(chunkPos2D);
				this->chunks.push_back(Chunk(glm::vec3(chunkPos2D.x * Chunk::WIDTH, 0, chunkPos2D.y * Chunk::DEPTH)));
			}
		}
	}
}

void	Engine::frustumCulling()
{
	glm::mat4	clipMatrix = this->camera.getProjectionMatrix(WINDOW_WIDTH, WINDOW_HEIGHT, this->frustumDistance) * this->camera.getViewMatrix();
	std::array<glm::vec4, 6>	frustumPlanes;

	frustumPlanes[0] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 0); // Gauche
	frustumPlanes[1] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 0); // Droite
	frustumPlanes[2] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 1); // Bas
	frustumPlanes[3] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 1); // Haut
	frustumPlanes[4] = glm::row(clipMatrix, 3) + glm::row(clipMatrix, 2); // Proche
	frustumPlanes[5] = glm::row(clipMatrix, 3) - glm::row(clipMatrix, 2); // Lointain

	for (auto& plane : frustumPlanes) {
		plane /= glm::length(glm::vec3(plane));
	}

	for (auto& chunk : this->chunks) {
		glm::vec3 center = chunk.getPosition() + glm::vec3(Chunk::WIDTH, Chunk::HEIGHT, Chunk::DEPTH) / 2.0f;
		float radius = chunk.getRadius();

		bool inside = true;
		for (const auto& plane : frustumPlanes) {
			float distance = glm::dot(glm::vec3(plane), center) + plane.w;
			if (distance < -radius) {
				inside = false;
				break;
			}
		}

		if (inside) {
			chunk.generate();
			this->renderer->draw(chunk, *this->shader, this->camera);
			this->visibleChunksCount++;
			this->visibleVoxelsCount += chunk.getVoxels().size();
		}
	}
}
