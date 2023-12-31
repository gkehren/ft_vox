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
	glfwSwapInterval(1); // Enable vsync
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

	this->shader = new Shader(VERTEX_PATH, FRAGMENT_PATH);
	this->boundingBoxShader = new Shader("/home/gkehren/Documents/ft_vox/ressources/boundingBoxVertex.glsl", "/home/gkehren/Documents/ft_vox/ressources/boundingBoxFragment.glsl");
	this->renderer = new Renderer();
	this->camera.setWindow(this->window);

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
	this->width = 1;
	this->height = 1;
	this->depth = 1;

	for (int x = 0; x < this->width; x++) {
		for (int y = 0; y < this->height; y++) {
			for (int z = 0; z < this->depth; z++) {
				this->chunks.push_back(Chunk(glm::vec3(x * Chunk::WIDTH, y * Chunk::HEIGHT - (Chunk::HEIGHT / 2), z * Chunk::DEPTH)));
			}
		}
	}
	this->generateChunks();

	while (!glfwWindowShouldClose(this->window)) {
		float currentFrame = glfwGetTime();
		this->deltaTime = currentFrame - lastFrame;
		this->lastFrame = currentFrame;

		this->updateUI();
		this->camera.processKeyboard(deltaTime);

		glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		this->cullChunks();

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

	ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
	ImGui::Text("Chunk count: %d (%lu)", this->visibleChunksCount, this->chunks.size());
	ImGui::Text("Voxel count: %d (%lu)",  this->visibleChunksCount * Chunk::WIDTH * Chunk::HEIGHT * Chunk::DEPTH, this->chunks.size() * Chunk::WIDTH * Chunk::HEIGHT * Chunk::DEPTH);
	ImGui::Text("Camera position: (%.1f, %.1f, %.1f)", this->camera.getPosition().x, this->camera.getPosition().y, this->camera.getPosition().z);
	ImGui::Text("Camera speed: %.1f", this->camera.getMovementSpeed());
	ImGui::InputFloat("Frustum distance", &this->frustumDistance);

	ImGui::InputInt("Width", &this->width);
	ImGui::InputInt("Height", &this->height);
	ImGui::InputInt("Depth", &this->depth);

	if (ImGui::Button("Generate")) {
		this->chunks.clear();
		for (int x = 0; x < this->width; x++) {
			for (int y = 0; y < this->height; y++) {
				for (int z = 0; z < this->depth; z++) {
					this->chunks.push_back(Chunk(glm::vec3(x * Chunk::WIDTH, y * Chunk::HEIGHT, z * Chunk::DEPTH)));
				}
			}
		}
		this->generateChunks();
	}

	ImGui::End();
}

void	Engine::generateChunks()
{
	for (auto& chunk : this->chunks) {
		chunk.generate();
	}
}

void	Engine::cullChunks()
{
	this->visibleChunksCount = 0;
	std::sort(this->chunks.begin(), this->chunks.end(), [this](const Chunk& a, const Chunk& b) {
		float distA = glm::distance(this->camera.getPosition(), a.getPosition());
		float distB = glm::distance(this->camera.getPosition(), b.getPosition());
		return distA < distB;
	});
	this->occlusionCulling();
}

void	Engine::frustumCulling(std::vector<Chunk>& visibleChunks)
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

	for (const auto& chunk : this->chunks) {
		glm::vec3 center = chunk.getPosition();
		float radius = glm::length(glm::vec3(Chunk::HEIGHT / 2.0f));

		bool inside = true;
		for (const auto& plane : frustumPlanes) {
			float distance = glm::dot(glm::vec3(plane), center) + plane.w;
			if (distance < -radius) {
				inside = false;
				break;
			}
		}

		if (inside) {
			visibleChunks.push_back(chunk);
		}
	}
}

void	Engine::occlusionCulling()
{
	for (auto& chunk : this->chunks) {
		GLuint query;
		glGenQueries(1, &query);

		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glDepthMask(GL_FALSE);

		glBeginQuery(GL_SAMPLES_PASSED, query);
		this->renderer->drawBoundingBox(chunk, *this->boundingBoxShader, this->camera);
		glEndQuery(GL_SAMPLES_PASSED);

		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDepthMask(GL_TRUE);

		GLuint samples;
		glGetQueryObjectuiv(query, GL_QUERY_RESULT, &samples);

		if (samples > 0) {
			this->renderer->draw(chunk, *this->shader, this->camera);
			this->visibleChunksCount++;
		}

		glDeleteQueries(1, &query);
	}
}
