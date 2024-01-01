#include "Engine.hpp"

Engine::Engine()
{
	if (!glfwInit()) {
		throw std::runtime_error("Failed to initialize GLFW");
	}

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

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

	std::string path = BASE_PATH;
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

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(this->window);
	glfwTerminate();
}

void	Engine::run()
{
	this->frustumDistance = 160.0f;
	this->chunkRadius = 2;
	this->chunkX = 10;
	this->chunkZ = 10;
	this->generateChunks();

	while (!glfwWindowShouldClose(this->window)) {
		float currentFrame = glfwGetTime();
		this->deltaTime = currentFrame - lastFrame;
		this->lastFrame = currentFrame;

		this->updateUI();
		this->camera.processKeyboard(deltaTime);

		glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		this->render();

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
	ImGui::Text("Chunk count: %d (%lu)", this->visibleChunksCount, this->chunkPositions.size());
	ImGui::Text("Voxel count: %d (%.1f)",  this->visibleVoxelsCount, this->chunks.size() * Chunk::WIDTH * 1 * Chunk::DEPTH);
	ImGui::Text("Camera position: (%.1f, %.1f, %.1f)", this->camera.getPosition().x, this->camera.getPosition().y, this->camera.getPosition().z);
	ImGui::Text("Camera speed: %.1f", this->camera.getMovementSpeed());
	ImGui::InputInt("Chunk radius", &this->chunkRadius);

	ImGui::InputInt("Chunk X", &this->chunkX);
	ImGui::InputInt("Chunk Z", &this->chunkZ);

	if (ImGui::Button("Regenerate chunks")) {
		this->generateChunks();
	}

	ImGui::End();
}

void	Engine::generateChunks()
{
	// FINAL CODE
	//for (int x = 0; x < WORLD_SIZE; x++) {
	//	for (int y = 0; y < WORLD_HEIGHT; y++) {
	//		for (int z = 0; z < WORLD_SIZE; z++) {
	//			chunkPositions.insert(glm::ivec3(x, y, z));
	//		}
	//	}
	//}

	// TEST CODE
	for (int x = 0; x < chunkX; x++) {
		for (int y = 0; y < WORLD_HEIGHT; y++) {
			for (int z = 0; z < chunkZ; z++) {
				chunkPositions.insert(glm::ivec3(x, y, z));
			}
		}
	}
}

void	Engine::render()
{
	this->visibleChunksCount = 0;
	this->visibleVoxelsCount = 0;

	this->chunkManagement();

	for (auto& chunk : this->chunks) {
		for (auto& voxel : chunk.getVoxels()) {
			this->renderer->draw(voxel, *this->shader, this->camera);
			this->visibleVoxelsCount++;
		}
		this->renderer->drawBoundingBox(chunk, *this->boundingBoxShader, this->camera);
		this->visibleChunksCount++;
	}

	//this->frustumCulling();
}

void	Engine::chunkManagement()
{
	glm::ivec3	cameraGridPos = glm::ivec3(this->camera.getPosition() / Chunk::WIDTH);

	this->chunks.clear();
	for (int x = -chunkRadius; x <= chunkRadius; x++) {
		for (int y = -chunkRadius; y <= chunkRadius; y++) {
			for (int z = -chunkRadius; z <= chunkRadius; z++) {
				glm::ivec3 gridPos = cameraGridPos + glm::ivec3(x, y, z);
				if (chunkPositions.count(gridPos)) {
					this->chunks.push_back(Chunk(glm::vec3(gridPos) * Chunk::WIDTH));
				}
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
			for (const auto& voxel : chunk.getVoxels()) {
				this->renderer->draw(voxel, *this->shader, this->camera);
				this->visibleVoxelsCount++;
			}
			this->visibleChunksCount++;
		}
	}
}

//void	Engine::occlusionCulling(std::vector<Chunk>& visibleChunks)
//{
//	for (auto& chunk : visibleChunks) {
//		for (auto& voxel : chunk.getVoxelsSorted(this->camera.getPosition())) {
//			GLuint query;
//			glGenQueries(1, &query);

//			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
//			glDepthMask(GL_FALSE);

//			glBeginQuery(GL_SAMPLES_PASSED, query);
//			this->renderer->drawBoundingBox(voxel, *this->boundingBoxShader, this->camera);
//			glEndQuery(GL_SAMPLES_PASSED);

//			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
//			glDepthMask(GL_TRUE);

//			GLuint samples;
//			glGetQueryObjectuiv(query, GL_QUERY_RESULT, &samples);

//			if (samples > 0) {
//				this->renderer->draw(voxel, *this->shader, this->camera);
//				this->visibleVoxelsCount++;
//			}

//			glDeleteQueries(1, &query);
//		}
//	}
//}
