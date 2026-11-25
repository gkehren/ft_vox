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
	/// Backing storage for io.IniFilename during the ini-persistence proof: ImGui
	/// keeps the pointer, so it has to outlive every frame.
	std::string m_iniPath;

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

	void button(Uint32 type, float x, float y, Uint8 which = SDL_BUTTON_LEFT)
	{
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

	void clickAt(glm::vec2 point)
	{
		SDL_WarpMouseInWindow(engine.window, point.x, point.y);
		motion(point.x, point.y);
		button(SDL_EVENT_MOUSE_BUTTON_DOWN, point.x, point.y);
		button(SDL_EVENT_MOUSE_BUTTON_UP, point.x, point.y);
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

			const VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
			world.resizeOutput(extent, format, kColorSpace);
			sample.renderer().setOutput(format, kColorSpace);
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

		// S04: the Engine settings seam, on an isolated directory. A missing record
		// stays silent, a corrupt one warns and stays untouched until the player
		// selects a value, a failed write stays dirty and the next explicit
		// selection persists.
		{
			namespace fs = std::filesystem;
			const auto previousRoot = engine.m_savesRoot;
			const auto root = fs::temp_directory_path() / "ftvox-player-ui-proof";
			std::error_code ec;
			fs::remove_all(root, ec);
			engine.setSavesRootForTests(root);
			const auto record = root / ".client-profiles" / "player-ui.txt";
			const auto readRecord = [&record] {
				std::ifstream in(record, std::ios::binary);
				return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			};

			engine.loadPlayerUiPreferences();
			CHECK(engine.m_playerUiSettings.scale.mode == gameui::ScaleMode::Auto &&
					!engine.m_playerUiDirty && engine.m_playerUiError.empty() && !fs::exists(record),
				"a missing preference record loads Auto defaults without a warning or a write");
			engine.applyPlayerUiScaleRequests();
			CHECK(!fs::exists(record), "no unsolicited write happens without a selection");

			fs::create_directories(record.parent_path(), ec);
			{
				std::ofstream out(record, std::ios::binary | std::ios::trunc);
				out << "ftvox-player-ui 9\nscale auto\n";
			}
			const auto corruptBytes = readRecord();
			engine.loadPlayerUiPreferences();
			CHECK(engine.m_playerUiDirty && !engine.m_playerUiError.empty() &&
					engine.m_playerUiSettings.scale.mode == gameui::ScaleMode::Auto,
				"a corrupt preference record warns and asks for a repair");
			engine.applyPlayerUiScaleRequests();
			CHECK(readRecord() == corruptBytes,
				"a corrupt record stays untouched while the player does not interact");

			sample.m_scaleRequest = gameui::ScalePolicy{gameui::ScaleMode::Auto, 1.f};
			engine.applyPlayerUiScaleRequests();
			gameui::PlayerUiSettings repaired;
			CHECK(repaired.load(record) == gameui::LoadStatus::Ok && engine.m_playerUiError.empty() &&
					!engine.m_playerUiDirty,
				"selecting the current default repairs the record and clears the warning");

			// A destination that cannot be replaced (a directory in its place) keeps
			// the change in memory and the store dirty.
			fs::remove(record, ec);
			fs::create_directories(record, ec);
			sample.m_scaleRequest = gameui::ScalePolicy{gameui::ScaleMode::Fixed, 1.5f};
			engine.applyPlayerUiScaleRequests();
			CHECK(engine.m_playerUiSettings.scale.mode == gameui::ScaleMode::Fixed &&
					engine.m_playerUiSettings.scale.value == 1.5f && engine.m_playerUiDirty &&
					!engine.m_playerUiError.empty(),
				"a failed write keeps the in-memory preference and the dirty state");
			fs::remove_all(record, ec);
			sample.m_scaleRequest = gameui::ScalePolicy{gameui::ScaleMode::Fixed, 1.5f};
			engine.applyPlayerUiScaleRequests();
			gameui::PlayerUiSettings persisted;
			CHECK(persisted.load(record) == gameui::LoadStatus::Ok &&
					persisted.scale.mode == gameui::ScaleMode::Fixed && persisted.scale.value == 1.5f &&
					engine.m_playerUiError.empty() && !engine.m_playerUiDirty,
				"the same selection after recovery persists and clears the warning");

			engine.setSavesRootForTests(previousRoot);
			fs::remove_all(root, ec);
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
		// The legacy inventory keeps working in both layouts: docked/floating in
		// the workspace, and floating over gameplay while it is hidden.
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
		CHECK(engine.inventoryPanelOpen() && engine.developerUiAvailable() &&
				!engine.developerWorkspaceVisible(),
			"a hidden workspace still opens the legacy inventory as a floating window");
		tap(SDLK_ESCAPE);
		present();
		CHECK(!engine.inventoryPanelOpen() && engine.mouseCaptured,
			"Escape closes the floating inventory and returns to the HUD");
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
		tap(SDLK_Q);
		present();
		CHECK(engine.inventoryPanelOpen() && engine.developerUiAvailable() && !hasStoneIcon() &&
			engine.playerHudView().hotbar.slots[0].count == 11,
			"inventory hides the HUD and consumes gameplay input without the workspace");
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

	/// The persisted inventory record of the canonical window, straight out of the
	/// settings blob ImGui would write (issue #303 F01/F12). Empty when the record
	/// is absent.
	std::string inventoryIniRecord()
	{
		const char *ini = ImGui::SaveIniSettingsToMemory();
		const std::string text = ini ? ini : "";
		const auto start = text.find("[Window][Inventory]\n");
		if (start == std::string::npos)
			return {};
		const auto end = text.find("\n[", start + 1);
		return end == std::string::npos ? text.substr(start) : text.substr(start, end + 1 - start);
	}

	/// F04/F09/F11/F12: the workspace visibility chord, its benchmark guard,
	/// developer-input cancellation and the inventory docking identity are
	/// exercised through real SDL events, not through direct setter calls.
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

		// (6) F01: the hidden-workspace inventory is a separate, non-persistent
		// identity, so the canonical window keeps its dock and its settings record.
		tap(SDLK_E);
		frames(2);
		checked(engine.inventoryPanelOpen(), "the inventory opens in the visible workspace");
		const ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(shell.dockspaceId());
		checked(central && central->ID != 0, "the dockspace exposes a central node to dock into");
		ImGui::DockBuilderDockWindow("Inventory", central ? central->ID : shell.dockspaceId());
		ImGui::DockBuilderFinish(shell.dockspaceId());
		frames(2);
		const ImGuiWindow *canonical = ImGui::FindWindowByName("Inventory");
		checked(canonical != nullptr, "the canonical inventory window exists");
		const ImGuiID dockedId = canonical ? canonical->DockId : 0;
		const auto recordBefore = inventoryIniRecord();
		CHECK(dockedId != 0 && recordBefore.find("DockId=") != std::string::npos,
			"the inventory is docked with a persisted association before hiding (dock=" << dockedId
				<< " record=[" << recordBefore << "])");
		tap(SDLK_D, chord);
		present();
		frames(2);
		const ImGuiWindow *floating = ImGui::FindWindowByName("Inventory###inventory-floating");
		CHECK(engine.inventoryPanelOpen() && floating != nullptr,
			"the hidden workspace keeps the inventory open through its temporary identity");
		CHECK(floating && floating->DockId == 0 &&
			(floating->Flags & ImGuiWindowFlags_NoDocking) &&
			(floating->Flags & ImGuiWindowFlags_NoSavedSettings),
			"the temporary inventory refuses docking and the settings file");
		const ImGuiWindow *stillCanonical = ImGui::FindWindowByName("Inventory");
		CHECK(stillCanonical && stillCanonical->DockId == dockedId,
			"opening the floating inventory does not undock the canonical window");
		const auto recordWhileHidden = inventoryIniRecord();
		CHECK(recordWhileHidden == recordBefore &&
			recordWhileHidden.find("inventory-floating") == std::string::npos,
			"the persisted inventory record is untouched while the workspace is hidden");
		tap(SDLK_E);
		present();
		CHECK(!engine.inventoryPanelOpen(), "the floating inventory still closes normally");
		engine.setDeveloperWorkspaceVisible(true);
		frames(2);
		tap(SDLK_E);
		frames(2);
		const ImGuiWindow *restored = ImGui::FindWindowByName("Inventory");
		CHECK(engine.inventoryPanelOpen() && restored && restored->DockId == dockedId &&
			inventoryIniRecord() == recordBefore,
			"restoring the workspace reopens the inventory in its original dock");
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
	/// F01/F12 across a real process boundary: the same ini file is written by the
	/// `dock` phase (workspace visible, inventory docked into the central node) and
	/// then re-opened by the `hidden` phase, which starts without the developer
	/// workspace and opens the inventory through the temporary identity. The
	/// canonical `[Window][Inventory]` record must come back byte-identical.
	void iniPersistence(const std::filesystem::path &ini, std::string_view phase)
	{
		init();
		m_iniPath = ini.string();
		ImGui::GetIO().IniFilename = m_iniPath.c_str();
		if (std::filesystem::exists(ini))
			ImGui::LoadIniSettingsFromDisk(m_iniPath.c_str());
		const bool dock = phase == "dock";
		engine.setDeveloperWorkspaceVisible(dock);
		engine.setInventoryOpen(false);
		frames(3);
		tap(SDLK_E);
		frames(2);
		checked(engine.inventoryPanelOpen(), "the inventory is open for the ini proof");
		if (dock)
		{
			const ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(engine.gameUi->shell().dockspaceId());
			checked(central && central->ID != 0, "the dockspace exposes a central node");
			ImGui::DockBuilderDockWindow("Inventory", central ? central->ID : engine.gameUi->shell().dockspaceId());
			ImGui::DockBuilderFinish(engine.gameUi->shell().dockspaceId());
			frames(2);
			tap(SDLK_E);
			frames(1);
			ImGui::SaveIniSettingsToDisk(m_iniPath.c_str());
			const auto record = iniFileInventoryRecord(ini);
			std::cout << "ini dock phase record: " << record << '\n';
			CHECK(record.find("DockId=") != std::string::npos &&
				record.find("DockId=0x00000000") == std::string::npos,
				"the dock phase persists a real docking association");
		}
		else
		{
			const auto before = iniFileInventoryRecord(ini);
			checked(before.find("DockId=0x00000000") == std::string::npos && !before.empty(),
				"the hidden phase starts from a persisted docking association");
			tap(SDLK_E); // close through the temporary identity
			frames(1);
			tap(SDLK_E); // and open it again
			frames(2);
			checked(engine.inventoryPanelOpen() &&
				ImGui::FindWindowByName("Inventory###inventory-floating") != nullptr,
				"the hidden workspace opens the inventory through its temporary identity");
			ImGui::SaveIniSettingsToDisk(m_iniPath.c_str());
			const auto after = iniFileInventoryRecord(ini);
			std::cout << "ini hidden phase record: " << after << '\n';
			CHECK(after == before,
				"opening and closing the inventory with the workspace hidden keeps its persisted record");
		}
	}

	std::string iniFileInventoryRecord(const std::filesystem::path &ini) const
	{
		std::ifstream stream(ini);
		if (!stream)
			return {};
		std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		const auto start = text.find("[Window][Inventory]\n");
		if (start == std::string::npos)
			return {};
		const auto end = text.find("\n[", start + 1);
		return end == std::string::npos ? text.substr(start) : text.substr(start, end + 1 - start);
	}
};

int runGameUiIniPersistence(Engine &engine, const std::filesystem::path &ini, std::string_view phase)
{
	GameUiBackendProbe(engine).iniPersistence(ini, phase);
	if (testmain::fails == 0)
		std::cout << "PASS: inventory ini persistence, " << phase << " phase (" << testmain::checks
			<< " checks)\n";
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
