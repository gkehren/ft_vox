#include <Engine/Engine.hpp>
#include <Engine/EngineMultiplayer.hpp>
#include <Engine/GameUiSample.hpp>
#include <Engine/Profiler.hpp>
#include <Renderer/ColorSpace.hpp>
#include <Vulkan/ImageBarrier.hpp>
#include <imgui/imgui_internal.h>

#include "TestMain.hpp"
#include "VisualImage.hpp"

#include <World/WorldCheckpoint.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{
constexpr VkColorSpaceKHR kColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
constexpr VkClearColorValue kSky{{0.38f, 0.58f, 0.92f, 1.f}};
constexpr glm::vec4 kBlack{0.f, 0.f, 0.f, 1.f};
constexpr glm::vec4 kHalfWhite{1.f, 1.f, 1.f, 0.5f};

void checked(bool ok, const char *message)
{
	if (!ok)
		throw std::runtime_error(message);
}

std::string readText(const std::filesystem::path &path)
{
	std::ifstream in(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Intercept only vkCreateSwapchainKHR. The usual retire, rollback and WSI
// recovery paths still run on the real device.
struct SwapchainCreationFailure
{
	static inline VkResult error{};
	static inline int calls{};
	PFN_vkCreateSwapchainKHR original = vkCreateSwapchainKHR;

	explicit SwapchainCreationFailure(VkResult result)
	{
		error = result;
		calls = 0;
		vkCreateSwapchainKHR = reject;
	}
	~SwapchainCreationFailure() { vkCreateSwapchainKHR = original; }

	static VKAPI_ATTR VkResult VKAPI_CALL reject(VkDevice, const VkSwapchainCreateInfoKHR *,
		const VkAllocationCallbacks *, VkSwapchainKHR *)
	{
		++calls;
		return error;
	}
};

struct Readback
{
	VkContext &context;
	AllocatedImage output{};
	AllocatedBuffer buffer{};

	~Readback()
	{
		destroyBuffer(context.getAllocator(), buffer);
		destroyImage(context.getAllocator(), context.getDevice(), output);
	}

	visual::RgbaImage pixels() const
	{
		const auto *data = static_cast<const uint8_t *>(buffer.info.pMappedData);
		visual::RgbaImage image{output.width, output.height,
			std::vector<uint8_t>(data, data + size_t(output.width) * output.height * 4)};
		if (output.format == VK_FORMAT_B8G8R8A8_SRGB || output.format == VK_FORMAT_B8G8R8A8_UNORM)
			for (size_t i = 0; i < image.pixels.size(); i += 4)
				std::swap(image.pixels[i], image.pixels[i + 2]);
		return image;
	}
};

uint8_t channel(const visual::RgbaImage &image, uint32_t x, uint32_t y, uint32_t c)
{
	return image.pixels[(size_t(y) * image.width + x) * 4 + c];
}

bool matchesSrgb(uint8_t actual, float linear)
{
	return std::abs(int(actual) - int(std::lround(colorspace::linearToSrgb(linear) * 255.f))) <= 1;
}

float percentile(std::vector<float> values, float fraction)
{
	std::sort(values.begin(), values.end());
	return values[static_cast<size_t>(fraction * (values.size() - 1))];
}
} // namespace

struct GameUiBackendProbe
{
	explicit GameUiBackendProbe(Engine &host) : engine(host) {}
	Engine &engine;

	void init()
	{
		ImGui::GetIO().IniFilename = nullptr;
		engine.renderSettings.minRenderDistance = 64;
		engine.renderSettings.maxRenderDistance = 128;
		engine.worldRenderer->postSettings().applyPreset(GraphicsQualityPreset::Low);
		engine.worldRenderer->postSettings().autoExposureEnabled = false;
		engine.initializeNoiseGenerator(42);
		engine.stepDerivedProjectionForTests(1.0 / 60.0);
		engine.enableGameUi();
		windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
	}

	visual::RgbaImage capture(Readback &target, gameui::Renderer *ui,
							  const std::function<void(VkCommandBuffer)> &devMarker = {})
	{
		const VkExtent2D extent{target.output.width, target.output.height};
		engine.updateFrameUniforms(0, int(extent.width), int(extent.height), 5.f);
		engine.immediate->submitAndWait([&](VkCommandBuffer cmd) {
			engine.worldRenderer->recordFrameToImage(cmd, 0, target.output.image, target.output.view,
				extent, engine.drawList, engine.shadowList, kSky, {}, nullptr, ui, devMarker);
			vkbar::cmdTransitionColor(cmd, target.output.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT);
			VkBufferImageCopy copy{};
			copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			copy.imageExtent = {extent.width, extent.height, 1};
			vkCmdCopyImageToBuffer(cmd, target.output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				target.buffer.buffer, 1, &copy);
		});
		checked(vmaInvalidateAllocation(engine.vkContext->getAllocator(), target.buffer.allocation, 0,
			VK_WHOLE_SIZE) == VK_SUCCESS, "Game UI screenshot invalidate");
		return target.pixels();
	}

	void sessionMenuProof(const std::filesystem::path &out)
	{
		using enum gameui::SessionAction;
		using gameui::Screen;
		using namespace std::chrono_literals;
		ImGui::GetIO().IniFilename = nullptr;
		engine.setShowDeveloperUi(false);
		engine.setSavesRootForTests(out / "saves");
		engine.renderSettings.maxRenderDistance = 128;
		engine.worldRenderer->postSettings().applyPreset(GraphicsQualityPreset::Low);
		engine.enableGameUi(false, false);
		engine.running = true;
		engine.showMainMenu();
		auto &sample = *engine.m_playerUi;
		auto &device = *engine.vkContext;
		const auto errors = device.validationErrorCount();
		const auto rtssBefore = device.rtssValidationErrorCount();
		constexpr VkExtent2D extent{960, 540};
		resize(int(extent.width), int(extent.height));
		windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
		present();
		const auto clickControl = [&](std::string_view label) {
			for (const auto &widget : sample.m_widgets.widgets())
				if (widget.label == label)
				{
					const auto rect = sample.m_widgets.bounds(widget).rect;
					clickAt({(rect.x + rect.z) * 0.5f, (rect.y + rect.w) * 0.5f});
					return;
				}
			throw std::runtime_error("missing native control: " + std::string(label));
		};
		tap(SDLK_TAB);
		tap(SDLK_TAB);
		tap(SDLK_RETURN);
		engine.handleEvents();
		engine.pumpPlayerSession();
		present();
		CHECK(sample.screens().top() == Screen::Settings && !engine.viewWorld() && !engine.developerWorkspaceVisible(),
			"keyboard opens native Settings without a world or the developer workspace");
		tap(SDLK_ESCAPE);
		present();
		CHECK(sample.screens().top() == Screen::MainMenu, "one Escape returns to the main menu");

		clickControl("Multiplayer");
		engine.handleEvents();
		engine.pumpPlayerSession();
		present();
		GameUIFrame join;
		engine.populateJoinUi(join);
		CHECK(sample.screens().top() == Screen::Multiplayer && !engine.viewWorld() &&
			join.connectServer && join.startHostedServer && join.hostedServer,
			"pointer opens the existing Host and Join controls without a simulation");
		join.connectServer({"invalid-endpoint", "", {}});
		engine.applySessionRequests();
		CHECK(!engine.m_joinError.empty() && !engine.viewWorld(), "Join validation works from the empty menu");
		engine.pumpPlayerMultiplayer();
		present();
		CHECK(engine.suppressDeveloperUi(), "native multiplayer works independently of the developer workspace");
		clickControl("Join server");
		engine.handleEvents();
		engine.pumpPlayerMultiplayer();
		present();
		CHECK(sample.screens().top() == Screen::JoinServer, "multiplayer opens native Join");
		tap(SDLK_ESCAPE);
		present();
		tap(SDLK_ESCAPE);
		present();
		CHECK(sample.screens().top() == Screen::MainMenu, "Back restores the previous screen");

		constexpr auto format = VK_FORMAT_R8G8B8A8_SRGB;
		engine.worldRenderer->resizeOutput(extent, format, kColorSpace);
		sample.renderer().setOutput(format, kColorSpace);
		Readback target{device};
		const auto allocator = device.getAllocator();
		target.output = createImage2D(allocator, device.getDevice(), extent.width, extent.height,
			format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		target.buffer = createBuffer(allocator, extent.width * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
		const auto shot = [&](const char *name) {
			sample.prepare(engine.window, 0, extent, {}, engine.worldRenderer->getTextureManager(), engine.m_atlasVersion);
			const auto image = capture(target, &sample.renderer());
			checked(visual::writePng((out / name).string(), image), "write session screenshot");
		};
		const auto request = [&](gameui::SessionAction action, std::string name = {}, std::string seed = {}) {
			sample.m_sessionRequest = gameui::SessionRequest{action, std::move(name), std::move(seed)};
			engine.pumpPlayerSession();
		};
		const auto until = [&](auto done) {
			const auto deadline = std::chrono::steady_clock::now() + 15s;
			while (!done() && std::chrono::steady_clock::now() < deadline)
			{
				engine.pumpPlayerSession();
				SDL_PumpEvents();
				SDL_Delay(2);
			}
			checked(done(), "session menu transition timed out");
		};
		const auto complete = [&] {
			engine.noteSessionFramePresented();
			until([&] { return !engine.m_playerSessionRequest; });
		};

		shot("launch.png");
		CHECK(!engine.viewWorld() && !engine.renderBridge && !std::filesystem::exists(out / "saves"),
			"launch has no active world or saves folder");
		request(Save);
		request(Leave);
		request(ForceStop);
		request(ConfirmFailedQuit);
		CHECK(!engine.m_playerSessionRequest && engine.running && sample.screens().top() == Screen::MainMenu,
			"stale requests are ignored without modifying the empty session");
		clickControl("Local worlds");
		engine.handleEvents();
		engine.pumpPlayerSession();
		until([&] { return !engine.m_worldCatalog.valid(); });
		sample.openScreen(Screen::CreateWorld);
		request(Create, "../escape", "42");
		complete();
		CHECK(sample.screens().top() == Screen::SessionError, "unsafe world name is rejected");
		request(Cancel);
		request(Create, "menu-proof", "wrong");
		complete();
		CHECK(sample.screens().top() == Screen::SessionError, "invalid seed is visible");
		request(Cancel);
		engine.m_beforeLocalWorldInitializeForTests = [](const auto &) {
			throw std::runtime_error("test failure before persistence initialization");
		};
		request(Create, "menu-proof", "42");
		shot("loading.png");
		request(Create, "duplicate", "43");
		complete();
		CHECK(sample.screens().top() == Screen::SessionError && engine.acceptsPlayerSession(Retry) &&
			!std::filesystem::exists(out / "saves" / "menu-proof"), "failure releases the empty reservation");
		engine.m_beforeLocalWorldInitializeForTests = {};
		request(Retry);
		complete();
		CHECK(engine.sim && engine.sim->isWorldOpen() && engine.worldSeed() == 42 &&
			!std::filesystem::exists(out / "saves" / "duplicate"), "create publishes one persistent world");
		auto *const authority = engine.sim.get();
		auto *const projection = engine.renderBridge.get();
		request(Create, "unexpected", "43");
		request(Open, "menu-proof");
		CHECK(engine.sim.get() == authority && engine.renderBridge.get() == projection &&
			!engine.m_playerSessionRequest, "invalid local start preserves the active authority and projection");
		engine.tickStreaming(0.);
		sample.openPause();
		engine.syncUiInput();
		engine.pumpPlayerSession();
		shot("pause.png");
		CHECK(engine.simulationSuspended() && !engine.mouseCaptured, "native pause suspends solo steps");
		sample.back();
		engine.syncUiInput();
		CHECK(!engine.nativePause(), "resume releases native suspension");
		sample.openPause();
		engine.sim->world()->worldPersistence()->setCheckpointFaultForTests(worldsave::CheckpointFaultPoint::BeforeStaging);
		request(Leave);
		complete();
		CHECK(sample.screens().top() == Screen::SessionError && engine.sim && engine.sim->isWorldOpen(),
			"checkpoint failure refuses leave and retains the authoritative world");
		shot("recoverable-error.png");
		engine.sim->world()->worldPersistence()->setCheckpointFaultForTests(worldsave::CheckpointFaultPoint::None);
		request(Retry);
		complete();
		CHECK(!engine.sim && !engine.renderBridge && sample.screens().top() == Screen::MainMenu,
			"retry closes the saved session and returns to the menu");
		sample.openScreen(Screen::Worlds);
		request(List);
		until([&] { return !engine.m_worldCatalog.valid(); });
		CHECK(sample.m_worldCatalog.names == std::vector<std::string>{"menu-proof"}, "catalog lists the saved world");
		shot("world-selection.png");
		request(Create, "menu-proof", "43");
		complete();
		int storedSeed = 0;
		std::string seedError;
		CHECK(sample.screens().top() == Screen::SessionError &&
			WorldPersistence::peekStoredSeed(out / "saves", "menu-proof", storedSeed, seedError) && storedSeed == 42,
			"an existing name cannot replace the stored world identity");
		request(Cancel);
		shot("world-selection.png");
		const auto worldPath = out / "saves" / "menu-proof";
		std::filesystem::rename(worldPath, out / "moved-world");
		clickControl("Open selected world");
		engine.handleEvents();
		engine.pumpPlayerSession();
		complete();
		CHECK(sample.screens().top() == Screen::SessionError && !std::filesystem::exists(worldPath),
			"a removed catalog entry produces an error without recreating the save");
		std::filesystem::rename(out / "moved-world", worldPath);
		request(Cancel);
		request(Open, "menu-proof");
		complete();
		CHECK(engine.sim && engine.sim->isWorldOpen() && engine.worldSeed() == 42, "reopen uses persisted metadata");
		sample.openPause();

		const auto beginCheckpointAndClose = [&] {
			request(Save);
			engine.noteSessionFramePresented();
			engine.pumpPlayerSession();
			checked(engine.m_soloWork.valid() && !engine.viewWorld() && !engine.renderBridge,
				"the worker exclusively owns the world during checkpointing");
			windowEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
			windowEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
			engine.handleEvents();
			CHECK(engine.m_deferredQuit && engine.m_playerSessionRequest && engine.m_playerSessionRequest->action == Save,
				"two window-close events preserve a single quit while checkpointing");
		};
		engine.sim->world()->worldPersistence()->setCheckpointFaultForTests(worldsave::CheckpointFaultPoint::BeforeStaging);
		beginCheckpointAndClose();
		complete();
		engine.pumpPlayerSession();
		CHECK(engine.running && engine.sim && engine.m_deferredQuit && sample.screens().top() == Screen::SessionError,
			"deferred quit stops at checkpoint failure");
		request(Cancel);
		CHECK(engine.running && !engine.m_deferredQuit && sample.screens().top() == Screen::Pause,
			"Back clears the pending quit and returns to Pause");
		beginCheckpointAndClose();
		complete();
		engine.sim->world()->worldPersistence()->setCheckpointFaultForTests(worldsave::CheckpointFaultPoint::None);
		request(Retry);
		complete();
		CHECK(engine.running && engine.sim && sample.screens().top() == Screen::Pause, "checkpoint retry completes before deferred quit starts");
		engine.pumpPlayerSession();
		CHECK(engine.m_playerSessionRequest && engine.m_playerSessionRequest->action == Quit,
			"successful checkpoint releases deferred quit");
		complete();
		CHECK(!engine.running && !engine.viewWorld(), "deferred quit closes the saved world");

		engine.running = true;
		engine.showMainMenu();
		engine.m_beforeLocalWorldInitializeForTests = [](const auto &path) {
			std::ofstream(path / "recoverable-data") << "keep";
			throw std::runtime_error("test failure after creating save data");
		};
		request(Create, "partial-save", "42");
		complete();
		CHECK(sample.screens().top() == Screen::SessionError && !engine.acceptsPlayerSession(Retry) &&
			std::filesystem::exists(out / "saves" / "partial-save" / "recoverable-data"),
			"creation preserves nonempty reservations and directs the player to other recovery");
		request(Cancel);
		engine.m_beforeLocalWorldInitializeForTests = {};
		request(Create, "close-on-load", "42");
		windowEvent(SDL_EVENT_WINDOW_MINIMIZED);
		windowEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
		engine.handleEvents();
		engine.pumpPlayerSession();
		CHECK(engine.m_windowMinimized && engine.m_soloWork.valid() && engine.m_deferredQuit && !engine.viewWorld(),
			"minimize lets loading advance without a present and window close is deferred");
		windowEvent(SDL_EVENT_WINDOW_RESTORED);
		engine.handleEvents();
		complete();
		CHECK(engine.running && engine.viewWorld(), "loading publishes its world before deferred quit");
		engine.pumpPlayerSession();
		complete();
		CHECK(!engine.running && !engine.viewWorld() && WorldPersistence::worldExists(out / "saves", "close-on-load"),
			"quit after loading saves the new world");
		const auto rtssErrors = device.rtssValidationErrorCount() - rtssBefore;
		const auto validationErrors = device.validationErrorCount() - errors;
		CHECK(validationErrors == rtssErrors, "session proof has no new application Vulkan validation errors");
		std::cout << "Session menu validation errors=" << validationErrors << " known_rtss_errors=" << rtssErrors << '\n';
	}


	void multiplayerMenuProof(const std::filesystem::path &evidence, const std::filesystem::path &server)
	{
		namespace fs = std::filesystem;
		using gameui::Screen;
		using namespace std::chrono_literals;
		const bool host = !server.empty();
		ImGui::GetIO().IniFilename = nullptr;
		engine.setShowDeveloperUi(false);
		engine.loadJoinPreferences();
		engine.loadHosting();
		if (host)
		{
			engine.m_hostExecutable = server;
			engine.m_hostSavesRoot = engine.m_savesRoot / "dedicated";
		}
		engine.renderSettings.maxRenderDistance = 128;
		engine.worldRenderer->postSettings().applyPreset(GraphicsQualityPreset::Low);
		engine.enableGameUi(false, false);
		engine.running = true;
		engine.showMainMenu();
		constexpr VkExtent2D size{960, 540};
		resize(int(size.width), int(size.height));
		auto &sample = *engine.m_playerUi;
		auto &device = *engine.vkContext;
		const auto errors = device.validationErrorCount() - device.rtssValidationErrorCount();
		const auto frame = [&] {
			windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
			engine.handleEvents();
			engine.tickHosting();
			engine.pumpPlayerSession();
			engine.pumpPlayerMultiplayer();
			engine.applySessionRequests();
			if (engine.m_remote)
			{
				engine.pollRemote();
				engine.processInput(1. / 60.);
				engine.tickStreaming(1. / 60.);
				engine.updateRemoteEntities();
			}
			present();
		};
		const auto until = [&](auto done, const char *error) {
			const auto deadline = std::chrono::steady_clock::now() + 25s;
			while (!done() && std::chrono::steady_clock::now() < deadline)
			{
				frame();
				SDL_Delay(4);
			}
			checked(done(), error);
		};
		const auto widget = [&](std::string_view label) -> const gameui::Widget & {
			const auto widgets = sample.m_widgets.widgets();
			const auto found = std::ranges::find(widgets, label, &gameui::Widget::label);
			if (found == widgets.end())
				throw std::runtime_error("missing multiplayer control: " + std::string(label) +
					" (screen " + std::to_string(int(sample.screens().top())) + ")");
			return *found;
		};
		const auto press = [&](std::string_view label, int row = -1) {
			if (!widget(label).enabled)
				throw std::runtime_error("disabled multiplayer control: " + std::string(label));
			for (size_t i = 0; i < gameui::WidgetSet::kMaxWidgets &&
				sample.m_widgets.bounds(widget(label)).intersect(sample.m_widgets.viewport().clip).empty(); ++i)
			{
				tap(SDLK_TAB);
				frame();
			}
			const auto &control = widget(label);
			auto bounds = sample.m_widgets.bounds(control);
			if (row >= 0)
			{
				const auto *list = std::get<gameui::ListBox *>(control.control);
				bounds = gameui::listRowRect(gameui::listGeometry(bounds, *list,
					gameui::style::baseline(), sample.m_screenScale), row);
			}
			const auto clipped = bounds.intersect(sample.m_widgets.viewport().clip);
			if (clipped.empty())
				throw std::runtime_error("multiplayer control outside the native viewport: " + std::string(label));
			const auto r = clipped.rect;
			button(SDL_EVENT_MOUSE_BUTTON_DOWN, (r.x + r.z) * .5f, (r.y + r.w) * .5f);
			button(SDL_EVENT_MOUSE_BUTTON_UP, (r.x + r.z) * .5f, (r.y + r.w) * .5f);
			frame();
		};
		const auto type = [&](std::string_view label, const std::string &value) {
			press(label);
			tap(SDLK_A, SDL_KMOD_CTRL);
			commitText(value.c_str());
			frame();
		};
		const auto publish = [&](const char *name, std::string_view value) {
			const auto path = evidence / name;
			auto pending = path;
			pending += ".pending";
			{
				std::ofstream output(pending);
				output << value;
				checked(bool(output), "write multiplayer proof rendezvous");
			}
			fs::rename(pending, path);
		};
		const auto shot = [&](const char *name, VkExtent2D extent = {}) {
			if (extent.width == 0u || extent.height == 0u)
				extent = size;
			device.waitIdle();
			constexpr auto format = VK_FORMAT_R8G8B8A8_SRGB;
			auto &world = *engine.worldRenderer;
			world.resizeOutput(extent, format, kColorSpace);
			sample.renderer().setOutput(format, kColorSpace);
			Readback target{device};
			const auto allocator = device.getAllocator();
			target.output = createImage2D(allocator, device.getDevice(), extent.width, extent.height, format,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			target.buffer = createBuffer(allocator, extent.width * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
			sample.prepare(engine.window, 0, extent, {}, world.getTextureManager(), engine.m_atlasVersion);
			checked(visual::writePng((evidence / name).string(), capture(target, &sample.renderer())),
				"write multiplayer proof screenshot");
			world.resizeOutput(engine.swapchain->getExtent(), engine.swapchain->getImageFormat(), engine.swapchain->getColorSpace());
			sample.renderer().setOutput(engine.swapchain->getImageFormat(), engine.swapchain->getColorSpace());
		};

		frame();
		press("Multiplayer");
		press(host ? "Host server" : "Join server");
		if (!host)
		{
			CHECK(sample.m_joinEndpoint.value() == "127.0.0.1:7777",
				"the join draft starts at the automatic default (F10)");
			// UI scale sweep (manual validation item): the native multiplayer
			// screens stay reachable at 100%, 150% and 200% on a large
			// window, with screenshots as evidence.
			resize(1920, 1080);
			for (const auto &[scale, name] : std::array<std::pair<float, const char *>, 3>{
				{{1.f, "join-scale-100.png"}, {1.5f, "join-scale-150.png"}, {2.f, "join-scale-200.png"}}})
			{
				sample.setScalePolicy({gameui::ScaleMode::Fixed, scale});
				frame();
				frame();
				// The scaled content can exceed the panel viewport; the
				// primary action only has to stay reachable through keyboard
				// navigation, which scrolls it into view.
				// Reachable, not enabled: no profile is selected yet at
				// this point in the flow, so the primary action is
				// legitimately disabled. The sweep verifies that the
				// scaled layout never makes the control unreachable.
				const auto connectVisible = [&] {
					const auto widgets = sample.m_widgets.widgets();
					const auto found = std::ranges::find(widgets, "Connect", &gameui::Widget::label);
					return found != widgets.end() &&
						!sample.m_widgets.bounds(*found).intersect(sample.m_widgets.viewport().clip).empty();
				};
				for (size_t i = 0; i < gameui::WidgetSet::kMaxWidgets && !connectVisible(); ++i)
				{
					tap(SDLK_TAB);
					frame();
				}
				CHECK(connectVisible(), "the Connect action stays reachable across UI scales (F05)");
				shot(name, VkExtent2D{1920, 1080});
			}
			sample.setScalePolicy({gameui::ScaleMode::Auto, 1.f});
			frame();
			resize(960, 540);
			frame();
		}
		press("Profiles");
		type("New profile name", host ? "owner-native" : "guest-native");
		press("Create profile");
		CHECK(engine.m_joinPreferences.selectedProfile == (host ? "owner-native" : "guest-native"),
			"native profile creation selects the saved identity");
		if (!host)
		{
			// Correction F17 regression: a shared multiplayer list keeps the
			// wheel's scroll offset across screen reconstructions, and a row
			// picked outside the initial window drives its action.
			for (const char *extra : {"profile-a", "profile-b", "profile-c", "profile-d", "profile-e"})
			{
				type("New profile name", extra);
				press("Create profile");
			}
			until([&] { return sample.m_multiplayer.profiles.size() >= 6; },
				"the extra profiles reached the native list");
			CHECK(sample.m_mpList.selection.first == 0, "the profiles list starts unscrolled");
			const auto widgets = sample.m_widgets.widgets();
			const auto list = std::ranges::find(widgets, "Saved profiles", &gameui::Widget::label);
			checked(list != widgets.end(), "the profiles list is on screen");
			const auto bounds = sample.m_widgets.bounds(*list);
			const auto wheel = [&](float y) {
				SDL_Event event{};
				event.type = SDL_EVENT_MOUSE_WHEEL;
				event.wheel.windowID = SDL_GetWindowID(engine.window);
				event.wheel.y = y;
				event.wheel.mouse_x = (bounds.rect.x + bounds.rect.z) * .5f;
				event.wheel.mouse_y = (bounds.rect.y + bounds.rect.w) * .5f;
				checked(SDL_PushEvent(&event), "queue the profiles wheel");
				frame();
			};
			wheel(-1.f);
			wheel(-1.f);
			CHECK(sample.m_mpList.selection.first == 1, "mouse-wheel scrolling moves the profiles list");
			for (int i = 0; i < 3; ++i)
				frame();
			// The regression: the per-frame screen reconstruction must not
			// snap the offset back to the selected row.
			CHECK(sample.m_mpList.selection.first == 1,
				"the scroll offset survives screen reconstruction (F17)");
			press("Saved profiles", 4);
			const auto picked = std::string(
				sample.m_mpLabels[static_cast<size_t>(sample.m_mpList.selection.selected)]);
			press("Use selected profile");
			CHECK(engine.m_joinPreferences.selectedProfile == picked && picked != "guest-native",
				"a row outside the initial window drives the profile action (F17)");
			wheel(1.f);
			wheel(1.f);
			press("Saved profiles", 0);
			press("Use selected profile");
			CHECK(engine.m_joinPreferences.selectedProfile == "guest-native",
				"the original identity is restored for the rest of the flow");
		}
		tap(SDLK_ESCAPE);
		frame();

		if (host)
		{
			type("New or existing server world", "native-menu-proof");
			type("Port (0 assigns a free port)", "0");
			shot("host.png");
			press("Private network and streaming options");
			shot("host-options.png");
			tap(SDLK_ESCAPE);
			frame();
			press("Start server");
			until([&] { return sample.m_multiplayer.playable && sample.m_multiplayer.administration; },
				"native Host and auto-join did not become ready");
			CHECK(sample.m_mpStatus == "Server ready",
				"the supervisor state stays visible beside earlier hosting errors (F19)");
			publish("endpoint", sample.m_multiplayer.hostEndpoint);
		}
		else
		{
			until([&] { return fs::exists(evidence / "endpoint"); }, "native host did not publish an endpoint");
			std::string endpoint;
			std::ifstream(evidence / "endpoint") >> endpoint;
			// F10 untouched case: selecting the AUTOMATIC draft must survive
			// view updates — the previous per-frame re-initialization
			// destroyed the caret and selection while the value still matched
			// the automatic endpoint.
			press("Private IPv4 address and port");
			tap(SDLK_A, SDL_KMOD_CTRL);
			frame();
			CHECK(sample.m_joinEndpoint.selectionStart() == 0 &&
				sample.m_joinEndpoint.selectionEnd() == sample.m_joinEndpoint.value().size(),
				"Ctrl+A selects the automatic draft (F10)");
			const std::string automatic = sample.m_joinEndpoint.value();
			for (int i = 0; i < 6; ++i)
				frame();
			CHECK(sample.m_joinEndpoint.value() == automatic &&
				sample.m_joinEndpoint.selectionStart() == 0 &&
				sample.m_joinEndpoint.selectionEnd() == automatic.size(),
				"the untouched draft selection survives view updates (F10)");
			type("Private IPv4 address and port", "invalid");
			// Correction F10: the automatic endpoint must not fight the
			// editor — the value, the caret and a Ctrl+A selection survive
			// rendered frames while the draft is being edited.
			const std::string draft = sample.m_joinEndpoint.value();
			CHECK(sample.m_joinEndpoint.caret() == draft.size(), "typing leaves the caret at the end");
			for (int i = 0; i < 6; ++i)
				frame();
			CHECK(sample.m_joinEndpoint.value() == draft &&
				sample.m_joinEndpoint.caret() == draft.size(),
				"an edited draft and its caret survive view updates (F10)");
			tap(SDLK_A, SDL_KMOD_CTRL);
			frame();
			CHECK(sample.m_joinEndpoint.selectionStart() == 0 &&
				sample.m_joinEndpoint.selectionEnd() == draft.size(),
				"Ctrl+A selects the whole draft (F10)");
			for (int i = 0; i < 6; ++i)
				frame();
			CHECK(sample.m_joinEndpoint.selectionStart() == 0 &&
				sample.m_joinEndpoint.selectionEnd() == draft.size(),
				"the selection survives view updates (F10)");
			press("Connect");
			frame();
			CHECK(!sample.m_multiplayer.joinError.empty(), "native Join shows an invalid endpoint");
			type("Private IPv4 address and port", endpoint);
			press("Toggle favorite");
			CHECK(engine.m_joinPreferences.favorites.size() == 1, "native Join saves favorites");
			type("Expected world seed (optional)", "43");
			press("Connect");
			until([&] {
				return sample.m_multiplayer.connection && sample.m_multiplayer.connection->state == ui::MultiplayerState::Rejected;
			}, "wrong seed did not reach native rejection state");
			CHECK(std::string_view(sample.m_multiplayer.connection->detail) == "World seed mismatch",
				"native Join uses the real seed rejection");
			type("Expected world seed (optional)", "42");
			shot("join.png");
			press("Connect");
			until([&] { return sample.m_multiplayer.playable; }, "native guest did not join");
		}
		const int selectedSlot = host ? 2 : 5;
		engine.selectHotbarSlot(selectedSlot);
		until([&] { return engine.selectedHotbarSlot() == selectedSlot; }, "live inventory action was not acknowledged");
		until([&] {
			return sample.m_multiplayer.nearby.size() >= 2 && sample.m_multiplayer.network.ping &&
				(!host || (sample.m_multiplayer.monitor && sample.m_multiplayer.monitor->transport.count == 2));
		}, "two live players and independent ping were not observed");
		const auto self = sample.m_multiplayer.self;
		checked(self.valid(), "server assigned a live player");
		if (host)
			press("Players");
		else
		{
			tap(SDLK_ESCAPE);
			frame();
			press("Players");
		}
		shot(host ? "owner-roster.png" : "guest-roster.png");
		tap(SDLK_ESCAPE);
		frame();

		if (host)
		{
			press("Administration");
			press("Refresh players and audit");
			until([&] {
				return sample.m_multiplayer.administration &&
					sample.m_multiplayer.monitor && sample.m_multiplayer.administration->players.size() >= 2;
			}, "native refresh did not return the registered roster");
			// The guest walks the deliberate seed-mismatch rejection before
			// its final join, so its live session id lands in the registry a
			// moment after the roster first shows two players. A single
			// refresh can race that transition: poll with the idempotent
			// refresh until the registry names the session the monitor
			// observes, then assert the linkage once.
			const auto liveGuestRegistered = [&] {
				if (!sample.m_multiplayer.administration || !sample.m_multiplayer.monitor)
					return false;
				const auto roster = sample.m_multiplayer.monitor->transport.roster();
				const auto live = std::ranges::find_if(roster, [&](const auto &row) { return row.id != self; });
				return live != roster.end() &&
					std::ranges::find(sample.m_multiplayer.administration->players, live->id,
						&localcontrol::Player::id) != sample.m_multiplayer.administration->players.end();
			};
			for (int round = 0; round < 6 && !liveGuestRegistered(); ++round)
			{
				press("Refresh players and audit");
				until([&] { return !sample.m_multiplayer.adminPending; },
					"the administration refresh did not settle");
			}
			const auto roster = sample.m_multiplayer.monitor->transport.roster();
			const auto liveGuest = std::ranges::find_if(roster, [&](const auto &row) { return row.id != self; });
			checked(liveGuest != roster.end(), "host monitor has the live guest");
			const auto &rows = sample.m_multiplayer.administration->players;
			const auto other = std::ranges::find(rows, liveGuest->id, &localcontrol::Player::id);
			checked(other != rows.end(), "host sees registered guest");
			const auto guest = other->id;
			// Correction F01: before an explicit selection no destructive
			// action exists at all, and the selection is a stable identity.
			{
				const auto widgets = sample.m_widgets.widgets();
				CHECK(std::ranges::none_of(widgets, [](const gameui::Widget &widget) {
					return widget.label == "Kick for 60 seconds...";
				}), "no destructive action exists before an explicit target");
			}
			press("Registered players", static_cast<int>(other - rows.begin()));
			CHECK(sample.m_mpAdminTarget == guest, "the row press selects the target identity");
			const auto targetRow = std::ranges::find(sample.m_multiplayer.administration->players,
				sample.m_mpAdminTarget, &localcontrol::Player::id);
			checked(targetRow != sample.m_multiplayer.administration->players.end(),
				"the selected identity resolves to a live row");
			auto kick = gameui::AdministrationIntent{sample.m_multiplayer.launch,
				sample.m_multiplayer.administration->world, sample.m_multiplayer.administration->epoch,
				localcontrol::Role::Owner, localcontrol::Operation::Kick, *targetRow, false, {}};
			CHECK(!gameui::eligibleAdministration(kick, sample.m_multiplayer.launch,
				*sample.m_multiplayer.administration, nullptr), "stale monitoring prevents online moderation");
			press("Grant admin role...");
			CHECK(sample.screens().top() == Screen::MultiplayerConfirm && !engine.m_hostSnapshot.adminPending,
				"first click opens native confirmation without submitting");
			auto stale = *sample.m_multiplayerConfirm->administration;
			++stale.epoch;
			CHECK(!gameui::eligibleAdministration(stale, sample.m_multiplayer.launch,
				*sample.m_multiplayer.administration, sample.m_multiplayer.monitor.get()), "obsolete permission epoch is refused");
			shot("owner-confirmation.png");
			press("Confirm");
			until([&] {
				const auto &admin = sample.m_multiplayer.administration;
				return admin && std::ranges::any_of(admin->players, [&](const auto &row) {
					return row.id == guest && row.role == localcontrol::Role::Admin && !row.localAdmin;
				});
			}, "native confirm did not grant the role only");
			publish("role-granted", "yes");
			// Correction F01: the grant refreshed the roster rows while the
			// target row content changed; the identity selection survived.
			CHECK(sample.m_mpAdminTarget == guest, "a roster refresh preserves the selected identity");
			until([&] { return fs::exists(evidence / "guest-reconnected"); }, "guest did not reconnect");
			// Delegated control honors the credential boundary: a profile the
			// target does not own is refused by the server (correction F04).
			press("Authorize local control...");
			CHECK(sample.m_multiplayerConfirm->administration->profile == "owner-native",
				"the delegation confirm carries the selected local profile");
			press("Confirm");
			until([&] {
				return sample.m_multiplayer.adminResult == localcontrol::Result::InvalidCredential;
			}, "delegating a profile that is not the target's own credential is refused");
			// Kick end-to-end (corrections F02/F04): destructive moderation of
			// a live target through the confirmation pipeline. The guest
			// finishes its closed-menu probe first (F14), so the kick lands
			// against a known client state.
			until([&] { return fs::exists(evidence / "guest-ready-for-kick"); },
				"the guest did not finish its closed-menu probe");
			press("Kick for 60 seconds...");
			CHECK(sample.m_multiplayerConfirm->administration->operation == localcontrol::Operation::Kick &&
				sample.m_multiplayerConfirm->administration->subject &&
				sample.m_multiplayerConfirm->administration->subject->id == guest,
				"the kick confirmation captures the operation and identity");
			shot("owner-kick-confirmation.png");
			press("Confirm");
			until([&] { return fs::exists(evidence / "guest-kicked"); }, "the kicked guest did not observe it");
			until([&] { return fs::exists(evidence / "guest-kick-refused"); },
				"the kicked profile was not refused reconnection");
			// Ban the now-offline target (corrections F04/F16): an in-flight
			// administration request disables a close-succession duplicate
			// activation, and a resubmission of the SAME captured intent is
			// refused by the pipeline revalidation because its row changed.
			const auto banEnabled = [&] {
				const auto widgets = sample.m_widgets.widgets();
				const auto found = std::ranges::find(widgets, "Ban...", &gameui::Widget::label);
				return found != widgets.end() && found->enabled;
			};
			press("Ban...");
			auto duplicateBan = *sample.m_multiplayerConfirm;
			press("Confirm");
			until([&] { return sample.m_multiplayer.adminPending; },
				"the ban request reached the server");
			CHECK(!banEnabled(), "an in-flight administration request blocks a duplicate activation (F16)");
			until([&] {
				const auto &admin = sample.m_multiplayer.administration;
				return admin && std::ranges::any_of(admin->players, [&](const auto &row) {
					return row.id == guest && row.banned;
				});
			}, "native confirm did not ban the player");
			sample.m_multiplayerRequest = std::move(duplicateBan);
			frame();
			CHECK(sample.m_multiplayer.notice &&
				sample.m_multiplayer.notice->outcome == gameui::MultiplayerOutcome::PermissionChanged,
				"a duplicated submission is refused because its captured row changed (F16)");
			press("Unban...");
			press("Confirm");
			until([&] {
				const auto &admin = sample.m_multiplayer.administration;
				return admin && std::ranges::any_of(admin->players, [&](const auto &row) {
					return row.id == guest && !row.banned;
				});
			}, "native confirm did not unban the player");
			// Allowlist enable, membership and disable (correction F04).
			press("Enable allowlist...");
			press("Confirm");
			until([&] {
				return sample.m_multiplayer.administration && sample.m_multiplayer.administration->allowlist;
			}, "native confirm did not enable the allowlist");
			press("Add to allowlist...");
			press("Confirm");
			until([&] {
				const auto &admin = sample.m_multiplayer.administration;
				return admin && std::ranges::any_of(admin->players, [&](const auto &row) {
					return row.id == guest && row.allowed;
				});
			}, "native confirm did not add the allowlist member");
			press("Disable allowlist...");
			press("Confirm");
			until([&] {
				return sample.m_multiplayer.administration && !sample.m_multiplayer.administration->allowlist;
			}, "native confirm did not disable the allowlist");
			// A cancelled confirmation has no side effect (correction F04).
			press("Stop and save server...");
			CHECK(sample.screens().top() == Screen::MultiplayerConfirm, "stop opens the confirmation");
			press("Cancel");
			CHECK(sample.m_multiplayer.hostState == hosting::State::Ready && !sample.m_multiplayerConfirm,
				"a cancelled confirmation does not stop the server");
			// Stale confirmations are refused through the real execution
			// pipeline with typed feedback (corrections F03/F04): the requests
			// below are enqueued directly so the pump evaluates them exactly
			// as a late or duplicated submission would arrive.
			press("Enable allowlist...");
			const auto revisionBefore = sample.m_multiplayer.administration->revision;
			auto staleEpoch = *sample.m_multiplayerConfirm;
			++staleEpoch.administration->epoch;
			sample.m_multiplayerRequest = std::move(staleEpoch);
			frame();
			CHECK(sample.m_multiplayer.notice &&
				sample.m_multiplayer.notice->outcome == gameui::MultiplayerOutcome::PermissionChanged,
				"an obsolete permission epoch is refused with typed feedback");
			CHECK(!engine.m_hostSnapshot.adminPending, "the refused confirmation never reached the server");
			auto staleLaunch = *sample.m_multiplayerConfirm;
			staleLaunch.launch += 12345;
			sample.m_multiplayerRequest = std::move(staleLaunch);
			frame();
			CHECK(sample.m_multiplayer.notice &&
				sample.m_multiplayer.notice->outcome == gameui::MultiplayerOutcome::ServerChanged,
				"a confirmation from a previous server launch is refused");
			press("Cancel");
			// The vanished-target refusal uses a real TARGETED operation
			// (correction F16): the rejection comes from the unknown subject,
			// not from an untargeted operation incorrectly carrying one.
			press("Ban...");
			auto staleTarget = *sample.m_multiplayerConfirm;
			staleTarget.administration->subject->id = sim::PlayerId{99};
			sample.m_multiplayerRequest = std::move(staleTarget);
			frame();
			CHECK(sample.m_multiplayer.notice &&
				sample.m_multiplayer.notice->outcome == gameui::MultiplayerOutcome::PermissionChanged,
				"a confirmation whose target no longer exists is refused (F16)");
			press("Cancel");
			CHECK(sample.m_multiplayer.administration &&
				sample.m_multiplayer.administration->revision == revisionBefore,
				"refused and duplicated confirmations commit nothing (F16)");
			// The reviewed action finally applies, proving the refusals above
			// did not consume or corrupt the real intent.
			press("Enable allowlist...");
			press("Confirm");
			until([&] {
				return sample.m_multiplayer.administration && sample.m_multiplayer.administration->allowlist;
			}, "the reviewed enable allowlist was applied");
			press("Disable allowlist...");
			press("Confirm");
			until([&] {
				return sample.m_multiplayer.administration && !sample.m_multiplayer.administration->allowlist;
			}, "the allowlist was disabled again");
			// Revoke the granted role on the offline target (correction F04):
			// the demotion is the final committed role change.
			press("Revoke admin...");
			press("Confirm");
			until([&] {
				const auto &admin = sample.m_multiplayer.administration;
				return admin && std::ranges::any_of(admin->players, [&](const auto &row) {
					return row.id == guest && row.role == localcontrol::Role::Player;
				});
			}, "native confirm did not revoke the role");
			// The audit history proves single execution end-to-end (F04).
			press("Recent audit history");
			CHECK(std::ranges::count_if(sample.m_multiplayer.administration->audit,
				[&](const localcontrol::Audit &row) {
					return row.operation == localcontrol::Operation::Kick && row.target == guest;
				}) == 1, "exactly one committed kick reached the audit history");
			CHECK(std::ranges::count_if(sample.m_multiplayer.administration->audit,
				[&](const localcontrol::Audit &row) {
					return row.operation == localcontrol::Operation::Ban && row.target == guest;
				}) == 1, "a duplicated ban activation committed at most one operation (F16)");
			tap(SDLK_ESCAPE);
			frame();
			tap(SDLK_ESCAPE);
			frame();
			press("Stop and save server...");
			press("Confirm");
			until([&] { return sample.m_multiplayer.hostState == hosting::State::Stopped; },
				"hosted native checkpoint not confirmed");
			net::udp::Socket occupied({0x7f000001, 0});
			type("Port (0 assigns a free port)", std::to_string(occupied.address().port));
			press("Start server");
			until([&] { return sample.m_multiplayer.hostState == hosting::State::Failed; },
				"occupied port did not reach native failure");
			CHECK(sample.m_multiplayer.hostMessage.find("choose another port") != std::string::npos &&
				sample.m_mpStatus == sample.m_multiplayer.hostMessage,
				"the hosted bind failure offers a recovery step in the visible status slot (F19)");
			CHECK(!sample.m_mpAdminTarget.valid(),
				"a failed relaunch cleared the administrative selection (F12)");
		}
		else
		{
			until([&] { return fs::exists(evidence / "role-granted"); }, "owner did not confirm role");
			CHECK(!sample.m_multiplayer.administration, "remote role grants no local administration channel");
			press("Join server");
			press("Disconnect");
			until([&] { return sample.m_multiplayer.reconnect; }, "native disconnect did not finish");
			press("Reconnect");
			until([&] { return sample.m_multiplayer.playable; }, "native reconnect did not finish");
			CHECK(sample.m_multiplayer.self == self, "reconnect restored the original player identity");
			publish("guest-reconnected", "yes");
			// The clipboard result is displayed despite a standing connection
			// error (correction F15), and a refused switch preserves the live
			// session.
			type("Private IPv4 address and port", "invalid");
			press("Switch server or profile");
			until([&] { return !sample.m_multiplayer.joinError.empty(); },
				"the refused switch shows the endpoint error");
			CHECK(sample.m_multiplayer.playable, "the refused switch preserved the live session");
			press("Copy connected endpoint");
			CHECK(sample.m_mpClipboard == "Endpoint copied to the clipboard." &&
				sample.m_mpDetail == "Endpoint copied to the clipboard.",
				"the clipboard result is reported and painted despite the connection error (F15/F19)");
			if (char *copied = SDL_GetClipboardText())
			{
				CHECK(std::string_view(copied) == sample.m_multiplayer.endpoint,
					"the connected endpoint reached the clipboard");
				SDL_free(copied);
			}
			else
				checked(false, "the clipboard holds the endpoint");
			type("Private IPv4 address and port", sample.m_multiplayer.endpoint);
			// Closing the multiplayer flow does not pause the session
			// (correction F14): the transport keeps advancing with the menu
			// closed, and the flow re-enters cleanly afterwards.
			press("Play");
			for (int i = 0; i < 30; ++i)
				frame();
			CHECK(engine.m_remote && engine.m_remote->ready() &&
				engine.m_remote->players.fresh(Engine::RemoteSession::Clock::now()),
				"the live session keeps advancing with the multiplayer menu closed (F14)");
			tap(SDLK_ESCAPE);
			frame();
			press("Multiplayer");
			press("Join server");
			publish("guest-ready-for-kick", "yes");
			// The kick is destructive moderation observed end-to-end
			// (correction F02): the server actively closes the session and the
			// persistent identity is then refused reconnection.
			until([&] {
				return sample.m_multiplayer.connection &&
					sample.m_multiplayer.connection->state == ui::MultiplayerState::Disconnected;
			}, "the kicked guest did not observe the disconnect");
			publish("guest-kicked", "yes");
			press("Reconnect");
			until([&] {
				return sample.m_multiplayer.connection &&
					sample.m_multiplayer.connection->state == ui::MultiplayerState::Rejected &&
					sample.m_multiplayer.connection->detail &&
					std::string_view(sample.m_multiplayer.connection->detail) == "Server refused the connection";
			}, "the kicked profile was not refused reconnection");
			publish("guest-kick-refused", "yes");
		}
		CHECK(!engine.developerWorkspaceVisible(), "all actions use the native workspace");
		CHECK(device.validationErrorCount() - device.rtssValidationErrorCount() == errors,
			"native multiplayer produced no new application validation errors");
	}


	void screenshot(const std::filesystem::path &out)
	{
		init();
		auto &device = *engine.vkContext;
		const auto before = device.validationErrorCount();
		constexpr VkExtent2D extent{640, 360};
		resize(int(extent.width), int(extent.height));
		const uint8_t coverage = 128;
		gameui::Renderer reference;
		reference.init(device, *engine.immediate, {1, 1}, {&coverage, 1}, VK_FORMAT_R8G8B8A8_SRGB, kColorSpace);
		auto &world = *engine.worldRenderer;
		gameui::DrawList draws;
		draws.reset(glm::vec2(extent.width, extent.height), engine.m_atlasVersion);
		const auto full = draws.bounds();
		draws.panel({0, 0, 192, 64}, kBlack, full);
		draws.panel({0, 0, 64, 64}, kHalfWhite, full.intersect({{8, 8, 56, 56}}));
		draws.panel({64, 0, 128, 64}, {1.f, 0.f, 0.f, 0.5f}, full);
		draws.panel({64, 0, 128, 64}, {0.f, 0.f, 1.f, 0.5f}, full);
		draws.glyph({128, 0, 192, 64}, {0.f, 0.f, 1.f, 1.f}, glm::vec4(1.f), full);
		draws.panel({196, 0, 224, 16}, glm::vec4(0.f), full);
		draws.icon({200, 0, 216, 16}, {engine.m_atlasVersion - 1, GRASS_SIDE}, glm::vec4(1.f), full);
		CHECK(draws.quads().size() == 6 && draws.batches().size() == 3,
			"only consecutive equal scissors batch and stale atlas handles are dropped");
		const auto dev = [](VkCommandBuffer cmd) {
			VkClearAttachment marker{};
			marker.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			marker.clearValue.color = {{0.f, 1.f, 0.f, 1.f}};
			const VkClearRect rect{{{16, 16}, {16, 16}}, 0, 1};
			vkCmdClearAttachments(cmd, 1, &marker, 1, &rect);
		};
		for (const VkFormat format : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM})
		{
			world.resizeOutput(extent, format, kColorSpace);
			reference.setOutput(format, kColorSpace);
			auto &sample = *engine.m_playerUi;
			sample.renderer().setOutput(format, kColorSpace);
			Readback target{device};
			const auto allocator = device.getAllocator();
			target.output = createImage2D(allocator, device.getDevice(), extent.width, extent.height, format,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			target.buffer = createBuffer(allocator, extent.width * extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
			for (const bool aa : {false, true})
			{
				world.postSettings().fxaaEnabled = aa;
				const auto base = capture(target, nullptr);
				reference.prepare(0, extent, draws, world.getTextureManager(), engine.m_atlasVersion);
				const auto ui = capture(target, &reference);
				CHECK(matchesSrgb(channel(ui, 10, 10, 0), 0.5f) && channel(ui, 4, 4, 0) == 0,
					"half-white is display encoded after linear blending and respects its scissor");
				CHECK(matchesSrgb(channel(ui, 80, 20, 0), 0.25f) && matchesSrgb(channel(ui, 80, 20, 2), 0.5f),
					"overlapping layers blend in paint order");
				CHECK(matchesSrgb(channel(ui, 142, 20, 0), float(coverage) / 255.f),
					"glyph coverage stays linear");
				CHECK(std::abs(int(channel(ui, 203, 8, 0)) - int(channel(base, 203, 8, 0))) <= 1,
					"untouched scene survives either final output route");
				const auto both = capture(target, &reference, dev);
				CHECK(channel(both, 20, 20, 0) == 0 && channel(both, 20, 20, 1) == 255 &&
					channel(both, 20, 20, 2) == 0, "developer pass loads the completed Game UI output");
				sample.prepare(engine.window, 0, extent, {}, world.getTextureManager(), engine.m_atlasVersion);
				const auto image = capture(target, &sample.renderer());
				const std::string name = std::string(format == VK_FORMAT_R8G8B8A8_SRGB ? "srgb" : "unorm") +
					(aa ? "-fxaa.png" : "-direct.png");
				checked(visual::writePng((out / name).string(), image), "write Game UI screenshot");
			}
			if (format == VK_FORMAT_R8G8B8A8_UNORM)
			{
				const auto &atlas = world.getTextureManager();
				reference.prepare(1, extent, draws, atlas, engine.m_atlasVersion);
				const auto twoSlots = reference.stats(0);
				CHECK(reference.linearTarget(0) && reference.linearTarget(1) &&
					reference.linearTarget(0)->image != reference.linearTarget(1)->image,
					"UNORM composition images belong to individual frame slots");
				reference.prepare(1, {801, 451}, draws, atlas, engine.m_atlasVersion);
				CHECK(reference.stats(0).compositionBytes > twoSlots.compositionBytes, "UNORM slot grows on resize");
				reference.prepare(1, extent, draws, atlas, engine.m_atlasVersion);
				CHECK(reference.stats(0).compositionBytes == twoSlots.compositionBytes &&
					reference.stats(0).descriptorSets == twoSlots.descriptorSets, "UNORM resize returns to its allocation footprint");
			}
		}
		CHECK(device.validationErrorCount() == before, "offscreen UI format and resize paths are validation-clean");
	}

	VkExtent2D extent() const
	{
		int w = 0, h = 0;
		checked(SDL_GetWindowSizeInPixels(engine.window, &w, &h) && w > 0 && h > 0, "read SDL framebuffer extent");
		return {uint32_t(w), uint32_t(h)};
	}

	void present(const std::function<void()> &extraDevUi = {}, std::optional<uint64_t> gpuTag = std::nullopt)
	{
		auto &prof = GetProfiler();
		prof.beginFrame();
		engine.handleEvents();
		engine.pumpPlayerSettings();
		const auto size = engine.playerFramebufferSize();
		const bool ready = size.width > 0 && size.height > 0 &&
			engine.recreateSwapchainIfNeeded(uint32_t(size.width), uint32_t(size.height));
		engine.updatePlayerDisplayPreview(size, ready);
		if (!ready)
		{
			prof.endFrame();
			return;
		}
		uint32_t image = 0;
		checked(engine.frameCtx->beginFrame(*engine.swapchain, image), "acquire Game UI proof frame");
		const uint32_t slot = engine.frameCtx->frameIndex();
		engine.resourceRetire.beginFrame(engine.frameNumber);
		engine.worldRenderer->arenas().beginFrame(engine.frameNumber);
		engine.stagingRing.beginFrame(slot);
		if (!engine.m_inspectionView)
		{
			PROFILE_SCOPE("DevUIBuild");
			engine.imgui->beginFrame();
			engine.drawUi();
			if (extraDevUi)
				extraDevUi();
			engine.imgui->endFrame();
		}
		engine.updateFrameUniforms(slot, size.width, size.height, 5.f);
		engine.recordFrame(slot, image, 0, gpuTag);
		checked(engine.frameCtx->submitAndPresent(*engine.swapchain, image), "present Game UI proof frame");
		engine.noteSessionFramePresented();
		++engine.frameNumber;
		prof.endFrame();
	}

	void resize(int width, int height)
	{
		checked(SDL_SetWindowSize(engine.window, width, height) && SDL_SyncWindow(engine.window),
			"resize Game UI proof window");
	}

	void frames(int count)
	{
		for (int i = 0; i < count; ++i)
			present();
	}

	void button(Uint32 type, float x, float y, Uint8 which = SDL_BUTTON_LEFT)
	{
		// Real pointer input from the desktop interleaves with the synthetic
		// clicks: a motion or window leave queued between the press and its
		// release cancels the armed activation, and the click is silently
		// dropped. The proof drives only synthetic input, so drop any queued
		// real pointer events before each click pair.
		if (type == SDL_EVENT_MOUSE_BUTTON_DOWN)
		{
			SDL_PumpEvents();
			SDL_FlushEvent(SDL_EVENT_MOUSE_MOTION);
			SDL_FlushEvent(SDL_EVENT_MOUSE_BUTTON_DOWN);
			SDL_FlushEvent(SDL_EVENT_MOUSE_BUTTON_UP);
			SDL_FlushEvent(SDL_EVENT_MOUSE_WHEEL);
			SDL_FlushEvent(SDL_EVENT_WINDOW_MOUSE_ENTER);
			SDL_FlushEvent(SDL_EVENT_WINDOW_MOUSE_LEAVE);
		}
		SDL_Event event{};
		event.type = type;
		event.button.windowID = SDL_GetWindowID(engine.window);
		event.button.button = which;
		event.button.x = x;
		event.button.y = y;
		checked(SDL_PushEvent(&event), "queue Game UI sample click");
	}

	void motion(float x, float y)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(engine.window);
		event.motion.x = x;
		event.motion.y = y;
		checked(SDL_PushEvent(&event), "queue pointer motion");
	}

	void clickAt(glm::vec2 point, Uint8 which = SDL_BUTTON_LEFT)
	{
		SDL_WarpMouseInWindow(engine.window, point.x, point.y);
		motion(point.x, point.y);
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, point.x, point.y, which);
		button(SDL_EVENT_MOUSE_BUTTON_UP, point.x, point.y, which);
	}

	void commitText(const char *text)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_TEXT_INPUT;
		event.text.windowID = SDL_GetWindowID(engine.window);
		event.text.text = text;
		checked(SDL_PushEvent(&event), "queue committed SDL text");
	}

	void key(SDL_Keycode code, Uint32 type, bool repeat = false, SDL_Keymod mods = SDL_KMOD_NONE)
	{
		SDL_Event event{};
		event.type = type;
		event.key.windowID = SDL_GetWindowID(engine.window);
		event.key.key = code;
		event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
		event.key.repeat = repeat;
		event.key.mod = mods;
		checked(SDL_PushEvent(&event), "queue Game UI keyboard event");
	}

	void tap(SDL_Keycode code, SDL_Keymod mods = SDL_KMOD_NONE)
	{
		key(code, SDL_EVENT_KEY_DOWN, false, mods);
		key(code, SDL_EVENT_KEY_UP, false, mods);
	}

	void windowEvent(Uint32 type)
	{
		SDL_Event event{};
		event.type = type;
		event.window.windowID = SDL_GetWindowID(engine.window);
		checked(SDL_PushEvent(&event), "queue Game UI window event");
	}

	void showcaseProof()
	{
		init();
		auto &context = *engine.vkContext;
		const auto before = context.validationErrorCount();
		const auto rtssBefore = context.rtssValidationErrorCount();
		auto &sample = *engine.m_playerUi;
		using gameui::Screen;
		using gameui::Widget;
		const auto widget = [&](std::string_view label) -> const Widget & {
			for (const auto &item : sample.m_widgets.widgets())
				if (item.label == label)
					return item;
			throw std::runtime_error("showcase control missing");
		};
		const auto center = [&](std::string_view label) {
			const auto bounds = sample.m_widgets.bounds(widget(label)).rect;
			return (glm::vec2(bounds) + glm::vec2(bounds.z, bounds.w)) * 0.5f;
		};
		const auto click = [&](glm::vec2 point) {
			SDL_WarpMouseInWindow(engine.window, point.x, point.y);
			button(SDL_EVENT_MOUSE_BUTTON_DOWN, point.x, point.y);
			button(SDL_EVENT_MOUSE_BUTTON_UP, point.x, point.y);
		};
		resize(1920, 1080);
		frames(3);
		const auto firstPolicy = engine.inputContext();
		CHECK(firstPolicy.interactive && !firstPolicy.devKeyboard,
			"focused WSI test starts with gameplay ownership; focused=" << firstPolicy.interactive
			<< " developer=" << firstPolicy.devKeyboard);
		tap(SDLK_E);
		present();
		CHECK(engine.inventoryPanelOpen() && sample.screens().top() == Screen::Inventory &&
			!engine.mouseCaptured && !engine.simulationSuspended(), "inventory releases cursor without pausing solo");
		tap(SDLK_ESCAPE);
		present();
		CHECK(!engine.inventoryPanelOpen() && sample.screens().top() == Screen::Hud && engine.mouseCaptured,
			"Escape closes inventory and restores capture");

		key(SDLK_W, SDL_EVENT_KEY_DOWN);
		engine.handleEvents();
		CHECK(engine.m_inputRouter.gameplayKey(SDL_SCANCODE_W), "held W initially belongs to gameplay");
		tap(SDLK_ESCAPE);
		present();
		CHECK(sample.screens().top() == Screen::Pause && engine.simulationSuspended() &&
			!engine.m_inputRouter.gameplayKey(SDL_SCANCODE_W) && !engine.mouseCaptured,
			"solo Escape opens Pause and clears held gameplay");
		CHECK(sample.screens().focus() == widget("Resume").id,
			"new Pause focuses Resume (focus=" << sample.screens().focus() << ", layout revision=" <<
			sample.m_layoutRevision << ", screen revision=" << sample.screens().revision() << ")");
		for (int i = 0; i < 4; ++i)
			tap(SDLK_TAB);
		engine.handleEvents();
		CHECK(sample.screens().focus() == widget("Widget showcase").id,
			"Tab focuses the showcase opener (focus=" << sample.screens().focus() << ", active=" <<
			engine.inputContext().interactive << ", developer text=" << engine.inputContext().devText <<
			", layout=" << sample.m_layoutRevision << ", stack=" << sample.screens().revision() << ")");
		tap(SDLK_RETURN);
		present();
		CHECK(sample.screens().top() == Screen::Showcase && sample.m_sdlEditor == &sample.m_field && SDL_TextInputActive(engine.window),
			"keyboard opens showcase and starts SDL text input at the first field (screen=" <<
			int(sample.screens().top()) << ", focus=" << sample.screens().focus() << ")");
		SDL_Event editing{};
		editing.type = SDL_EVENT_TEXT_EDITING;
		editing.edit.windowID = SDL_GetWindowID(engine.window);
		editing.edit.text = "a\xc3\xa9";
		editing.edit.start = 1;
		editing.edit.length = 1;
		checked(SDL_PushEvent(&editing), "queue SDL composition");
		engine.handleEvents();
		CHECK(sample.m_field.value().empty() && sample.m_field.composition() == "a\xc3\xa9" &&
			sample.m_field.compositionCursor() == 1 && sample.m_field.compositionEnd() == 3,
			"SDL preedit preserves a multibyte caret and selection without committing");
		SDL_Event text{};
		text.type = SDL_EVENT_TEXT_INPUT;
		text.text.windowID = SDL_GetWindowID(engine.window);
		text.text.text = "Qa\xc3\xa9";
		tap(SDLK_Q);
		checked(SDL_PushEvent(&text), "queue UTF-8 text commit");
		present();
		CHECK(sample.m_field.value() == "Qa\xc3\xa9" && sample.m_field.composition().empty(),
			"committed text reaches one editor without gameplay ownership");
		tap(SDLK_LEFT);
		tap(SDLK_BACKSPACE);
		present();
		CHECK(sample.m_field.value() == "Q\xc3\xa9", "caret moves and deletes at UTF-8 boundaries");
		tap(SDLK_TAB);
		key(SDLK_SPACE, SDL_EVENT_KEY_DOWN);
		key(SDLK_SPACE, SDL_EVENT_KEY_DOWN, true);
		key(SDLK_SPACE, SDL_EVENT_KEY_UP);
		present();
		CHECK(!sample.m_feature.value && !sample.m_sdlEditor && !engine.jumpPressed,
			"toggle fires once and keyboard space does not queue gameplay jump");
		tap(SDLK_TAB);
		tap(SDLK_RIGHT);
		tap(SDLK_TAB);
		tap(SDLK_DOWN);
		present();
		CHECK(std::abs(sample.m_slider.value - 0.55f) < 0.001f && sample.m_list.selection.selected == 1,
			"keyboard adjusts slider and list selection");
		SDL_Event wheel{};
		wheel.type = SDL_EVENT_MOUSE_WHEEL;
		wheel.wheel.windowID = SDL_GetWindowID(engine.window);
		wheel.wheel.y = -1.f;
		wheel.wheel.mouse_x = center("Biome preview").x;
		wheel.wheel.mouse_y = center("Biome preview").y;
		checked(SDL_PushEvent(&wheel), "queue SDL wheel");
		present();
		CHECK(sample.m_list.selection.first == 1 && sample.m_list.selection.selected == 1, "wheel scrolls only the visible list");

		click(center("Secondary name"));
		text.text.text = "Secondary";
		checked(SDL_PushEvent(&text), "queue text for a second editor");
		present();
		CHECK(sample.m_secondaryField.value() == "Secondary" && sample.m_field.value() == "Q\xc3\xa9",
			"two bound text fields keep independent committed values");
		const std::string longPreedit(gameui::TextField::kMaxBytes, 'W');
		editing.edit.text = longPreedit.c_str();
		editing.edit.start = int(longPreedit.size());
		editing.edit.length = 0;
		checked(SDL_PushEvent(&editing), "queue horizontally scrolling preedit");
		engine.handleEvents();
		SDL_Rect textArea{};
		int cursor = 0;
		const auto textGeometry = sample.m_widgets.editorGeometry(sample.screens().focus(), sample.font(),
			sample.m_screenScale, sample.m_fontScale);
		CHECK(SDL_GetTextInputArea(engine.window, &textArea, &cursor) &&
			textArea.x + cursor == int(std::lround(textGeometry.caretX)) &&
			textGeometry.caretX == textGeometry.clip.rect.z - 1.f &&
			textGeometry.glyphScale == gameui::textStyle(gameui::style::TextRole::Body,
				sample.m_screenScale, sample.m_fontScale).glyphScale(),
			"SDL candidate location follows the visible horizontally scrolled caret");
		click(center("Secondary level"));
		click(center("Enable secondary name"));
		click(center("Secondary name"));
		text.text.text = "Ignored";
		checked(SDL_PushEvent(&text), "queue committed text over disabled editor");
		present();
		CHECK(!sample.m_enableSecond.value && !sample.m_sdlEditor && !SDL_TextInputActive(engine.window) &&
			sample.m_secondaryField.value() == "Secondary" && !sample.m_feature.value &&
			std::abs(sample.m_secondarySlider.value - 0.5f) < 0.01f && std::abs(sample.m_slider.value - 0.55f) < 0.001f,
			"second slider and toggle are independent, and disabling the field stops its SDL session");
		CHECK(sample.lastRecordedStats() && sample.lastRecordedStats()->metrics.draws <= 31 &&
			sample.lastRecordedStats()->metrics.dropped == 0, "two widget groups fit the existing draw budget");

		const auto sliderPoint = center("Preview level");
		const bool overDeveloper = engine.developerPointerAt(50.f, 12.f) == ImGuiLayer::PointerHit::Interactive;
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, sliderPoint.x, sliderPoint.y);
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, 50.f, 12.f, SDL_BUTTON_RIGHT);
		button(SDL_EVENT_MOUSE_BUTTON_UP, 50.f, 12.f, SDL_BUTTON_RIGHT);
		motion(sliderPoint.x + 40.f, sliderPoint.y);
		engine.handleEvents();
		CHECK(overDeveloper && sample.m_slider.value > 0.55f && sample.m_widgets.hasPress(),
			"cross-surface blocked release cannot cancel the native slider");
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, sliderPoint.x, sliderPoint.y, SDL_BUTTON_RIGHT);
		motion(sliderPoint.x + 50.f, sliderPoint.y);
		engine.handleEvents();
		const float beforeCancel = sample.m_slider.value;
		windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST);
		windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
		motion(sliderPoint.x - 60.f, sliderPoint.y);
		button(SDL_EVENT_MOUSE_BUTTON_UP, sliderPoint.x, sliderPoint.y, SDL_BUTTON_RIGHT);
		button(SDL_EVENT_MOUSE_BUTTON_UP, sliderPoint.x, sliderPoint.y);
		present();
		CHECK(beforeCancel > 0.5f && sample.m_slider.value == beforeCancel && !sample.m_widgets.hasPress(),
			"two-button native drag does not resume after cancellation or out-of-order releases");

		auto pos = center("Preview option");
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, pos.x, pos.y);
		windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST);
		button(SDL_EVENT_MOUSE_BUTTON_UP, pos.x, pos.y);
		tap(SDLK_ESCAPE);
		engine.handleEvents();
		CHECK(!sample.m_feature.value && sample.screens().top() == Screen::Showcase && !engine.windowFocused,
			"focus loss cancels a press and ignores delayed Escape");
		windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
		present();
		const auto previousFocus = sample.screens().focus();
		click(center("Unavailable"));
		present();
		CHECK(sample.screens().top() == Screen::Showcase && sample.screens().focus() == previousFocus,
			"disabled control cannot focus or activate");
		const auto oldPanel = sample.m_panel;
		pos = center("Preview option");
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, pos.x, pos.y);
		engine.handleEvents();
		resize(1160, 680);
		frames(2);
		pos = center("Preview option");
		button(SDL_EVENT_MOUSE_BUTTON_UP, pos.x, pos.y);
		present();
		CHECK(sample.m_panel != oldPanel && !sample.m_feature.value, "WSI resize reflows geometry and cancels the old press");
		click(pos);
		present();
		CHECK(sample.m_feature.value, "real pointer events hit the relocated control");

		tap(SDLK_ESCAPE);
		present();
		CHECK(sample.screens().top() == Screen::Pause && sample.screens().focus() == widget("Widget showcase").id,
			"back restores focus to the showcase opener");
		tap(SDLK_TAB);
		tap(SDLK_RETURN);
		present();
		CHECK(sample.screens().top() == Screen::ConfirmExit && engine.suppressDeveloperUi(),
			"confirmation takes full-window priority over Developer UI");
		tap(SDLK_F10);
		present();
		CHECK(!engine.m_pendingVSync.has_value(), "modal vetoes view-changing globals");
		tap(SDLK_ESCAPE);
		present();
		CHECK(sample.screens().top() == Screen::Pause && !engine.suppressDeveloperUi() &&
			sample.screens().focus() == widget("Exit to desktop").id, "Escape dismisses only the confirmation");
		// R10: the destructive action is visually distinct and opens with Cancel
		// focused, so one Enter cannot exit the game.
		tap(SDLK_RETURN);
		present();
		CHECK(sample.screens().top() == Screen::ConfirmExit &&
				widget("Exit").variant == gameui::style::ButtonVariant::Danger &&
				widget("Cancel").variant == gameui::style::ButtonVariant::Secondary &&
				sample.screens().focus() == widget("Cancel").id,
			"a destructive confirmation opens with Cancel focused and the danger action styled");
		tap(SDLK_RETURN);
		present();
		CHECK(sample.screens().top() == Screen::Pause,
			"Enter immediately after opening a destructive confirmation activates Cancel");
		tap(SDLK_ESCAPE);
		present();
		CHECK(sample.screens().top() == Screen::Hud && !engine.simulationSuspended() &&
			!engine.m_inputRouter.gameplayKey(SDL_SCANCODE_W), "resume cannot replay held W");
		key(SDLK_W, SDL_EVENT_KEY_UP);
		key(SDLK_W, SDL_EVENT_KEY_DOWN);
		engine.handleEvents();
		CHECK(engine.m_inputRouter.gameplayKey(SDL_SCANCODE_W), "fresh gameplay press works after resume");
		key(SDLK_W, SDL_EVENT_KEY_UP);
		keyboardHandoffProof();
		context.waitIdle();
		if (context.isValidationEnabled())
			CHECK(context.validationErrorCount() - before == context.rtssValidationErrorCount() - rtssBefore,
				"showcase presentation reports only the known RTSS subset if the hook is loaded");
	}

	void keyboardHandoffProof()
	{
		auto &sample = *engine.m_playerUi;
		char developerValue[96]{};
		glm::vec2 developerPoint{};
		const auto developer = [&] {
			ImGui::SetNextWindowPos({45.f, 210.f}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({190.f, 90.f}, ImGuiCond_Always);
			if (ImGui::Begin("Developer text handoff", nullptr,
				ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing))
			{
				ImGui::InputText("##handoff", developerValue, sizeof(developerValue));
				const auto position = ImGui::GetItemRectMin();
				const auto origin = ImGui::GetMainViewport()->Pos;
				developerPoint = {position.x - origin.x + 15.f, position.y - origin.y + 8.f};
			}
			ImGui::End();
		};
		const auto redraw = [&] {
			for (int frame = 0; frame < 7; ++frame)
				present(developer);
		};
		tap(SDLK_ESCAPE);
		present(developer);
		for (int i = 0; i < 4; ++i)
			tap(SDLK_TAB);
		tap(SDLK_RETURN);
		present(developer);
		present(developer);
		CHECK(sample.m_sdlEditor == &sample.m_field, "handoff starts with the native editor active");
		glm::vec2 nativePoint{};
		for (const auto &widget : sample.m_widgets.widgets())
			if (widget.label == "Name")
			{
				const auto rect = sample.m_widgets.bounds(widget).rect;
				nativePoint = {(rect.x + rect.z) * 0.5f, (rect.y + rect.w) * 0.5f};
			}
		const std::string originalNative = sample.m_field.value();
		clickAt(developerPoint);
		tap(SDLK_END);
		commitText("dev");
		engine.handleEvents();
		CHECK(engine.imgui->pendingKeyboard() && SDL_TextInputActive(engine.window) && !sample.m_sdlEditor &&
			sample.m_field.value() == originalNative, "native-to-developer press hands over SDL in the same event batch");
		redraw();
		CHECK(engine.imgui->editingText() && std::string_view(developerValue) == "dev",
			"real ImGui InputText consumes its same-batch text");

		commitText("-before");
		clickAt(nativePoint);
		tap(SDLK_END);
		commitText("-native");
		SDL_Event composition{};
		composition.type = SDL_EVENT_TEXT_EDITING;
		composition.edit.windowID = SDL_GetWindowID(engine.window);
		composition.edit.text = "preview";
		composition.edit.start = 1;
		checked(SDL_PushEvent(&composition), "queue preedit after native handoff");
		engine.handleEvents();
		CHECK(!engine.imgui->editingText() && !engine.imgui->pendingKeyboard() &&
			sample.m_sdlEditor == &sample.m_field && SDL_TextInputActive(engine.window) &&
			sample.m_field.value() == originalNative + "-native" && sample.m_field.composition() == "preview",
			"developer-to-native press transfers same-batch keys, commit and preedit");

		// Switch back before ImGui builds. The old editor must still receive
		// its earlier commit, followed by the newer click and text.
		clickAt(developerPoint);
		tap(SDLK_END);
		commitText("-after");
		engine.handleEvents();
		CHECK(sample.m_field.composition().empty() && !sample.m_sdlEditor &&
			engine.imgui->pendingKeyboard() && SDL_TextInputActive(engine.window),
			"return to developer clears native composition without dropping the reservation");
		redraw();
		CHECK(std::string_view(developerValue) == "dev-before-after" && engine.imgui->editingText() &&
			sample.m_field.value() == originalNative + "-native",
			"queued developer commits are preserved across the boundary without receiving native text");

		clickAt(nativePoint);
		tap(SDLK_END);
		commitText("-end");
		redraw();
		CHECK(!engine.imgui->editingText() && sample.m_sdlEditor == &sample.m_field &&
			SDL_TextInputActive(engine.window) && sample.m_field.value() == originalNative + "-native-end" &&
			std::string_view(developerValue) == "dev-before-after",
			"retiring ImGui does not stop the new native SDL session at EndFrame");
	}

	static int queuedReleases()
	{
		int result = 0;
		for (const auto &event : ImGui::GetCurrentContext()->InputEventsQueue)
			if (event.Type == ImGuiInputEventType_MouseButton &&
				event.MouseButton.Button == ImGuiMouseButton_Left && !event.MouseButton.Down)
				++result;
		return result;
	}

	void developerDragProof(bool native)
	{
		engine.setMouseCaptured(false);
		bool clicked = false;
		const glm::vec2 header{150.f, 82.f}, control{145.f, 110.f};
		const auto floating = [&] {
			ImGui::SetNextWindowPos({45.f, 70.f}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({240.f, 113.f}, ImGuiCond_Always);
			if (ImGui::Begin("Input cancellation proof", nullptr,
				ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
				clicked |= ImGui::Button("Fresh activation", {200.f, 34.f});
			ImGui::End();
		};
		const auto startDrag = [&] {
			SDL_WarpMouseInWindow(engine.window, header.x, header.y);
			motion(header.x, header.y);
			button(SDL_EVENT_MOUSE_BUTTON_DOWN, header.x, header.y);
			present(floating);
			CHECK(ImGui::GetIO().MouseDown[0] && ImGui::GetCurrentContext()->MovingWindow &&
				engine.m_inputRouter.pointerOwner() == gameui::InputOwner::DeveloperUi,
				"ImGui title drag owns the held left button");
		};
		const auto freshClick = [&] {
			clicked = false;
			SDL_WarpMouseInWindow(engine.window, control.x, control.y);
			motion(control.x, control.y);
			button(SDL_EVENT_MOUSE_BUTTON_DOWN, control.x, control.y);
			present(floating);
			button(SDL_EVENT_MOUSE_BUTTON_UP, control.x, control.y);
			present(floating);
			CHECK(clicked, "fresh Developer UI click activates after cancellation");
		};
		windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
		present(floating);
		present(floating);
		startDrag();
		const bool actualFocus = SDL_GetKeyboardFocus() == engine.window;
#if defined(_WIN32)
		if (actualFocus)
			CHECK((SDL_GetWindowFlags(engine.window) & SDL_WINDOW_MOUSE_CAPTURE) != 0,
				"focused SDL window has OS mouse capture during the developer drag");
#else
		(void)actualFocus;
#endif
		windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST);
		engine.handleEvents();
		const int releases = queuedReleases();
		button(SDL_EVENT_MOUSE_BUTTON_UP, control.x, control.y);
		windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
		engine.handleEvents();
		const auto &imguiContext = *ImGui::GetCurrentContext();
		bool lost = false, regained = false;
		for (const auto &event : imguiContext.InputEventsQueue)
			if (event.Type == ImGuiInputEventType_Focus)
			{
				lost |= !event.AppFocused.Focused;
				regained |= lost && event.AppFocused.Focused;
			}
		CHECK(lost && regained && !imguiContext.MovingWindow && !ImGui::GetIO().MouseDown[0] &&
			!(SDL_GetWindowFlags(engine.window) & SDL_WINDOW_MOUSE_CAPTURE) && releases == queuedReleases() && !clicked,
			"focus loss and gain survive cancellation and the late release activates nothing");
		present(floating);
		present(floating);
		freshClick();

		if (native)
		{
			startDrag();
			auto &sample = *engine.m_playerUi;
			checked(sample.m_screens.push(gameui::Screen::ConfirmExit), "suppress developer UI during drag");
			sample.invalidateLayout();
			engine.syncUiInput();
			const int modalReleases = queuedReleases();
			button(SDL_EVENT_MOUSE_BUTTON_UP, control.x, control.y);
			engine.handleEvents();
			CHECK(!imguiContext.MovingWindow && !ImGui::GetIO().MouseDown[0] &&
				!(SDL_GetWindowFlags(engine.window) & SDL_WINDOW_MOUSE_CAPTURE) &&
				engine.m_inputRouter.pointerOwner() == gameui::InputOwner::None && modalReleases == queuedReleases(),
				"native modal cancels the developer drag, OS capture and the late release");
			sample.back();
			engine.syncUiInput();
			present(floating);
			present(floating);
			freshClick();
		}
	}

	void pointerProof()
	{
		auto &sample = *engine.m_playerUi;
		engine.setMouseCaptured(false);
		const auto buttonCenter = [&] {
			const auto r = sample.m_button;
			return glm::vec2(r.x + r.z, r.y + r.w) * 0.5f;
		};
		auto pos = buttonCenter();
		SDL_WarpMouseInWindow(engine.window, pos.x, pos.y);
		frames(2);
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, pos.x, pos.y);
		button(SDL_EVENT_MOUSE_BUTTON_UP, pos.x, pos.y);
		present();
		CHECK(sample.m_activations == 4, "the scale button accepts one click while ImGui is active");
		CHECK(sample.lastRecordedStats() && sample.lastRecordedStats()->frameNumber == engine.frameNumber - 1,
			"UI stats identify the last recorded frame, regardless of frame slot");
		pos = buttonCenter();

		bool floatingClicked = false;
		const auto floating = [&] {
			ImGui::SetNextWindowPos({pos.x - 50.f, pos.y - 30.f}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({100.f, 60.f}, ImGuiCond_Always);
			if (ImGui::Begin("Proof floating window", nullptr,
				ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
				floatingClicked |= ImGui::Button("Occluding", {84.f, 44.f});
			ImGui::End();
		};
		SDL_WarpMouseInWindow(engine.window, pos.x, pos.y + 140.f);
		present(floating);
		present(floating);
		CHECK(!engine.imgui->wantCaptureMouse(), "previous ImGui frame saw the pointer over the scene");
		SDL_WarpMouseInWindow(engine.window, pos.x, pos.y);
		SDL_Event motion{};
		motion.type = SDL_EVENT_MOUSE_MOTION;
		motion.motion.windowID = SDL_GetWindowID(engine.window);
		motion.motion.x = pos.x;
		motion.motion.y = pos.y;
		checked(SDL_PushEvent(&motion), "queue pointer movement across the floating window");
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, pos.x, pos.y);
		tap(SDLK_P);
		present(floating);
		button(SDL_EVENT_MOUSE_BUTTON_UP, pos.x, pos.y);
		present(floating);
		present(floating);
		CHECK(sample.m_activations == 4 && floatingClicked,
			"a click before NewFrame belongs to the ImGui window covering Game UI");
		CHECK(sample.screens().top() == gameui::Screen::Hud,
			"a gameplay shortcut queued with the developer press cannot open the pause menu");

		SDL_WarpMouseInWindow(engine.window, pos.x, pos.y);
		frames(2);
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, pos.x, pos.y);
		engine.handleEvents();
		CHECK(sample.m_armed && engine.m_inputRouter.pointerOwner() == gameui::InputOwner::PlayerUi,
			"Game UI owns the uncovered press");
		engine.setMouseCaptured(true);
		CHECK(engine.mouseCaptured && !sample.m_armed && engine.m_inputRouter.pointerOwner() == gameui::InputOwner::None,
			"gameplay capture cancels activation and preserves release ownership");
		const int releasesBeforeCapture = queuedReleases();
		engine.setMouseCaptured(false);
		button(SDL_EVENT_MOUSE_BUTTON_UP, pos.x, pos.y);
		engine.handleEvents();
		CHECK(!engine.mouseCaptured && !sample.m_armed && sample.m_activations == 4 &&
			queuedReleases() == releasesBeforeCapture,
			"releasing the cursor does not rearm or deliver the cancelled press to ImGui");

		SDL_WarpMouseInWindow(engine.window, pos.x, pos.y);
		frames(2);
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, pos.x, pos.y);
		engine.handleEvents();
		CHECK(sample.m_armed, "a fresh press can arm before a dialog appears");
		present([] {
			ImGui::OpenPopup("Proof dialog");
			if (ImGui::BeginPopupModal("Proof dialog"))
			{
				ImGui::TextUnformatted("Game UI pointer proof");
				ImGui::EndPopup();
			}
		});
		CHECK(!sample.m_armed && engine.m_inputRouter.pointerOwner() == gameui::InputOwner::None,
			"opening a dialog between SDL events cancels activation");
		present([] {
			if (ImGui::BeginPopupModal("Proof dialog"))
			{
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
		});
		const int releasesBeforeDialog = queuedReleases();
		button(SDL_EVENT_MOUSE_BUTTON_UP, pos.x, pos.y);
		engine.handleEvents();
		CHECK(!sample.m_armed && sample.m_activations == 4 && queuedReleases() == releasesBeforeDialog,
			"closing the dialog keeps the cancelled release with its original owner");

		const auto overlay = [&](bool passThrough) {
			ImGui::SetNextWindowPos({pos.x - 52.f, pos.y - 30.f}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({104.f, 60.f}, ImGuiCond_Always);
			if (ImGui::Begin(passThrough ? "Pass through proof" : "Passive cover proof", nullptr,
				ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
				ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing))
			{
				if (passThrough)
					ImGuiLayer::markCurrentWindowPassThrough();
				ImGui::TextUnformatted("Status");
			}
			ImGui::End();
		};
		present([&] { overlay(false); });
		CHECK(engine.developerPointerAt(pos.x, pos.y) == ImGuiLayer::PointerHit::Occludes,
			"passive overlay shields its visible rectangle");
		clickAt(pos);
		present([&] { overlay(false); });
		CHECK(sample.m_activations == 4 && !engine.imgui->pendingKeyboard(),
			"passive overlay consumes neither a native click nor developer keyboard ownership");
		present([&] { overlay(true); });
		CHECK(engine.developerPointerAt(pos.x, pos.y) == ImGuiLayer::PointerHit::PassThrough,
			"marked HUD overlay leaves the lower surface reachable");
		clickAt(pos);
		present([&] { overlay(true); });
		CHECK(sample.m_activations == 5, "pass-through HUD permits the native scale click");
	}

	void legacyUnfocusProof()
	{
		char value[32]{};
		const auto editor = [&] {
			ImGui::SetNextWindowPos({45.f, 210.f}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({190.f, 90.f}, ImGuiCond_Always);
			if (ImGui::Begin("Legacy unfocus proof", nullptr, ImGuiWindowFlags_NoDecoration |
				ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings))
				ImGui::InputText("##legacy", value, sizeof(value));
			ImGui::End();
		};
		present(editor);
		clickAt({86.f, 225.f});
		present(editor);
		present(editor);
		CHECK(engine.imgui->editingText(), "legacy fixture activates a real ImGui editor");
		CHECK(engine.developerPointerAt(500.f, 330.f) == ImGuiLayer::PointerHit::PassThrough,
			"legacy game view is clear at the click-to-unfocus target");
		commitText("before");
		clickAt({500.f, 330.f});
		key(SDLK_W, SDL_EVENT_KEY_DOWN);
		commitText("ignored");
		engine.handleEvents();
		CHECK(!engine.imgui->editingText() && engine.m_inputRouter.gameplayKey(SDL_SCANCODE_W),
			"legacy gameplay click releases the editor and restores keyboard routing in the batch");
		key(SDLK_W, SDL_EVENT_KEY_UP);
		for (int frame = 0; frame < 4; ++frame)
			present(editor);
		CHECK(std::string_view(value) == "before" && !engine.imgui->editingText() && !SDL_TextInputActive(engine.window),
			"legacy editor commits only its pre-click text and relinquishes SDL");
	}

	void presentProof(const std::filesystem::path &out)
	{
		init();
		const auto &context = *engine.vkContext;
		const auto before = context.validationErrorCount();
		const auto rtssBefore = context.rtssValidationErrorCount();
		engine.gameUi->shell().queueDefaultLayout();
		engine.gameUi->enableDefaultDeveloperPanels();
		resize(1920, 1080);
		frames(16);
		resize(1160, 680);
		frames(16);
		checked(SDL_MinimizeWindow(engine.window) && SDL_SyncWindow(engine.window) &&
			(SDL_GetWindowFlags(engine.window) & SDL_WINDOW_MINIMIZED), "minimize Game UI proof window");
		engine.handleEvents();
		checked(SDL_RestoreWindow(engine.window) && SDL_SyncWindow(engine.window) &&
			!(SDL_GetWindowFlags(engine.window) & SDL_WINDOW_MINIMIZED), "restore Game UI proof window");
		frames(16);
		resize(1680, 720);
		frames(16);

		// Use a separate partial pack with a conspicuous replacement icon.
		visual::RgbaImage icon{16, 16, std::vector<uint8_t>(16 * 16 * 4, 255)};
		for (size_t i = 0; i < icon.pixels.size(); i += 4)
			icon.pixels[i + 1] = 32;
		checked(visual::writePng((out / "pack/assets/minecraft/textures/block/grass_block_side.png").string(), icon),
			"write Game UI resource-pack fixture");
		auto &sample = *engine.m_playerUi;
		const auto baseline = sample.renderer().stats(0);
		for (unsigned i = 0; i < 4; ++i)
		{
			resize(i % 2 ? 1680 : 1160, i % 2 ? 720 : 680);
			const auto result = engine.applyResourcePack(i % 2 ? (out / "pack").string() : "");
			CHECK(result.atlasOk && (i % 2 == 0 || result.packHits > 0), "pack published a new atlas");
			sample.m_activations = i;
			// The player scale is its own policy now (issue #302); the reload
			// proof still exercises four distinct scales with it.
			sample.setScalePolicy({gameui::ScaleMode::Fixed, gameui::kUserScales[i]});
			frames(4);
			const auto stats = sample.renderer().stats(0);
			CHECK(stats.descriptorSets == baseline.descriptorSets && stats.instanceBytes == baseline.instanceBytes &&
				stats.fontBytes == baseline.fontBytes && stats.compositionBytes == baseline.compositionBytes &&
				stats.draws <= 8 && stats.dropped == 0, "resize, scale and reload retain a bounded UI allocation footprint");
		}
		pointerProof();
		developerDragProof(true);
		auto native = std::move(engine.m_playerUi);
		developerDragProof(false);
		legacyUnfocusProof();
		engine.m_playerUi = std::move(native);
		context.waitIdle();
		if (context.isValidationEnabled())
		{
			const auto errors = context.validationErrorCount() - before;
			const auto rtssErrors = context.rtssValidationErrorCount() - rtssBefore;
			CHECK(errors == rtssErrors, "WSI validation errors=" << errors << ", known RTSS errors=" << rtssErrors);
			std::cout << "WSI validation errors=" << errors << " known_rtss_errors=" << rtssErrors << '\n';
		}
		else
			std::cout << "WSI validation unavailable or disabled for this run\n";
	}

	// Player design-system evidence (issue #302): the gallery and the HUD mock
	// captured from the production composition path at representative
	// resolutions and player UI scales. Every capture is also checked for a
	// bounded draw/quad budget and a clean validation counter.
	void designSystem(const std::filesystem::path &out)
	{
		init();
		auto &device = *engine.vkContext;
		const auto before = device.validationErrorCount();
		const auto rtssBefore = device.rtssValidationErrorCount();
		auto &world = *engine.worldRenderer;
		auto &sample = *engine.m_playerUi;
		struct Case
		{
			const char *name;
			int width, height;
			gameui::ScalePolicy policy;
			gameui::Screen screen;
		};
		const Case cases[] = {
			{"gallery-1920x1080-100", 1920, 1080, {gameui::ScaleMode::Fixed, 1.f}, gameui::Screen::Gallery},
			{"gallery-2560x1080-100", 2560, 1080, {gameui::ScaleMode::Fixed, 1.f}, gameui::Screen::Gallery},
			{"gallery-1160x680-100", 1160, 680, {gameui::ScaleMode::Fixed, 1.f}, gameui::Screen::Gallery},
			{"gallery-1920x1080-200", 1920, 1080, {gameui::ScaleMode::Fixed, 2.f}, gameui::Screen::Gallery},
			{"hud-1920x1080-100", 1920, 1080, {gameui::ScaleMode::Fixed, 1.f}, gameui::Screen::HudMock},
			{"hud-2560x1080-150", 2560, 1080, {gameui::ScaleMode::Fixed, 1.5f}, gameui::Screen::HudMock},
			{"hud-960x540-100", 960, 540, {gameui::ScaleMode::Fixed, 1.f}, gameui::Screen::HudMock},
		};
		std::filesystem::create_directories(out);
		for (const auto &test : cases)
		{
			resize(test.width, test.height);
			frames(2);
			// Drive the real screen stack the pause menu uses; the scale policy
			// is applied at the next frame boundary, never mid-frame.
			sample.resetScreens();
			sample.setScalePolicy(test.policy);
			if (test.screen == gameui::Screen::Gallery)
				sample.openGallery();
			else
				sample.openScreen(test.screen);
			frames(3);
			const auto extent = this->extent();
			CHECK(sample.screens().top() == test.screen,
				"the design-system screen is open for " << test.name);
			CHECK(std::abs(sample.playerUiScale() - sample.scalePolicy().value) < 0.001f,
				"the resolved player UI scale matches the requested policy for " << test.name
					<< " (resolved=" << sample.playerUiScale() << ')');
			if (test.screen == gameui::Screen::Gallery)
			{
				// R10: the gallery's destructive confirmation sample uses the same
				// danger variant as the real exit flow.
				const gameui::Widget *danger = nullptr, *cancel = nullptr;
				for (const auto &item : sample.m_widgets.widgets())
				{
					if (item.label == "Continue")
						danger = &item;
					if (item.label == "Cancel")
						cancel = &item;
				}
				CHECK(danger && cancel &&
						danger->variant == gameui::style::ButtonVariant::Danger &&
						cancel->variant == gameui::style::ButtonVariant::Secondary,
					"the gallery confirmation styles its destructive action as danger for " << test.name);
			}

			// Capture in the live swapchain format so subsequent window frames
			// always use pipelines compatible with their attachments.
			const auto format = engine.swapchain->getImageFormat();
			Readback target{device};
			const auto allocator = device.getAllocator();
			target.output = createImage2D(allocator, device.getDevice(), extent.width, extent.height,
				format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			target.buffer = createBuffer(allocator, extent.width * extent.height * 4,
				VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
				VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
			sample.prepare(engine.window, 0, extent, {}, world.getTextureManager(),
				engine.m_atlasVersion);
			const auto stats = sample.renderer().stats(0);
			const auto image = capture(target, &sample.renderer());
			CHECK(image.width == extent.width && image.height == extent.height,
				"the capture keeps the requested extent for " << test.name);
			CHECK(stats.dropped == 0 && stats.draws <= 32,
				"the design-system screen stays inside the 32-draw menu budget for " << test.name
					<< " (draws=" << stats.draws << ", dropped=" << stats.dropped << ')');
			std::string file = std::string(test.name) + ".png";
			checked(visual::writePng((out / file).string(), image), "write design-system screenshot");
		}
		// F16: a scale change rebuilds geometry without behaving like a screen
		// re-entry. Focus, the selected slot and the notification queue survive.
		sample.resetScreens();
		sample.setScalePolicy({gameui::ScaleMode::Fixed, 1.f});
		sample.openGallery();
		frames(3);
		const auto focus = sample.screens().focus();
		const auto slot = sample.m_gallerySlot;
		const auto toastCount = sample.m_toasts.size();
		const auto toastId = sample.m_toasts.newest() ? sample.m_toasts.newest()->id : 0u;
		CHECK(focus != 0 && toastId != 0 && toastCount > 0,
			"the gallery has focus, a selected slot and a notification before the scale change");
		sample.setScalePolicy({gameui::ScaleMode::Fixed, 1.5f});
		frames(3);
		CHECK(std::abs(sample.playerUiScale() - 1.5f) < 0.001f,
			"the in-session scale change is applied at the next frame boundary");
		CHECK(sample.screens().focus() == focus && sample.m_gallerySlot == slot &&
				sample.m_toasts.size() == toastCount && sample.m_toasts.newest() &&
				sample.m_toasts.newest()->id == toastId,
			"changing the player UI scale keeps focus, selection and notifications");

		// S03: dynamic atlas replacement on the real GPU path — invalid input is
		// refused without touching the working atlas, a valid density change swaps
		// the image and its CPU metrics together, and repeated round-trips return
		// to the same footprint.
		{
			auto &renderer = sample.renderer();
			const auto baseline = renderer.stats(0);
			const auto baselinePixels = sample.m_facePixels;
			const auto baselineFontScale = sample.m_fontScale;
			CHECK(baseline.draws > 0 && baseline.dropped == 0,
				"the proof renders with the original atlas before any replacement");
			const bool refusedEmpty = renderer.setFont(*sample.m_immediate, {0, 0}, {});
			const std::array<uint8_t, 16> wrongSpan{};
			const bool refusedSpan = renderer.setFont(*sample.m_immediate, {8, 8}, wrongSpan);
			CHECK(!refusedEmpty && !refusedSpan,
				"an invalid atlas replacement reports failure instead of publishing");
			frames(2);
			const auto afterFailure = renderer.stats(0);
			CHECK(afterFailure.fontBytes == baseline.fontBytes && afterFailure.draws > 0 &&
					afterFailure.dropped == 0 && std::abs(sample.m_facePixels - baselinePixels) < 0.001f,
				"the original atlas stays usable after a failed replacement");
			const float other = baselinePixels < 1.5f ? 2.f : 1.f;
			sample.setFaceDensityOverride(other);
			frames(2);
			const auto replaced = renderer.stats(0);
			CHECK(std::abs(sample.m_facePixels - other) < 0.001f && replaced.draws > 0 &&
					replaced.dropped == 0 && replaced.descriptorSets == baseline.descriptorSets &&
					replaced.fontBytes == VkDeviceSize(sample.m_face.width) * sample.m_face.height,
				"a valid atlas replacement publishes matching GPU and CPU metrics");
			CHECK(sample.m_fontScale != baselineFontScale,
				"the framebuffer compensation follows the new density");
			for (int round = 0; round < 3; ++round)
			{
				sample.setFaceDensityOverride(baselinePixels);
				frames(1);
				sample.setFaceDensityOverride(other);
				frames(1);
			}
			sample.setFaceDensityOverride(std::nullopt);
			sample.refreshFaceForDensity(baselinePixels);
			frames(2);
			const auto restored = renderer.stats(0);
			CHECK(std::abs(sample.m_facePixels - baselinePixels) < 0.001f &&
					sample.m_fontScale == baselineFontScale && restored.fontBytes == baseline.fontBytes &&
					restored.descriptorSets == baseline.descriptorSets && restored.draws > 0,
				"repeated atlas replacements leak no font or descriptor resources");
		}

		// The gallery is a temporary presentation preview. Exercise the actual
		// scale button and leave through both Back and an epoch-style screen reset.
		{
			namespace fs = std::filesystem;
			const auto previousRoot = engine.m_savesRoot;
			const auto root = out / "gallery-preferences";
			engine.setSavesRootForTests(root);
			const auto record = root / gameui::kPlayerSettingsPath;
			gameui::PlayerUiSettings saved;
			saved.scale = {gameui::ScaleMode::Fixed, 1.f};
			saved.window = {960, 540};
			checked(saved.save(record), "seed gallery preference fixture");
			const auto bytes = readText(record);
			engine.loadPlayerUiPreferences();
			const auto previewScale = [&] {
				sample.openGallery();
				frames(2);
				const auto widgets = sample.m_widgets.widgets();
				const auto scaleButton = std::ranges::find(widgets, std::string_view{"150%"}, &gameui::Widget::label);
				checked(scaleButton != widgets.end(), "gallery exposes its scale selector");
				sample.applyGalleryActivate(scaleButton->id);
				frames(2);
				CHECK(sample.playerUiScale() == 1.5f && readText(record) == bytes &&
						engine.m_playerUiSettings == saved, "gallery scale changes only presentation");
			};
			sample.resetScreens();
			previewScale();
			sample.back();
			frames(2);
			CHECK(sample.scalePolicyApplied(saved.scale) && readText(record) == bytes,
				"leaving the gallery restores the original scale and leaves the file intact");
			previewScale();
			sample.resetScreens();
			frames(2);
			CHECK(sample.scalePolicyApplied(saved.scale) && readText(record) == bytes,
				"resetting the screen stack also restores the original scale");
			engine.setSavesRootForTests(previousRoot);
		}

		sample.resetScreens();
		sample.setScalePolicy({gameui::ScaleMode::Auto, 1.f});
		frames(2);
		device.waitIdle();
		const auto errors = device.validationErrorCount() - before;
		const auto rtss = device.rtssValidationErrorCount() - rtssBefore;
		CHECK(errors == rtss, "design-system capture validation errors=" << errors
			<< ", known RTSS errors=" << rtss);
	}

	void playerSettingsProof(const std::filesystem::path &out)
	{
		using namespace gameui;
		namespace fs = std::filesystem;
		ImGui::GetIO().IniFilename = nullptr;
		engine.setShowDeveloperUi(false);
		engine.setSavesRootForTests(out / "saves");
		engine.enableGameUi(false, false);
		engine.running = true;
		engine.showMainMenu();
		auto &sample = *engine.m_playerUi;
		auto &device = *engine.vkContext;
		const auto errorsBefore = device.validationErrorCount();
		const auto rtssBefore = device.rtssValidationErrorCount();
		auto record = engine.m_savesRoot / kPlayerSettingsPath;
		const auto legacy = engine.m_savesRoot / kLegacyPlayerSettingsPath;
		engine.loadPlayerUiPreferences();
		CHECK(engine.m_playerUiSettings == PlayerUiSettings{} && !engine.m_playerUiDirty &&
				!fs::exists(record), "missing current and legacy files use defaults silently");
		fs::create_directories(legacy.parent_path());
		std::ofstream(legacy, std::ios::binary) << kLegacyPlayerUiSettingsHeader << "\nscale fixed 1.00\n";
		const auto legacyBytes = readText(legacy);
		engine.loadPlayerUiPreferences();
		CHECK(engine.m_playerUiSettings.scale == (ScalePolicy{ScaleMode::Fixed, 1.f}) && !fs::exists(record),
			"the legacy player scale loads without a migration write");
		fs::create_directories(record.parent_path());
		std::ofstream(record, std::ios::binary) << legacyBytes;
		engine.loadPlayerUiPreferences();
		CHECK(engine.m_playerUiSettings == PlayerUiSettings{} && engine.m_playerUiDirty &&
				readText(record) == legacyBytes && readText(legacy) == legacyBytes,
			"a legacy record at the current path is corrupt and blocks legacy fallback without a write");
		fs::remove(record);
		std::ofstream(legacy) << "broken legacy record\n";
		engine.loadPlayerUiPreferences();
		CHECK(engine.m_playerUiSettings == PlayerUiSettings{} && engine.m_playerUiDirty && !fs::exists(record),
			"a corrupt legacy record is reported without creating a current record");
		std::ofstream(legacy, std::ios::binary) << legacyBytes;
		engine.loadPlayerUiPreferences();
		engine.imgui->requestUiScale(1.5f);
		resize(960, 540);
		windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
		present();

		const auto click = [&](std::string_view label) {
			for (const auto &widget : sample.m_widgets.widgets())
				if (widget.enabled && widget.label.starts_with(label))
				{
					const auto rect = sample.m_widgets.bounds(widget).rect;
					clickAt({(rect.x + rect.z) * 0.5f, (rect.y + rect.w) * 0.5f});
					present();
					return;
				}
			throw std::runtime_error("missing enabled Settings control: " + std::string(label));
		};
		const auto shot = [&](const char *name) {
			device.waitIdle();
			SDL_WarpMouseInWindow(engine.window, 4.f, 4.f);
			SDL_PumpEvents();
			const auto size = engine.swapchain->getExtent();
			const auto format = engine.swapchain->getImageFormat();
			const auto allocator = device.getAllocator();
			Readback target{device};
			target.output = createImage2D(allocator, device.getDevice(), size.width, size.height, format,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			target.buffer = createBuffer(allocator, size.width * size.height * 4,
				VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO, VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
				VMA_ALLOCATION_CREATE_MAPPED_BIT);
			sample.prepare(engine.window, 0, size, {}, engine.worldRenderer->getTextureManager(), engine.m_atlasVersion);
			const auto image = capture(target, &sample.renderer());
			checked(visual::writePng((out / name).string(), image), "write Settings screenshot");
		};
		const auto stageDisplay = [&] {
			click("Window size");
			click("UI scale");
			click("Preview display changes");
			SDL_SyncWindow(engine.window);
			present();
		};
		const auto minimize = [&] {
			checked(SDL_MinimizeWindow(engine.window) && SDL_SyncWindow(engine.window), "minimize Settings window");
			windowEvent(SDL_EVENT_WINDOW_MINIMIZED);
			present();
			checked(engine.playerFramebufferSize() == WindowSize{}, "framebuffer unavailable while minimized");
		};
		const auto restore = [&] {
			checked(SDL_RestoreWindow(engine.window) && SDL_SyncWindow(engine.window), "restore Settings window");
			windowEvent(SDL_EVENT_WINDOW_RESTORED);
			windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
			present();
			SDL_SyncWindow(engine.window);
			frames(2);
		};
		tap(SDLK_TAB);
		tap(SDLK_TAB);
		tap(SDLK_RETURN);
		present();
		CHECK(sample.screens().top() == Screen::Settings && engine.suppressDeveloperUi(),
			"keyboard opens Settings from the world-free menu");
		shot("settings.png");
		click("Controls reference");
		CHECK(sample.screens().top() == Screen::Controls, "the controls reference opens natively");
		tap(SDLK_ESCAPE);
		present();

		stageDisplay();
		CHECK(sample.m_settingsView.display && sample.m_settingsView.display->phase == DisplayPhase::Confirming &&
				sample.m_settingsView.window == (WindowSize{1280, 720}) && sample.playerUiScale() == 1.25f &&
				!fs::exists(record), "SDL resize, swapchain and scale are applied before the preview can be kept");
		CHECK(engine.imgui->uiScale() == 1.5f && !engine.developerWorkspaceVisible(),
			"resizing and previewing the player scale preserve developer scale and workspace visibility");
		shot("resize-preview.png");
		tap(SDLK_ESCAPE);
		present();
		SDL_SyncWindow(engine.window);
		present();
		CHECK(sample.screens().top() == Screen::Settings && !sample.m_settingsView.display &&
				sample.m_settingsView.window == (WindowSize{960, 540}) && sample.playerUiScale() == 1.f &&
				!fs::exists(record), "Escape restores both settings without a disk edit");

		click("Window size");
		click("UI scale");
		auto beforeFailure = engine.frameNumber;
		{
			SwapchainCreationFailure refusal(VK_ERROR_INITIALIZATION_FAILED);
			click("Preview display changes");
			if (refusal.calls == 0)
			{
				SDL_SyncWindow(engine.window);
				beforeFailure = engine.frameNumber;
				present();
			}
			CHECK(refusal.calls == 1 && !engine.swapchain->getSwapchain() &&
					engine.m_playerDisplayPreview && engine.m_playerDisplayPreview->recoveringSwapchain &&
					engine.frameNumber == beforeFailure && !fs::exists(record),
				"a rejected resize after WSI retirement skips acquisition and enters rollback");
		}
		present();
		SDL_SyncWindow(engine.window);
		frames(2);
		CHECK(!sample.m_settingsView.display && sample.m_settingsView.window == (WindowSize{960, 540}) &&
				sample.playerUiScale() == 1.f && engine.swapchain->isVSync() && engine.frameNumber > beforeFailure &&
				!fs::exists(record) && sample.m_settingsView.notice.find("Previous display settings restored") != std::string::npos,
			"recovery refreshes swapchain consumers and verifies a successful rollback before reporting it");

		engine.beginPlayerDisplayPreview(WindowSize{1280, 720}, std::nullopt);
		SDL_SyncWindow(engine.window);
		const auto fatalExtent = extent();
		{
			SwapchainCreationFailure fatal(VK_ERROR_DEVICE_LOST);
			bool propagated = false;
			try
			{
				engine.recreateSwapchainIfNeeded(fatalExtent.width, fatalExtent.height);
			}
			catch (const SwapchainConfigurationError &)
			{
			}
			catch (const std::runtime_error &)
			{
				propagated = true;
			}
			CHECK(fatal.calls == 1 && propagated && !engine.swapchain->getSwapchain(),
				"a fatal creation error propagates without retrying as a configuration refusal");
		}
		// The injected device loss never reached the driver. Reset the fixture.
		engine.revertPlayerDisplayPreview();
		present();
		SDL_SyncWindow(engine.window);
		frames(2);

		stageDisplay();
		checked(engine.m_playerDisplayPreview && engine.m_playerDisplayPreview->view.phase == DisplayPhase::Confirming,
			"start the minimized confirmation timeout test");
		const auto confirmationDeadline = engine.m_playerDisplayPreview->deadline;
		minimize();
		checked(engine.m_playerDisplayPreview.has_value(), "minimized confirmation stays in flight");
		CHECK(engine.m_playerDisplayPreview->deadline == confirmationDeadline && !fs::exists(record),
			"minimizing does not extend confirmation or write preferences");
		engine.m_playerDisplayPreview->deadline = {};
		present();
		checked(engine.m_playerDisplayPreview.has_value(), "expired confirmation keeps the rollback transaction");
		CHECK(engine.m_playerDisplayPreview->view.phase == DisplayPhase::Restoring &&
				!engine.m_playerDisplayPreview->restoreCheck && !fs::exists(record) &&
				engine.m_playerSettingsNotice.find("restored") == std::string::npos,
			"confirmation expires while minimized and postpones verification");
		restore();
		CHECK(!sample.m_settingsView.display && sample.m_settingsView.window == (WindowSize{960, 540}) &&
				sample.playerUiScale() == 1.f && !fs::exists(record) &&
				sample.m_settingsView.notice.find("expired") != std::string::npos,
			"restoring the window verifies the expired preview was rolled back");
		stageDisplay();
		tap(SDLK_ESCAPE);
		present();
		checked(engine.m_playerDisplayPreview && engine.m_playerDisplayPreview->view.phase == DisplayPhase::Restoring,
			"rollback waits for the previous scale frame");
		engine.m_playerDisplayPreview->restoreCheck = std::chrono::steady_clock::now() - std::chrono::seconds(10);
		minimize();
		CHECK(engine.m_playerDisplayPreview && !engine.m_playerDisplayPreview->restoreCheck && !fs::exists(record),
			"an interrupted rollback stays live and pauses the visible verification clock");
		sample.m_settingsRequest = PreviewDisplay{WindowSize{1600, 900}, std::nullopt};
		present();
		CHECK(engine.m_playerDisplayPreview && engine.m_playerDisplayPreview->view.phase == DisplayPhase::Restoring &&
				engine.m_playerDisplayPreview->view.window == (WindowSize{960, 540}),
			"a pending rollback rejects a second display transaction");
		restore();
		CHECK(!sample.m_settingsView.display && sample.m_settingsView.window == (WindowSize{960, 540}) &&
				sample.playerUiScale() == 1.f && !fs::exists(record) && sample.m_settingsView.notice.empty(),
			"the interrupted rollback verifies on restore without reporting a false failure");
		stageDisplay();
		tap(SDLK_TAB, SDL_KMOD_SHIFT);
		tap(SDLK_RETURN);
		present();
		PlayerUiSettings saved;
		CHECK(saved.load(record) == LoadStatus::Ok && saved.window == (WindowSize{1280, 720}) &&
				saved.scale == (ScalePolicy{ScaleMode::Fixed, 1.25f}) && fs::exists(legacy),
			"keyboard confirms the preview, persisting it separately from legacy scale and credentials");
		click("Anti-aliasing");
		click("Mouse sensitivity");
		click("Invert vertical");
		const auto support = VkSwapchain::querySupport(device.getPhysicalDevice(), device.getSurface());
		const bool immediate = std::ranges::find(support.presentModes, VK_PRESENT_MODE_IMMEDIATE_KHR) !=
			support.presentModes.end();
		click("VSync");
		CHECK(engine.swapchain->isVSync() == !immediate && !engine.m_pendingVSync,
			"VSync changes only at the swapchain boundary, with the working mode retained on refusal");
		if (!immediate)
			CHECK(!engine.m_playerSettingsNotice.empty(), "an unavailable present mode is reported");
		CHECK(saved.load(record) == LoadStatus::Ok && !saved.fxaa && saved.sensitivity == 125 &&
				saved.invertY && saved.vsync == !immediate, "all changes are persisted through the native controls");
		shot("settings-applied.png");
		const auto savedBytes = readText(record);
		// Leave the original record in its original location for the next client
		// process. Use a separate copy for the destructive retry and reset cases.
		const auto restartRecord = record;
		engine.setSavesRootForTests(out / "continued" / "saves");
		record = engine.m_savesRoot / kPlayerSettingsPath;
		fs::create_directories(record.parent_path());
		fs::copy_file(restartRecord, record);

		const auto backup = record.parent_path() / "previous-record.txt";
		fs::rename(record, backup);
		fs::create_directory(record);
		click("Invert vertical");
		CHECK(engine.m_playerUiDirty && !engine.m_playerUiSettings.invertY && readText(backup) == savedBytes,
			"a blocked save leaves the new value active and preserves the previous record");
		fs::remove(record);
		fs::rename(backup, record);
		click("Retry saving preferences");
		PlayerUiSettings retried;
		CHECK(!engine.m_playerUiDirty && retried.load(record) == LoadStatus::Ok && !retried.invertY,
			"an explicit retry persists the pending value after the destination recovers");
		click("Invert vertical");
		CHECK(readText(record) == savedBytes, "return to the original preferences before testing reset");

		click("Reset to defaults");
		SDL_SyncWindow(engine.window);
		frames(2);
		checked(sample.m_settingsView.display && sample.m_settingsView.display->phase == DisplayPhase::Confirming,
			"multi-preference reset previews both display settings");
		tap(SDLK_ESCAPE);
		present();
		SDL_SyncWindow(engine.window);
		frames(2);
		CHECK(!sample.m_settingsView.display && readText(record) == savedBytes &&
				engine.m_playerUiSettings == saved && sample.m_settingsView.window == saved.window &&
				sample.scalePolicyApplied(saved.scale), "cancelling reset keeps every existing preference");

		// Exercise the actual startup loader against the on-disk record after changing the runtime values.
		resize(960, 540);
		engine.setVSync(true);
		engine.worldRenderer->postSettings().fxaaEnabled = true;
		engine.camera.setMouseSensitivity(0.1f);
		present();
		engine.loadPlayerUiPreferences();
		present();
		CHECK(sample.m_settingsView.window == saved.window && sample.scalePolicy() == saved.scale &&
				sample.m_settingsView.vsync == saved.vsync && sample.m_settingsView.fxaa == saved.fxaa &&
				sample.m_settingsView.sensitivity == saved.sensitivity && sample.m_settingsView.invertY,
			"the startup loader restores the complete record");
		engine.setPlayerSettingsOverrides(true, true);
		engine.setVSync(true);
		engine.worldRenderer->postSettings().fxaaEnabled = true;
		engine.loadPlayerUiPreferences();
		present();
		PlayerUiSettings afterOverrides;
		CHECK(sample.m_settingsView.vsync && sample.m_settingsView.fxaa && sample.m_settingsView.vsyncOverridden &&
				sample.m_settingsView.fxaaOverridden && afterOverrides.load(record) == LoadStatus::Ok &&
				afterOverrides == saved, "CLI overrides win for the session and do not rewrite the preferences");
		checked(SDL_MaximizeWindow(engine.window) && SDL_SyncWindow(engine.window), "maximize Settings window");
		frames(2);
		checked(!sample.m_settingsView.canResize, "a maximized window cannot be resized by Settings");
		click("Reset to defaults");
		frames(2);
		checked(sample.m_settingsView.display && !sample.m_settingsView.display->window,
			"maximized reset previews scale only");
		click("Keep changes");
		PlayerUiSettings partialReset;
		CHECK(partialReset.load(record) == LoadStatus::Ok && partialReset.window == saved.window &&
				partialReset.scale == ScalePolicy{} && partialReset.sensitivity == kDefaultSensitivity &&
				!partialReset.invertY && partialReset.vsync == saved.vsync && partialReset.fxaa == saved.fxaa &&
				sample.m_settingsView.vsync && sample.m_settingsView.fxaa &&
				sample.m_settingsView.notice.find("Restore the window") != std::string::npos &&
				sample.m_settingsView.notice.find("command-line overrides") != std::string::npos,
			"maximized reset keeps the saved window and CLI-locked preferences while resetting eligible values");
		restore();
		engine.setPlayerSettingsOverrides(false, false);
		engine.loadPlayerUiPreferences();
		present();
		CHECK(sample.m_settingsView.vsync == saved.vsync && sample.m_settingsView.fxaa == saved.fxaa,
			"normal startup application restores the saved VSync and FXAA choices");
		click("Reset to defaults");
		SDL_SyncWindow(engine.window);
		frames(2);
		checked(sample.m_settingsView.display.has_value(), "reset previews the default display settings");
		click("Keep changes");
		PlayerUiSettings defaults;
		CHECK(defaults.load(record) == LoadStatus::Ok && defaults == PlayerUiSettings{},
			"confirming reset restores every default");

		engine.renderSettings.maxRenderDistance = 128;
		engine.initializeNoiseGenerator(42);
		auto *solo = engine.sim.get();
		sim::InventorySnapshot inventory;
		solo->inventorySnapshot(inventory);
		windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
		frames(2);
		checked(engine.setMouseCaptured(true), "capture the camera for vertical look checks");
		const auto pitch = [&](bool inverted) {
			engine.handlePlayerSettingsRequest(SetInvertY{inverted});
			engine.camera.setYawPitch(-90.f, 0.f);
			SDL_PumpEvents();
			SDL_FlushEvent(SDL_EVENT_MOUSE_MOTION);
			SDL_Event event{};
			event.type = SDL_EVENT_MOUSE_MOTION;
			event.motion.windowID = SDL_GetWindowID(engine.window);
			event.motion.yrel = 12.f;
			checked(SDL_PushEvent(&event), "queue vertical mouse motion");
			engine.handleEvents();
			return engine.camera.getPitch();
		};
		const auto normalPitch = pitch(false);
		const auto invertedPitch = pitch(true);
		CHECK(std::abs(normalPitch + 1.2f) < 0.001f && std::abs(invertedPitch - 1.2f) < 0.001f,
			"inversion reverses real SDL mouse motion on the camera pitch");
		sample.openPause();
		engine.syncUiInput();
		present();
		click("Settings");
		CHECK(engine.simulationSuspended() && sample.screens().top() == Screen::Settings,
			"Settings also opens from solo pause");
		net::wire::PlayerKey key{};
		key.fill(17);
		engine.startRemote(std::make_unique<Engine::RemoteSession>(net::udp::parseAddress("127.0.0.1", 1),
			std::nullopt, key));
		sample.openPause();
		engine.syncUiInput();
		present();
		click("Settings");
		click("Mouse sensitivity");
		CHECK(engine.m_remote && !engine.nativePause() && engine.m_playerUiSettings.sensitivity == 125,
			"editing client preferences during a remote connection leaves the remote session running");
		engine.resumeSolo();
		sim::InventorySnapshot restored;
		engine.sim->inventorySnapshot(restored);
		CHECK(engine.sim.get() == solo && restored.slots == inventory.slots &&
				engine.camera.getMouseSensitivity() == cameraSensitivity(125),
			"returning to solo preserves player preferences and the suspended world's inventory");
		device.waitIdle();
		CHECK(device.validationErrorCount() - errorsBefore == device.rtssValidationErrorCount() - rtssBefore,
			"the Settings proof has no unexpected Vulkan validation errors");
	}

	void playerSettingsRestartProof(const std::filesystem::path &out)
	{
		using namespace gameui;
		ImGui::GetIO().IniFilename = nullptr;
		engine.setSavesRootForTests(out / "saves");
		const auto record = engine.m_savesRoot / kPlayerSettingsPath;
		const auto bytes = readText(record);
		PlayerUiSettings saved;
		checked(saved.load(record) == LoadStatus::Ok, "load the previous process's Settings proof fixture");
		engine.loadPlayerUiPreferences();
		engine.enableGameUi(false, false);
		engine.setShowDeveloperUi(false);
		engine.running = true;
		engine.showMainMenu();
		auto &sample = *engine.m_playerUi;
		sample.openScreen(Screen::Settings);
		windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
		frames(2);
		CHECK(saved.window == (WindowSize{1280, 720}) && saved.scale == (ScalePolicy{ScaleMode::Fixed, 1.25f}) &&
				saved.sensitivity == 125 && saved.invertY && !saved.fxaa,
			"restart fixture contains preferences changed in the previous process");
		CHECK(sample.m_settingsView.window == saved.window && sample.scalePolicyApplied(saved.scale) &&
				sample.m_settingsView.vsync == saved.vsync && sample.m_settingsView.fxaa == saved.fxaa &&
				engine.camera.getMouseSensitivity() == cameraSensitivity(saved.sensitivity) &&
				sample.m_settingsView.invertY == saved.invertY && readText(record) == bytes,
			"a fresh client process applies the persisted values without rewriting the record");
	}

	void playerInventoryProof()
	{
		using enum items::ItemId;
		init();
		auto &sample = *engine.m_playerUi;
		const auto &context = *engine.vkContext;
		const auto errorsBefore = context.validationErrorCount();
		const auto rtssBefore = context.rtssValidationErrorCount();
		engine.setDeveloperWorkspaceVisible(false);
		engine.sim->grantItems(Stone, 64);
		engine.sim->grantItems(Dirt, 5);
		engine.sim->grantItems(Stone, 12);
		engine.sim->grantItems(GrassBlock, 17);
		sample.setScalePolicy({gameui::ScaleMode::Fixed, 1.f});
		const auto snapshot = [&] { return engine.playerInventoryView().snapshot->slots; };
		const auto middle = [](gameui::Clip rect) { return glm::vec2(rect.rect) + gameui::size(rect) * 0.5f; };
		const auto cell = [&](int index) { return middle(sample.m_inventoryLayout.slot({index})); };
		const auto clickSlot = [&](int index, Uint8 button = SDL_BUTTON_LEFT) { clickAt(cell(index), button); };
		const auto held = [&] { return sample.m_inventoryInteraction.drag().active(); };
		const auto capture = [&](std::string_view scene) {
			std::cout << "INVENTORY capture=" << scene << " frame=" << engine.frameNumber - 1 << '\n';
		};

		resize(1920, 1080);
		frames(2);
		tap(SDLK_E);
		present();
		SDL_WarpMouseInWindow(engine.window, cell(0).x, cell(0).y);
		present();
		capture("16x9");
		CHECK(engine.inventoryPanelOpen() && !engine.developerUiAvailable() &&
			!engine.mouseCaptured && !engine.simulationSuspended(), "inventory owns native input without pausing solo");

		const auto before = snapshot();
		clickSlot(2);
		present();
		CHECK(held() && snapshot() == before, "whole pickup is a view of the confirmed stack");
		clickSlot(0);
		present();
		CHECK(!held() && snapshot() == before &&
			engine.playerInventoryView().feedbackKind == gameui::style::Status::Error,
			"full matching stack rejects the move, shows a reason and clears the drag");
		clickSlot(2);
		clickSlot(9);
		present();
		CHECK(snapshot()[9].item == Stone && snapshot()[9].count == 12 && snapshot()[2].isEmpty(),
			"whole move transfers exactly once");
		clickSlot(1, SDL_BUTTON_RIGHT);
		clickSlot(10);
		present();
		CHECK(snapshot()[10].item == Dirt && snapshot()[10].count == 3 && snapshot()[1].count == 2,
			"odd half split transfers three and leaves two");
		clickSlot(9);
		clickSlot(10);
		present();
		CHECK(snapshot()[9].item == Dirt && snapshot()[10].item == Stone, "whole drop onto another item swaps");
		clickSlot(9);
		clickSlot(1);
		present();
		CHECK(snapshot()[1].count == 5 && snapshot()[9].isEmpty(), "matching stacks merge without loss");

		clickSlot(10);
		const auto target = cell(11);
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, target.x, target.y);
		button(SDL_EVENT_MOUSE_BUTTON_UP, target.x + 100.f, target.y);
		present();
		CHECK(held() && snapshot()[11].isEmpty(), "release on another cell does not activate the pressed destination");
		clickAt({4.f, 4.f});
		present();
		CHECK(!held() && snapshot()[10].count == 12, "click outside cancels the drag without a world edit");
		clickSlot(10);
		tap(SDLK_E);
		present();
		CHECK(!engine.inventoryPanelOpen() && !held() && engine.mouseCaptured,
			"closing mid-drag restores mouse capture");
		tap(SDLK_E);
		present();
		clickSlot(10);
		present();
		windowEvent(SDL_EVENT_WINDOW_FOCUS_LOST);
		present();
		CHECK(!held() && snapshot()[10].count == 12, "focus loss discards an armed inventory gesture");
		windowEvent(SDL_EVENT_WINDOW_FOCUS_GAINED);
		present();
		clickSlot(10, SDL_BUTTON_RIGHT);
		tap(SDLK_RIGHT);
		key(SDLK_RETURN, SDL_EVENT_KEY_DOWN);
		key(SDLK_RETURN, SDL_EVENT_KEY_DOWN, true);
		key(SDLK_RETURN, SDL_EVENT_KEY_UP);
		present();
		CHECK(snapshot()[10].count == 6 && snapshot()[11].count == 6 && !held(),
			"keyboard focus reaches the destination and key repeat cannot submit twice");
		tap(SDLK_2);
		present();
		CHECK(engine.selectedHotbarSlot() == 1, "the inventory preserves number-key selection");
		tap(SDLK_Q);
		present();
		CHECK(snapshot()[1].count == 4, "Q drops one item from the selected stack");
		clickAt(middle(sample.m_inventoryLayout.drop));
		present();
		CHECK(snapshot()[1].isEmpty(), "the drop stack button consumes exactly the remaining selected stack");

		resize(2560, 1080);
		frames(2);
		clickSlot(10, SDL_BUTTON_RIGHT);
		SDL_WarpMouseInWindow(engine.window, cell(11).x, cell(11).y);
		present();
		capture("21x9");
		CHECK(held() && sample.m_inventoryLayout.panel.intersect(sample.m_draws.bounds()) == sample.m_inventoryLayout.panel,
			"the ultrawide screen renders a held stack and keeps the panel in bounds");
		tap(SDLK_D, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
		present();
		CHECK(engine.inventoryPanelOpen() && engine.developerUiAvailable() && !held(),
			"workspace changes cancel held state and keep the native screen open");

		// PR #316 F02 (solo) + F03/F05: the solo proof explicitly covers a
		// PARTIAL merge, and the held-stack presentation follows the active
		// input modality and stays inside the available game view. All
		// addressing goes through slot identities: the layout is a grid whose
		// CELL n maps to slot (n + 9) % 36, so the fixture searches slots.
		// The developer workspace hides so the native screen owns the pointer
		// and the keyboard; the F01 block below shows it again explicitly.
		engine.setDeveloperWorkspaceVisible(false);
		engine.sim->grantItems(Cobblestone, 64); // fills the first empty hotbar slot at the cap
		engine.sim->grantItems(Cobblestone, 40); // overflows into a second pile
		present();
		const auto slotPoint = [&](int slot) { return middle(sample.m_inventoryLayout.slot({slot})); };
		const auto pileOf = [&](uint32_t count) {
			const auto all = snapshot();
			for (int slot = 0; slot < inventoryui::kSlotCount; ++slot)
				if (all[size_t(slot)].item == Cobblestone && all[size_t(slot)].count == count)
					return slot;
			return -1;
		};
		const auto capped = pileOf(64);
		auto over = pileOf(40);
		CHECK(capped >= 0 && over >= 0 && capped != over,
			"the partial-merge fixture holds a clashing 64/40 cobblestone pair");
		const auto scene = [&] { return sample.m_draws.bounds(); };
		const auto heldInset = std::min(gameui::style::baseline().space.xs * sample.m_inventoryLayout.scale,
			sample.m_inventoryLayout.main.cell * 0.5f);
		const auto holdsHeldIcon = [&](gameui::Clip placement) {
			const auto icon = gameui::inset(placement, heldInset);
			return std::ranges::any_of(sample.m_draws.quads(), [&](const gameui::Quad &quad) {
				return quad.layer == static_cast<int32_t>(TextureType::COBBLESTONE) && quad.rect == icon.rect;
			});
		};
		const auto heldRect = [&](glm::vec2 point) {
			return gameui::heldStackPlacement(point, sample.m_inventoryLayout.main.cell,
				10.f * sample.m_inventoryLayout.scale, scene());
		};
		const auto slotRect = [&](inventoryui::SlotId slot) {
			return gameui::heldStackPlacement(middle(sample.m_inventoryLayout.slot(slot)),
				sample.m_inventoryLayout.main.cell, 10.f * sample.m_inventoryLayout.scale, scene());
		};
		// F02 (solo): split the capped pile onto the 40 pile — the destination
		// reaches the cap and the overflow stays at the source.
		clickAt(slotPoint(capped), SDL_BUTTON_RIGHT);
		clickAt(slotPoint(over));
		present();
		CHECK(!held() && snapshot()[size_t(capped)].item == Cobblestone &&
			snapshot()[size_t(capped)].count == 40 && snapshot()[size_t(over)].count == 64,
			"the split fills the destination slot to capacity and keeps the remainder in the source");
		CHECK(engine.playerInventoryView().feedbackKind == gameui::style::Status::Info &&
			engine.playerInventoryView().feedback == "Only part of the stack fit.",
			"a partial merge reports the overflow without an error state");
		// F05: re-seat the 40 remainder pile in the main grid, then a pointer
		// pickup anchors the held stack to the pointer. The pile must sit above
		// the hotbar row: arrow navigation refuses moves out of the bottom row.
		{
			const auto mainSeat = [&] {
				for (int slot = items::kHotbarSize; slot < inventoryui::kSlotCount; ++slot)
					if (snapshot()[size_t(slot)].isEmpty())
						return slot;
				throw std::runtime_error("proof found no empty main slot");
			}();
			clickAt(slotPoint(over));
			clickAt(slotPoint(mainSeat));
			present();
			over = mainSeat;
		}
		clickAt(slotPoint(over), SDL_BUTTON_RIGHT);
		present();
		CHECK(held() && holdsHeldIcon(heldRect(slotPoint(over))),
			"pointer pickup anchors the held stack next to the pointer");
		// F05: keyboard navigation moves the anchor to the focused slot without
		// touching the armed gesture, and passive motion cannot pull it back.
		const auto beforeKeys = snapshot();
		const int focusStart = int(sample.screens().focus());
		tap(SDLK_DOWN);
		present();
		inventoryui::SlotId destination = inventoryui::slotAtCell(focusStart - 1 + inventoryui::kColumns);
		CHECK(held() && snapshot() == beforeKeys &&
			int(sample.screens().focus()) == focusStart + inventoryui::kColumns,
			"an arrow key moves the focus without submitting or cancelling the held stack");
		CHECK(holdsHeldIcon(slotRect(destination)),
			"the held stack follows the focused slot under keyboard control");
		tap(SDLK_RIGHT);
		present();
		const int focusNext = int(sample.screens().focus());
		destination = inventoryui::slotAtCell(focusNext - 1);
		const glm::vec4 keyboardRect = slotRect(destination).rect;
		CHECK(focusNext == focusStart + inventoryui::kColumns + 1 && held() &&
			holdsHeldIcon(slotRect(destination)),
			"arrow navigation keeps moving the keyboard-anchored held stack");
		motion(middle(sample.m_inventoryLayout.slot(inventoryui::slotAtCell(4))).x,
			middle(sample.m_inventoryLayout.slot(inventoryui::slotAtCell(4))).y);
		present();
		CHECK(held() && holdsHeldIcon({keyboardRect}) && int(sample.screens().focus()) == focusNext,
			"passive pointer motion cannot jump the keyboard-anchored held stack");
		// F11 (#316): inert keyboard events never move the preview. A
		// modifier-only key is delivered but consumes nothing: focus pos,
		// keyboard anchoring and the held stack stay exactly where they are.
		tap(SDLK_LSHIFT);
		present();
		CHECK(held() && holdsHeldIcon({keyboardRect}) && int(sample.screens().focus()) == focusNext &&
			sample.m_inventoryModality == gameui::InventoryModality::Keyboard,
			"a modifier-only key leaves the keyboard anchoring untouched");
		// F11: an unsupported pointer button never re-anchors the preview.
		clickAt(middle(sample.m_inventoryLayout.slot(inventoryui::slotAtCell(4))), SDL_BUTTON_X1);
		present();
		CHECK(held() && holdsHeldIcon({keyboardRect}) &&
			sample.m_inventoryModality == gameui::InventoryModality::Keyboard,
			"an unsupported mouse button leaves the keyboard anchoring untouched");
		// F05: a completed click outside the grid is an invalid drop target: the
		// armed gesture cancels without a command and the presentation returns
		// to pointer anchoring.
		const auto deadZone = glm::vec2{middle(sample.m_inventoryLayout.slot(inventoryui::slotAtCell(4))).x,
			sample.m_inventoryLayout.main.bounds.rect.w + 8.f};
		clickAt(deadZone);
		present();
		CHECK(!held() && snapshot() == beforeKeys &&
			sample.m_inventoryModality == gameui::InventoryModality::Pointer,
			"a click outside the grid cancels the armed gesture without a command");
		// F03: re-arm by pointer, then carry the stack toward the bottom-right
		// game-view edge — the offset flips / clamps so it stays fully visible.
		clickAt(slotPoint(over), SDL_BUTTON_RIGHT);
		const glm::vec2 corner{scene().rect.z - 10.f, scene().rect.w - 10.f};
		SDL_WarpMouseInWindow(engine.window, corner.x, corner.y);
		motion(corner.x, corner.y);
		present();
		const auto edgeRect = heldRect(corner);
		const auto unflipped = gameui::rectangle(corner + glm::vec2{10.f * sample.m_inventoryLayout.scale},
			{sample.m_inventoryLayout.main.cell, sample.m_inventoryLayout.main.cell});
		const bool inside = edgeRect.rect.x >= scene().rect.x && edgeRect.rect.y >= scene().rect.y &&
			edgeRect.rect.z <= scene().rect.z + 0.5f && edgeRect.rect.w <= scene().rect.w + 0.5f;
		CHECK(held() && holdsHeldIcon(edgeRect) && inside && !(edgeRect.rect == unflipped.rect),
			"the held stack flips near the view edge and stays fully inside the game view");

		// PR #316 F01: Developer UI takes the keyboard from the open inventory;
		// its keys never become inventory commands until a slot click hands the
		// keyboard back.
		engine.setDeveloperWorkspaceVisible(true);
		char editorValue[32]{};
		glm::vec2 editorPoint{};
		bool popupWanted = false, popupVisible = false, popupCloseRequested = false;
		const auto ownershipEditor = [&] {
			ImGui::SetNextWindowPos({45.f, 70.f}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({220.f, 84.f}, ImGuiCond_Always);
			if (ImGui::Begin("Inventory keyboard ownership", nullptr,
				ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing))
			{
				ImGui::InputText("##owned", editorValue, sizeof(editorValue));
				const auto item = ImGui::GetItemRectMin();
				const auto origin = ImGui::GetMainViewport()->Pos;
				editorPoint = {item.x - origin.x + 15.f, item.y - origin.y + 8.f};
				popupVisible = false;
				if (popupWanted)
					ImGui::OpenPopup("owned-popup");
				if (ImGui::BeginPopup("owned-popup"))
				{
					popupVisible = true;
					ImGui::Text("Keyboard owned by Developer UI");
					if (popupCloseRequested)
						ImGui::CloseCurrentPopup();
					ImGui::EndPopup();
				}
			}
			ImGui::End();
		};
		popupWanted = true;
		present(ownershipEditor);
		present(ownershipEditor); // OpenPopup takes effect on the next frame
		CHECK(popupVisible, "the developer window opens a non-modal popup over the inventory");
		present(ownershipEditor);
		CHECK(!held() && engine.inventoryPanelOpen() && popupVisible &&
			sample.m_inventoryModality == gameui::InventoryModality::Pointer,
			"opening a Developer UI popup cancels the held stack and resets the modality");
		popupWanted = false;
		popupCloseRequested = true;
		present(ownershipEditor);
		popupCloseRequested = false;
		present(ownershipEditor);
		CHECK(!popupVisible, "the proof popup closes again");
		const auto beforeEditor = snapshot();
		const auto focusEditor = int(sample.screens().focus());
		clickAt(editorPoint);
		present(ownershipEditor);
		CHECK(engine.imgui->editingText(),
			"the developer editor takes keyboard ownership from the open inventory");
		tap(SDLK_END);
		commitText("dev");
		tap(SDLK_RETURN); // InputText applies its working buffer when the edit ends
		tap(SDLK_SPACE);
		tap(SDLK_LEFT);
		tap(SDLK_TAB);
		engine.handleEvents();
		for (int frame = 0; frame < 4; ++frame) // ImGui consumes its event queue one frame late
			present(ownershipEditor);
		CHECK(std::string_view(editorValue) == "dev", "committed text reaches only the developer editor");
		CHECK(snapshot() == beforeEditor && !held() && int(sample.screens().focus()) == focusEditor,
			"Enter, Space, arrows and Tab reach only the developer editor, not the inventory");
		clickSlot(2);
		present(ownershipEditor);
		CHECK(!engine.imgui->pendingKeyboard() && !engine.imgui->editingText() && !held(),
			"clicking an inventory slot returns keyboard ownership to the native screen");
		const int focusResume = int(sample.screens().focus());
		tap(SDLK_TAB);
		present(ownershipEditor);
		CHECK(int(sample.screens().focus()) == focusResume + 1,
			"native keyboard navigation resumes after the developer handoff");
		capture("ownership");

		// PR #316 round 3 F01: keyboard ownership with a NON-TEXTUAL widget.
		// A focused button stays the owner after its ActiveId releases; Tab,
		// arrows, Enter and Space reach only the Developer UI; a right-click
		// on a native slot hands the keyboard back and arms a half-stack
		// gesture that the deferred handoff must not re-latch away.
		int buttonActivations = 0;
		glm::vec2 buttonPoint{};
		const auto navButton = [&] {
			// Below the round-2 editor rect: that window stops being submitted
			// here, but ImGui can keep it as HoveredWindow for a while, and a
			// overlapping rect would steal the hover from this one.
			ImGui::SetNextWindowPos({45.f, 200.f}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({220.f, 84.f}, ImGuiCond_Always);
			if (ImGui::Begin("Inventory keyboard button", nullptr,
				ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing))
			{
				if (ImGui::Button("Dev button"))
					++buttonActivations;
				const auto item = ImGui::GetItemRectMin();
				const auto origin = ImGui::GetMainViewport()->Pos;
				buttonPoint = {item.x - origin.x + 15.f, item.y - origin.y + 8.f};
			}
			ImGui::End();
		};
		present(navButton); // paints the window and locates the button
		// Focus and release in separate builds: the press arms the widget
		// (ActiveId), the next build releases it and activates the button —
		// the exact press-then-release an interactive focus carries. The
		// pointer position is fed to ImGui directly: a real interaction has
		// the OS cursor at the press point, and the desktop can move the
		// physical cursor between the frames of an automated run. Events
		// queued behind a keyboard-yield boundary drain over a few frames,
		// so the activation is polled within a bounded deadline.
		SDL_WarpMouseInWindow(engine.window, buttonPoint.x, buttonPoint.y);
		engine.imgui->observePointer(buttonPoint.x, buttonPoint.y);
		motion(buttonPoint.x, buttonPoint.y);
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, buttonPoint.x, buttonPoint.y);
		present(navButton);
		engine.imgui->observePointer(buttonPoint.x, buttonPoint.y);
		button(SDL_EVENT_MOUSE_BUTTON_UP, buttonPoint.x, buttonPoint.y);
		for (int frame = 0; frame < 8 && buttonActivations == 0; ++frame)
			present(navButton);
		present(navButton); // the latch observes the released ActiveId here
		CHECK(buttonActivations == 1,
			"the click reaches and activates the non-textual developer button exactly once");
		CHECK(engine.imgui->ownsKeyboard() && !engine.imgui->editingText(),
			"a released non-textual developer button keeps the keyboard (no editing field involved)");
		CHECK(!engine.imgui->ownsActiveInput(),
			"the released button holds no ActiveId while the keyboard ownership persists");
		const auto beforeDevNav = snapshot();
		const int focusBeforeDevNav = int(sample.screens().focus());
		tap(SDLK_TAB);
		tap(SDLK_LEFT);
		tap(SDLK_RIGHT);
		tap(SDLK_RETURN);
		tap(SDLK_SPACE);
		engine.handleEvents();
		for (int frame = 0; frame < 4; ++frame) // ImGui consumes its event queue one frame late
			present(navButton);
		CHECK(engine.imgui->ownsKeyboard() && !held() && snapshot() == beforeDevNav &&
			int(sample.screens().focus()) == focusBeforeDevNav,
			"Tab, arrows, Enter and Space reach only the developer button, not the inventory");
		engine.sim->grantItems(Sand, 20);
		present(navButton);
		const auto sandSlot = [&] {
			const auto all = snapshot();
			for (int slot = 0; slot < inventoryui::kSlotCount; ++slot)
				if (all[size_t(slot)].item == Sand && all[size_t(slot)].count == 20)
					return slot;
			throw std::runtime_error("proof found no sand stack for the round-3 ownership handoff");
		}();
		const auto emptyMain = [&] {
			for (int slot = items::kHotbarSize; slot < inventoryui::kSlotCount; ++slot)
				if (snapshot()[size_t(slot)].isEmpty())
					return slot;
			throw std::runtime_error("proof found no empty main slot for the round-3 split");
		}();
		clickAt(slotPoint(sandSlot), SDL_BUTTON_RIGHT);
		present(navButton);
		CHECK(held() && !engine.imgui->ownsKeyboard() && !engine.imgui->editingText(),
			"a right-click on a native slot takes the keyboard back and arms the half-stack gesture");
		for (int frame = 0; frame < 6; ++frame)
			present(navButton);
		CHECK(held() && !engine.imgui->ownsKeyboard() && !engine.imgui->editingText(),
			"deferred frames do not restore the developer keyboard ownership");
		clickAt(slotPoint(emptyMain));
		present(navButton);
		CHECK(!held() && snapshot()[size_t(sandSlot)].count == 10 &&
			snapshot()[size_t(emptyMain)].item == Sand && snapshot()[size_t(emptyMain)].count == 10,
			"the half-stack split completes through native input after the developer handoff");
		capture("ownership-button");

		engine.setDeveloperWorkspaceVisible(false);
		resize(960, 540);
		sample.setScalePolicy({gameui::ScaleMode::Fixed, 2.f});
		sample.setFaceDensityOverride(2.f);
		const auto generation = engine.m_atlasVersion;
		CHECK(engine.applyResourcePack("").atlasOk && engine.m_atlasVersion != generation, "atlas generation changes while open");
		frames(3);
		capture("compact-2x");
		CHECK(sample.m_draws.atlasGeneration() == engine.m_atlasVersion &&
			sample.lastRecordedStats()->metrics.dropped == 0 &&
			std::ranges::any_of(sample.m_draws.quads(), [](const gameui::Quad &q) { return q.layer == STONE; }),
			"compact high-density layout renders fresh item icons after reload");
		clickSlot(10);
		present();
		CHECK(held() && engine.reloadWorld(43), "epoch test begins while holding a stack");
		present();
		CHECK(!engine.inventoryPanelOpen() && !held() && engine.playerHudView().hotbar.slots[0].empty(),
			"world reload drops the previous inventory and cursor presentation");
		context.waitIdle();
		const auto errors = context.validationErrorCount() - errorsBefore;
		const auto rtss = context.rtssValidationErrorCount() - rtssBefore;
		std::cout << "Inventory validation errors=" << errors << " known_rtss_errors=" << rtss << '\n';
		CHECK(errors == rtss, "inventory adds no validation errors outside known RTSS messages");
	}

	void playerHudProof()
	{
		init();
		auto &sample = *engine.m_playerUi;
		const auto &context = *engine.vkContext;
		const auto errorsBefore = context.validationErrorCount();
		const auto rtssBefore = context.rtssValidationErrorCount();
		sample.setPreviewStripVisible(false);
		// F01/F04: the launch state is a hidden workspace (--no-dev-ui), and the
		// whole workspace can be shown, hidden and restored at runtime. The
		// dockspace identity must survive every transition, so the central game
		// view keeps its exact rectangle (that rectangle IS the persisted split
		// tree: side/bottom docks included).
		engine.setDeveloperWorkspaceVisible(false);
		frames(2);
		CHECK(!engine.developerUiAvailable() && !engine.developerWorkspaceVisible(),
			"the proof starts without the developer workspace");
		engine.setDeveloperWorkspaceVisible(true);
		frames(3);
		const auto dockedView = engine.gameUi->shell().gameViewRect();
		CHECK(engine.developerUiAvailable() && dockedView.size.x > 0.f && dockedView.size.y > 0.f,
			"showing the workspace restores the developer chrome and its input ownership");
		for (int round = 0; round < 3; ++round)
		{
			engine.setDeveloperWorkspaceVisible(false);
			present();
			CHECK(!engine.developerUiAvailable(),
				"hiding the workspace removes the chrome and its input ownership");
			engine.setDeveloperWorkspaceVisible(true);
			frames(2);
			const auto restored = engine.gameUi->shell().gameViewRect();
			CHECK(engine.developerUiAvailable() &&
					std::abs(restored.pos.x - dockedView.pos.x) < 0.5f &&
					std::abs(restored.pos.y - dockedView.pos.y) < 0.5f &&
					std::abs(restored.size.x - dockedView.size.x) < 0.5f &&
					std::abs(restored.size.y - dockedView.size.y) < 0.5f,
				"the persisted docking layout is restored after every toggle");
		}
		tap(SDLK_E);
		present();
		CHECK(engine.inventoryPanelOpen() && engine.developerUiAvailable(),
			"the inventory opens inside the developer workspace");
		tap(SDLK_E);
		present();
		CHECK(!engine.inventoryPanelOpen(),
			"the inventory closes again without touching the workspace layout");
		engine.setDeveloperWorkspaceVisible(false);
		present();
		tap(SDLK_E);
		present();
		CHECK(engine.inventoryPanelOpen() && !engine.developerUiAvailable() &&
				!engine.developerWorkspaceVisible(),
			"the native inventory opens with the workspace hidden");
		tap(SDLK_ESCAPE);
		present();
		CHECK(!engine.inventoryPanelOpen() && engine.mouseCaptured,
			"Escape closes the inventory and returns to the HUD");
		engine.setShowDeveloperUi(false);
		engine.sim->grantItems(items::ItemId::Stone, 12);
		engine.sim->grantItems(items::ItemId::Dirt, 4);
		resize(960, 540);
		sample.setScalePolicy({gameui::ScaleMode::Fixed, 2.f});
		frames(3);
		const auto hasStoneIcon = [&] {
			return std::ranges::any_of(sample.m_draws.quads(), [](const gameui::Quad &q) { return q.layer == STONE; });
		};
		CHECK(hasStoneIcon() && sample.renderer().stats(0).dropped == 0 && !engine.developerUiAvailable(),
			"live hotbar draws in a small window at 200% without the developer workspace");
		CHECK(engine.playerHudView().hotbar.slots[0].count == 12,
			"live HUD reads the solo snapshot");

		tap(SDLK_2);
		present();
		CHECK(engine.playerHudView().hotbar.selectedName == "Dirt", "number key selects via the existing adapter");
		SDL_Event wheel{};
		wheel.type = SDL_EVENT_MOUSE_WHEEL;
		wheel.wheel.windowID = SDL_GetWindowID(engine.window);
		wheel.wheel.y = 1.f;
		checked(SDL_PushEvent(&wheel), "queue gameplay scroll");
		key(SDLK_Q, SDL_EVENT_KEY_DOWN);
		key(SDLK_Q, SDL_EVENT_KEY_DOWN, true);
		key(SDLK_Q, SDL_EVENT_KEY_UP);
		present();
		CHECK(engine.playerHudView().hotbar.selectedName == "Stone" &&
			engine.playerHudView().hotbar.slots[0].count == 11,
			"one wheel event and one non-repeat drop produce one selection and one count decrement");

		tap(SDLK_E);
		present();
		CHECK(engine.inventoryPanelOpen() && !engine.developerUiAvailable() && hasStoneIcon() &&
			sample.m_inventoryView.snapshot->slots[0].count == 11,
			"the native inventory uses the same authoritative stacks as the HUD");
		tap(SDLK_ESCAPE);
		present();
		CHECK(!engine.inventoryPanelOpen() && hasStoneIcon() && engine.mouseCaptured,
			"closing inventory restores the HUD and capture");

		// F06: the creative block selection is explicit and stays on screen when
		// the optional hints (F6) are off — same band, more copy.
		const auto hudSelectionGlyphs = [&] {
			const auto scene = sample.m_draws.bounds();
			const auto area = engine.gameUi->shell().gameViewRect();
			const ImVec2 origin = ImGui::GetMainViewport()->Pos;
			const gameui::Clip safe{{area.pos.x - origin.x, area.pos.y - origin.y,
				area.pos.x - origin.x + area.size.x, area.pos.y - origin.y + area.size.y}};
			const auto layout = gameui::hudLayout(scene, safe, gameui::style::baseline(),
				sample.playerUiScale());
			size_t glyphs = 0;
			for (const auto &quad : sample.m_draws.quads())
			{
				const glm::vec2 centre{(quad.rect.x + quad.rect.z) * 0.5f,
					(quad.rect.y + quad.rect.w) * 0.5f};
				glyphs += quad.layer == -2 && gameui::hit(layout.selection, centre);
			}
			return glyphs;
		};
		tap(SDLK_V);
		present();
		CHECK(engine.playerHudView().creative && !engine.playerHudView().creativeBlock.empty(),
			"debug flight exposes the creative block selection");
		if (engine.gameUi->showOverlayHints())
		{
			tap(SDLK_F6);
			present();
		}
		const auto creativeView = engine.playerHudView();
		CHECK(!creativeView.hints && creativeView.creative,
			"the creative label does not depend on the optional F6 hints");
		const auto creativeGlyphs = hudSelectionGlyphs();
		tap(SDLK_V);
		present();
		CHECK(!engine.playerHudView().creative, "leaving creative restores the normal selection");
		const auto plainGlyphs = hudSelectionGlyphs();
		CHECK(plainGlyphs > 0 && creativeGlyphs > plainGlyphs,
			"the creative prefix adds copy to the selection band (creative=" << creativeGlyphs
				<< ", inventory=" << plainGlyphs << ')');
		if (!engine.gameUi->showOverlayHints())
		{
			tap(SDLK_F6);
			present();
		}
		tap(SDLK_ESCAPE);
		present();
		CHECK(engine.nativePause() && !hasStoneIcon(), "pause supersedes the gameplay HUD");
		const auto toggle = std::ranges::find_if(sample.m_widgets.widgets(),
			[](const gameui::Widget &widget) { return widget.label == "Hide HUD"; });
		checked(toggle != sample.m_widgets.widgets().end(), "pause exposes the HUD toggle");
		const auto bounds = sample.m_widgets.bounds(*toggle);
		clickAt(glm::vec2(bounds.rect) + gameui::size(bounds) * 0.5f);
		present();
		tap(SDLK_ESCAPE);
		present();
		CHECK(!sample.hudVisible() && sample.m_draws.quads().empty() &&
			engine.playerHudView().hotbar.slots[0].count == 11,
			"the visibility toggle changes presentation without touching inventory");
		sample.setHudVisible(true);
		const auto generation = engine.m_atlasVersion;
		CHECK(engine.applyResourcePack("").atlasOk && engine.m_atlasVersion != generation,
			"resource-pack reload publishes a new atlas");
		frames(3);
		CHECK(hasStoneIcon() && sample.m_draws.atlasGeneration() == engine.m_atlasVersion,
			"live icon is repainted using the new atlas after both slots cycle");
		CHECK(engine.reloadWorld(43), "new solo epoch starts");
		present();
		CHECK(!hasStoneIcon() && engine.playerHudView().hotbar.selectedName == "Empty slot",
			"new epoch paints its own inventory with no stale item icon");
		const auto hudFrame = sample.lastRecordedStats()->frameNumber;
		engine.m_inspectionView = true;
		present();
		CHECK(sample.lastRecordedStats()->frameNumber == hudFrame, "inspection skips player UI preparation");
		engine.m_inspectionView = false;
		engine.m_benchmark.requestStart();
		present();
		CHECK(sample.lastRecordedStats()->frameNumber == hudFrame, "active benchmarks skip player UI preparation");
		engine.m_benchmark.cancel();
		workspaceProof();
		context.waitIdle();
		const auto errors = context.validationErrorCount() - errorsBefore;
		const auto rtss = context.rtssValidationErrorCount() - rtssBefore;
		std::cout << "HUD validation errors=" << errors << " known_rtss_errors=" << rtss << '\n';
		CHECK(errors == rtss, "HUD adds no validation errors outside known RTSS messages");
	}

	/// Exercise the workspace shortcut, dock tree and input cancellation with SDL events.
	void workspaceProof()
	{
		auto &shell = engine.gameUi->shell();
		const SDL_Keymod chord = SDL_KMOD_CTRL | SDL_KMOD_SHIFT;
		engine.setDeveloperWorkspaceVisible(true);
		engine.setInventoryOpen(false);
		frames(3);
		CHECK(engine.developerWorkspaceVisible() && shell.dockspaceId() != 0,
			"the workspace proof starts with a visible workspace and a live dockspace");

		// (1) One press, one transition, both directions.
		tap(SDLK_D, chord);
		present();
		CHECK(!engine.developerWorkspaceVisible(), "Ctrl+Shift+D hides the whole workspace");
		tap(SDLK_D, chord);
		present();
		CHECK(engine.developerWorkspaceVisible(), "Ctrl+Shift+D shows it again");

		// (2) Repeats and incomplete chords are not the shortcut.
		key(SDLK_D, SDL_EVENT_KEY_DOWN, true, chord);
		present();
		key(SDLK_D, SDL_EVENT_KEY_UP, false, chord);
		present();
		CHECK(engine.developerWorkspaceVisible(), "a key repeat does not toggle the workspace again");
		for (const auto partial : std::array<SDL_Keymod, 4>{SDL_KMOD_NONE, SDL_KMOD_CTRL, SDL_KMOD_SHIFT,
				 SDL_KMOD_ALT})
		{
			tap(SDLK_D, partial);
			present();
			CHECK(engine.developerWorkspaceVisible(),
				"plain D and incomplete chords stay plain keys (mods=" << int(partial) << ')');
		}

		// (3) Hiding cancels developer-owned input instead of leaking it.
		constexpr size_t kDevButton = 1;
		engine.m_inputRouter.button(kDevButton, true, gameui::InputOwner::DeveloperUi);
		checked(engine.m_inputRouter.pointerOwner() == gameui::InputOwner::DeveloperUi &&
			engine.m_inputRouter.activeButton(kDevButton), "a developer pointer drag owns the surface");
		tap(SDLK_D, chord);
		present();
		CHECK(!engine.developerWorkspaceVisible() && !engine.m_inputRouter.activeButton(kDevButton) &&
			engine.m_inputRouter.pointerOwner() != gameui::InputOwner::DeveloperUi,
			"hiding the workspace cancels the in-flight developer drag");
		tap(SDLK_D, chord);
		frames(2);
		checked(engine.developerWorkspaceVisible(), "the workspace is back for the docking proof");

		// (4) The benchmark guard covers both the chord and the menu action.
		engine.m_benchmark.requestStart();
		const bool benchmarkActive = engine.m_benchmark.isActive();
		tap(SDLK_D, chord);
		present();
		CHECK(benchmarkActive && engine.developerWorkspaceVisible(),
			"the chord is consumed without changing visibility during a benchmark");
		engine.setDeveloperWorkspaceVisible(false);
		CHECK(!engine.m_benchmark.isActive() || engine.developerWorkspaceVisible(),
			"the menu action obeys the same benchmark guard");
		engine.m_benchmark.cancel();
		tap(SDLK_D, chord);
		present();
		CHECK(!engine.developerWorkspaceVisible(),
			"the chord works again once the benchmark ended");

		// (5) F09: with the workspace hidden and the inventory closed only the
		// dockspace is maintained, so the developer frame is not built at all.
		// The main menu bar belongs to that frame, so the last frame it was
		// submitted in makes the difference observable.
		const auto menuBarFrame = [] {
			const ImGuiWindow *bar = ImGui::FindWindowByName("##MainMenuBar");
			return bar ? bar->LastFrameActive : 0;
		};
		engine.setInventoryOpen(false);
		engine.setDeveloperWorkspaceVisible(false);
		frames(4);
		const int hiddenBarFrame = menuBarFrame();
		const auto hiddenDockspace = shell.dockspaceId();
		const auto medianDevBuild = [&](int samples) {
			std::vector<float> values;
			for (int i = 0; i < samples; ++i)
			{
				present();
				values.push_back(GetProfiler().lastScopeMs("DevUIBuild"));
			}
			return percentile(values, 0.5f);
		};
		const auto hiddenMs = medianDevBuild(40);
		CHECK(hiddenDockspace != 0 && hiddenBarFrame > 0 && menuBarFrame() == hiddenBarFrame,
			"the full-window path maintains the dockspace without building the developer frame");
		// The developer keys still own their overlay toggles on that path: B has to
		// reach the overlay layer even though no developer panel is drawn.
		const bool bordersBefore = engine.worldRenderer->overlays().showChunkBorders();
		tap(SDLK_B);
		present();
		CHECK(engine.worldRenderer->overlays().showChunkBorders() != bordersBefore,
			"chunk borders still toggle with the workspace hidden and the inventory closed");
		tap(SDLK_B);
		present();
		engine.setDeveloperWorkspaceVisible(true);
		frames(2);
		const auto visibleMs = medianDevBuild(40);
		std::cout << "DevUIBuild median: hidden=" << hiddenMs << "ms visible=" << visibleMs << "ms\n";
		CHECK(menuBarFrame() >= ImGui::GetFrameCount() - 1,
			"the developer frame is built again as soon as the workspace returns");

		// A player screen can remain open as the workspace changes around it.
		auto &sample = *engine.m_playerUi;
		tap(SDLK_E);
		frames(2);
		const auto dockedPanel = sample.m_inventoryLayout.panel;
		const auto dockId = shell.dockspaceId();
		tap(SDLK_D, chord);
		frames(2);
		CHECK(engine.inventoryPanelOpen() && !engine.developerUiAvailable() &&
			sample.m_inventoryLayout.panel != dockedPanel && shell.dockspaceId() == dockId,
			"native inventory uses the full view when the workspace hides");
		tap(SDLK_D, chord);
		frames(2);
		CHECK(engine.inventoryPanelOpen() && sample.m_inventoryLayout.panel == dockedPanel &&
			shell.dockspaceId() == dockId, "restoring the workspace restores the inventory safe area");
		tap(SDLK_E);
		present();
	}

	void performance()
	{
		auto &gpuProfiler = engine.frameCtx->gpuProfiler();
		if (!gpuProfiler.supported() || !gpuProfiler.enabled())
		{
			CHECK(false, gpuProfiler.status());
			return;
		}
		init();
		engine.setShowDeveloperUi(false);
		engine.m_playerUi->setPreviewStripVisible(false);
		for (auto item : {items::ItemId::Stone, items::ItemId::Dirt, items::ItemId::Sand,
				items::ItemId::Cobblestone, items::ItemId::GrassBlock})
			engine.sim->grantItems(item, 32);
		engine.setVSync(false);
		resize(1920, 1080);
		auto sample = std::move(engine.m_playerUi);
		const auto measure = [&](bool uiEnabled, uint64_t tag) {
			constexpr unsigned measuredFrames = 600;
			constexpr unsigned minGpuSamples = measuredFrames * 95 / 100;
			frames(120);
			std::vector<float> wall, gpuFrame, uiCpu, uiGpu;
			uint64_t serial = gpuProfiler.latest().serial;
			const bool expectEncode = uiEnabled && colorspace::outputTransferRequiresShaderEncode(
				colorspace::classifyOutputTransfer(engine.swapchain->getImageFormat(), engine.swapchain->getColorSpace()));
			const auto collect = [&] {
				const auto &gpu = gpuProfiler.latest();
				if (gpu.serial <= serial)
					return;
				serial = gpu.serial;
				const auto draw = size_t(GpuPass::GameUi);
				const auto encode = size_t(GpuPass::GameUiEncode);
				if (gpu.benchmarkTag != tag || !gpu.present[size_t(GpuPass::Frame)] ||
					gpu.present[draw] != uiEnabled || gpu.present[encode] != expectEncode)
					return;
				gpuFrame.push_back(gpu.ms[size_t(GpuPass::Frame)]);
				if (uiEnabled)
					uiGpu.push_back(gpu.ms[draw] + (expectEncode ? gpu.ms[encode] : 0.f));
			};
			for (unsigned i = 0; i < measuredFrames; ++i)
			{
				present({}, tag);
				const auto &prof = GetProfiler();
				wall.push_back(prof.lastFrameMs());
				if (uiEnabled)
					uiCpu.push_back(prof.lastScopeMs("GameUIBuild") + prof.lastScopeMs("GameUIDraw"));
				collect();
			}
			// Slot reads happen before recording. Drain the final measured
			// slots with untagged frames and the normal fence path.
			for (uint32_t i = 0; i < VkFrameContext::kMaxFramesInFlight; ++i)
			{
				present();
				collect();
			}
			const auto report = [](const char *name, const std::vector<float> &samples) {
				if (samples.empty())
					std::cout << ' ' << name << "=n/a";
				else
					std::cout << ' ' << name << "_median=" << percentile(samples, 0.5f)
						<< ' ' << name << "_p95=" << percentile(samples, 0.95f);
			};
			std::cout << (uiEnabled ? "game_ui" : "baseline") << " frames=" << wall.size()
				<< " gpu_samples=" << gpuFrame.size() << " ui_gpu_samples=" << uiGpu.size();
			report("frame", wall);
			report("gpu", gpuFrame);
			report("ui_cpu", uiCpu);
			report("ui_gpu", uiGpu);
			std::cout << '\n';
			CHECK(gpuFrame.size() >= minGpuSamples, "GPU sample coverage below " << minGpuSamples <<
				"/" << measuredFrames << " with the expected passes, mode=" << (uiEnabled ? "game_ui" : "baseline"));
		};
		measure(false, 1);
		engine.m_playerUi = std::move(sample);
		measure(true, 2);
		measure(true, 3);
		sample = std::move(engine.m_playerUi);
		measure(false, 4);
		engine.m_playerUi = std::move(sample);
		const auto stats = engine.m_playerUi->renderer().stats(0);
		std::cout << "ui_resources draws=" << stats.draws << " vertices=" << stats.vertices
			<< " sets=" << stats.descriptorSets << " instance_bytes=" << stats.instanceBytes
			<< " font_bytes=" << stats.fontBytes << " composition_bytes=" << stats.compositionBytes << '\n';
	}
};

int runMultiplayerMenuProof(Engine &engine, const std::filesystem::path &out, const std::filesystem::path &server)
{
	GameUiBackendProbe(engine).multiplayerMenuProof(out, server);
	if (testmain::fails == 0)
		std::cout << "Native multiplayer menu: " << testmain::checks << " checks\n";
	return testmain::exitCode();
}

int runSessionMenuProof(Engine &engine, const std::filesystem::path &output)
{
	std::filesystem::create_directories(output);
	GameUiBackendProbe(engine).sessionMenuProof(output);
	if (testmain::fails == 0)
		std::cout << "PASS: Session menu Vulkan proof (" << testmain::checks << " checks)\n";
	return testmain::exitCode();
}

int runGameUiScreenshot(Engine &engine, const std::filesystem::path &output)
{
	GameUiBackendProbe(engine).screenshot(output);
	if (testmain::fails == 0)
		std::cout << "PASS: Game UI offscreen proof (" << testmain::checks << " checks)\n";
	return testmain::exitCode();
}

int runGameUiPresentProof(Engine &engine, const std::filesystem::path &output)
{
	GameUiBackendProbe(engine).presentProof(output);
	if (testmain::fails == 0)
		std::cout << "PASS: Game UI present proof (" << testmain::checks << " checks)\n";
	return testmain::exitCode();
}

int runGameUiShowcaseProof(Engine &engine)
{
	GameUiBackendProbe(engine).showcaseProof();
	if (testmain::fails == 0)
		std::cout << "PASS: Game UI showcase proof (" << testmain::checks << " checks)\n";
	return testmain::exitCode();
}

int runGameUiDesignSystem(Engine &engine, const std::filesystem::path &output)
{
	GameUiBackendProbe(engine).designSystem(output);
	if (testmain::fails == 0)
		std::cout << "PASS: Game UI design-system proof (" << testmain::checks << " checks)\n";
	return testmain::exitCode();
}

int runGameUiPerformance(Engine &engine)
{
	GameUiBackendProbe(engine).performance();
	return testmain::exitCode();
}

int runPlayerHudProof(Engine &engine)
{
	GameUiBackendProbe(engine).playerHudProof();
	if (testmain::fails == 0)
		std::cout << "PASS: live player HUD proof (" << testmain::checks << " checks)\n";
	return testmain::exitCode();
}

int runPlayerInventoryProof(Engine &engine)
{
	GameUiBackendProbe(engine).playerInventoryProof();
	if (testmain::fails == 0)
		std::cout << "PASS: native player inventory (" << testmain::checks << " checks)\n";
	return testmain::exitCode();
}

int runPlayerSettingsProof(Engine &engine, const std::filesystem::path &output)
{
	GameUiBackendProbe(engine).playerSettingsProof(output);
	if (testmain::fails == 0)
		std::cout << "PASS: native player Settings (" << testmain::checks << " checks)\n";
	return testmain::exitCode();
}

int runPlayerSettingsRestartProof(Engine &engine, const std::filesystem::path &output)
{
	GameUiBackendProbe(engine).playerSettingsRestartProof(output);
	if (testmain::fails == 0)
		std::cout << "PASS: native player Settings restart (" << testmain::checks << " checks)\n";
	return testmain::exitCode();
}
