#include <Engine/Engine.hpp>
#include <Engine/GameUiSample.hpp>
#include <Engine/Profiler.hpp>
#include <Renderer/ColorSpace.hpp>
#include <Vulkan/ImageBarrier.hpp>
#include <imgui/imgui_internal.h>

#include "TestMain.hpp"
#include "VisualImage.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
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
		const auto size = extent();
		engine.recreateSwapchainIfNeeded(size.width, size.height);
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
		engine.updateFrameUniforms(slot, int(size.width), int(size.height), 5.f);
		engine.recordFrame(slot, image, 0, gpuTag);
		checked(engine.frameCtx->submitAndPresent(*engine.swapchain, image), "present Game UI proof frame");
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

	void button(Uint32 type, float x, float y)
	{
		SDL_Event event{};
		event.type = type;
		event.button.windowID = SDL_GetWindowID(engine.window);
		event.button.button = SDL_BUTTON_LEFT;
		event.button.x = x;
		event.button.y = y;
		checked(SDL_PushEvent(&event), "queue Game UI sample click");
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
		present(floating);
		button(SDL_EVENT_MOUSE_BUTTON_UP, pos.x, pos.y);
		present(floating);
		present(floating);
		CHECK(sample.m_activations == 4 && floatingClicked,
			"a click before NewFrame belongs to the ImGui window covering Game UI");

		SDL_WarpMouseInWindow(engine.window, pos.x, pos.y);
		frames(2);
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, pos.x, pos.y);
		engine.handleEvents();
		CHECK(sample.m_armed && sample.m_ownsPress, "Game UI owns the uncovered press");
		engine.setMouseCaptured(true);
		CHECK(engine.mouseCaptured && !sample.m_armed && sample.m_ownsPress,
			"gameplay capture cancels activation and preserves release ownership");
		const int releasesBeforeCapture = queuedReleases();
		engine.setMouseCaptured(false);
		button(SDL_EVENT_MOUSE_BUTTON_UP, pos.x, pos.y);
		engine.handleEvents();
		CHECK(!engine.mouseCaptured && !sample.m_ownsPress && sample.m_activations == 4 &&
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
		CHECK(!sample.m_armed && sample.m_ownsPress, "opening a dialog between SDL events cancels activation");
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
		CHECK(!sample.m_ownsPress && sample.m_activations == 4 && queuedReleases() == releasesBeforeDialog,
			"closing the dialog keeps the cancelled release with its original owner");
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
			frames(4);
			const auto stats = sample.renderer().stats(0);
			CHECK(stats.descriptorSets == baseline.descriptorSets && stats.instanceBytes == baseline.instanceBytes &&
				stats.fontBytes == baseline.fontBytes && stats.compositionBytes == baseline.compositionBytes &&
				stats.draws <= 8 && stats.dropped == 0, "resize, scale and reload retain a bounded UI allocation footprint");
		}
		pointerProof();
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

	void performance()
	{
		auto &gpuProfiler = engine.frameCtx->gpuProfiler();
		if (!gpuProfiler.supported() || !gpuProfiler.enabled())
		{
			CHECK(false, gpuProfiler.status());
			return;
		}
		init();
		engine.m_inspectionView = true;
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

int runGameUiPerformance(Engine &engine)
{
	GameUiBackendProbe(engine).performance();
	return testmain::exitCode();
}
