#include "Engine/ImGuiLayer.hpp"

#include <Engine/UiTheme.hpp>

#include <imgui/imgui.h>
#include <imgui/imgui_impl_sdl3.h>
#include <imgui/imgui_impl_vulkan.h>
#include <imgui/imgui_internal.h> // settings-handler registration

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{
void checkVk(VkResult err)
{
	if (err != VK_SUCCESS)
		throw std::runtime_error("ImGui Vulkan backend VkResult error: " + std::to_string(static_cast<int>(err)));
}

PFN_vkVoidFunction imguiVulkanLoader(const char *function_name, void *user_data)
{
	auto *ctx = static_cast<VkContext *>(user_data);
	// Prefer device-level, then instance-level (covers both core and KHR).
	PFN_vkVoidFunction fn = vkGetDeviceProcAddr(ctx->getDevice(), function_name);
	if (!fn)
		fn = vkGetInstanceProcAddr(ctx->getInstance(), function_name);
	return fn;
}

/// Logical-size -> pixel-size ratio (1 on Windows where screen coordinates
/// are physical, 2 on Retina / fractional-density Wayland), from SDL3's
/// dedicated window-pixel-density query.
float queryFramebufferScale(SDL_Window *window)
{
	if (!window)
		return 1.f;
	const float density = SDL_GetWindowPixelDensity(window);
	return density > 0.f ? density : 1.f;
}

constexpr const char *kUiSettingsTypeName = "FtVoxUi";

void *uiSettingsReadOpen(ImGuiContext *, ImGuiSettingsHandler *, const char *name)
{
	return std::strcmp(name, "Scale") == 0 ? const_cast<char *>(name) : nullptr;
}

void uiSettingsReadLine(ImGuiContext *, ImGuiSettingsHandler *handler, void *, const char *line)
{
	float value = 0.f;
	if (std::sscanf(line, "ui_scale=%f", &value) == 1 && handler->UserData)
		*static_cast<float *>(handler->UserData) = ui::snapScale(ui::clampScale(value));
}

void uiSettingsWriteAll(ImGuiContext *, ImGuiSettingsHandler *handler, ImGuiTextBuffer *buf)
{
	if (!handler->UserData)
		return;
	buf->appendf("[%s][Scale]\nui_scale=%.3f\n\n", handler->TypeName,
				 *static_cast<const float *>(handler->UserData));
}
} // namespace

ImGuiLayer::~ImGuiLayer()
{
	shutdown();
}

void ImGuiLayer::init(SDL_Window *window, VkContext &context, VkSwapchain &swapchain)
{
	if (m_initialized)
		return;

	m_device = context.getDevice();
	m_context = &context;
	m_window = window;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	// Docked windows must be moved by their title bar, not their body: the
	// central viewport region stays a click-through game view (issue #183).
	io.ConfigWindowsMoveFromTitleBarOnly = true;

	// Initial UI scale from the display content scale; a persisted override
	// in imgui.ini wins when present (issue #183). The display scale is
	// divided by the framebuffer scale so platforms where the logical window
	// size already differs from the pixel size (macOS, Wayland) do not
	// double-scale — there the OS scaling is the framebuffer ratio itself.
	m_framebufferScale = queryFramebufferScale(window);
	m_uiScale = ui::snapScale(ui::clampScale(SDL_GetWindowDisplayScale(window) / m_framebufferScale));
	registerScaleSettingsHandler();
	{
		std::error_code ec;
		m_hadIniAtStartup = io.IniFilename && std::filesystem::exists(io.IniFilename, ec);
	}
	if (m_hadIniAtStartup)
		ImGui::LoadIniSettingsFromDisk(io.IniFilename); // docking layout + persisted UI scale

	ui::applyStyle(m_uiScale);
	ui::rebuildFontAtlas(m_uiScale, m_framebufferScale);

	if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_2, imguiVulkanLoader, &context))
		throw std::runtime_error("ImGui_ImplVulkan_LoadFunctions failed");

	if (!ImGui_ImplSDL3_InitForVulkan(window))
		throw std::runtime_error("ImGui_ImplSDL3_InitForVulkan failed");
	m_initialized = true;
	initVulkanBackend(context, swapchain);
}

void ImGuiLayer::registerScaleSettingsHandler()
{
	ImGuiSettingsHandler handler{};
	handler.TypeName = kUiSettingsTypeName;
	handler.TypeHash = ImHashStr(kUiSettingsTypeName);
	handler.ReadOpenFn = uiSettingsReadOpen;
	handler.ReadLineFn = uiSettingsReadLine;
	handler.WriteAllFn = uiSettingsWriteAll;
	handler.UserData = &m_uiScale;
	ImGui::AddSettingsHandler(&handler);
}

void ImGuiLayer::requestUiScale(float scale)
{
	if (scale > 0.f)
		m_pendingUiScale = scale;
}

void ImGuiLayer::initVulkanBackend(VkContext &context, VkSwapchain &swapchain)
{
	const uint32_t imageCount = std::max(2u, swapchain.getImageCount());
	const uint32_t minImageCount =
		std::min(imageCount, std::max(2u, swapchain.getMinImageCount()));

	ImGui_ImplVulkan_InitInfo initInfo{};
	initInfo.ApiVersion = VK_API_VERSION_1_2;
	initInfo.Instance = context.getInstance();
	initInfo.PhysicalDevice = context.getPhysicalDevice();
	initInfo.Device = context.getDevice();
	initInfo.QueueFamily = context.getGraphicsQueueFamily();
	initInfo.Queue = context.getGraphicsQueue();
	initInfo.DescriptorPool = VK_NULL_HANDLE;
	// Backend-created pool sized for the font + biome map + the per-block
	// item icon descriptors (issue #229: up to COUNT lazily-created atlas
	// layer views for the hotbar/inventory icons).
	initInfo.DescriptorPoolSize = 256;
	initInfo.MinImageCount = minImageCount;
	initInfo.ImageCount = imageCount;
	initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	initInfo.PipelineCache = VK_NULL_HANDLE;
	initInfo.Subpass = 0;
	initInfo.UseDynamicRendering = true;
	initInfo.CheckVkResultFn = checkVk;
	initInfo.MinAllocationSize = 1024 * 1024;

#ifdef IMGUI_IMPL_VULKAN_HAS_DYNAMIC_RENDERING
	VkPipelineRenderingCreateInfoKHR renderingInfo{};
	renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
	VkFormat colorFormat = swapchain.getImageFormat();
	renderingInfo.colorAttachmentCount = 1;
	renderingInfo.pColorAttachmentFormats = &colorFormat;
	// ImGui is drawn without depth testing; leave depth format undefined / 0
	renderingInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
	renderingInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
	initInfo.PipelineRenderingCreateInfo = renderingInfo;
#endif

	if (!ImGui_ImplVulkan_Init(&initInfo))
		throw std::runtime_error("ImGui_ImplVulkan_Init failed");
	m_vulkanInitialized = true;

	ImGui_ImplVulkan_CreateFontsTexture();

	// Destroy staging objects if the API provides it (present in 1.91.x)
	// After create, some versions require a submit — CreateFontsTexture already uses host-visible
	// uploads in recent backends. If not, first frame still works once font atlas is ready.

	m_colorFormat = swapchain.getImageFormat();
	m_swapchainImageCount = swapchain.getImageCount();
	m_minImageCount = swapchain.getMinImageCount();
}

bool ImGuiLayer::onSwapchainRecreate(VkSwapchain &swapchain)
{
	if (!m_initialized || !m_vulkanInitialized || !m_context)
		return false;

	if (m_colorFormat == swapchain.getImageFormat() &&
		m_swapchainImageCount == swapchain.getImageCount() &&
		m_minImageCount == swapchain.getMinImageCount())
		return false;

	// ImageCount is immutable in the backend init info, and a dynamic-rendering
	// pipeline is tied to its color format. Reinitialize the renderer backend for
	// either change while preserving the ImGui context and SDL platform backend.
	ImGui_ImplVulkan_Shutdown();
	m_vulkanInitialized = false;
	initVulkanBackend(*m_context, swapchain);
	return true;
}

void ImGuiLayer::shutdown()
{
	if (!m_initialized && !m_vulkanInitialized)
		return;
	if (m_device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_device);

	if (m_vulkanInitialized)
		ImGui_ImplVulkan_Shutdown();
	if (m_initialized)
	{
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();
	}
	m_vulkanInitialized = false;
	m_initialized = false;
	m_device = VK_NULL_HANDLE;
	m_context = nullptr;
	m_window = nullptr;
	m_colorFormat = VK_FORMAT_UNDEFINED;
	m_swapchainImageCount = 0;
	m_minImageCount = 0;
	m_framebufferScale = 1.f;
}

void ImGuiLayer::processEvent(const SDL_Event &event)
{
	if (m_initialized)
		ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::observePointer(float x, float y)
{
	if (!m_initialized)
		return;
	const auto &origin = ImGui::GetMainViewport()->Pos;
	ImGui::GetIO().AddMousePosEvent(origin.x + x, origin.y + y);
}

void ImGuiLayer::observeModifiers(SDL_Keymod mods)
{
	if (!m_initialized)
		return;
	auto &io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, (mods & SDL_KMOD_CTRL) != 0);
	io.AddKeyEvent(ImGuiMod_Shift, (mods & SDL_KMOD_SHIFT) != 0);
	io.AddKeyEvent(ImGuiMod_Alt, (mods & SDL_KMOD_ALT) != 0);
	io.AddKeyEvent(ImGuiMod_Super, (mods & SDL_KMOD_GUI) != 0);
}

void ImGuiLayer::releaseInput()
{
	if (!m_initialized)
		return;
	auto &io = ImGui::GetIO();
	io.ClearEventsQueue();
	io.ClearInputKeys();
	io.ClearInputMouse();
	ImGui::ClearActiveID();
	ImGui::GetCurrentContext()->MovingWindow = nullptr;
	ImGui::ClearDragDrop();
}

void ImGuiLayer::beginFrame()
{
	if (!m_initialized)
		return;

	// Apply queued UI-scale changes before NewFrame: rebuild the style and
	// font atlas, then recreate the backend font texture. Both start from
	// their canonical base, so repeated changes never drift.
	if (m_pendingUiScale > 0.f)
	{
		const float nextScale = ui::snapScale(ui::clampScale(m_pendingUiScale));
		m_pendingUiScale = 0.f;
		if (nextScale != m_uiScale)
		{
			m_uiScale = nextScale;
			ui::applyStyle(m_uiScale);
			ui::rebuildFontAtlas(m_uiScale, m_framebufferScale);
			if (m_vulkanInitialized)
				ImGui_ImplVulkan_CreateFontsTexture();
			ImGui::MarkIniSettingsDirty();
		}
	}

	// Follow framebuffer-density changes (window moved to another display):
	// the backend stretches UI by the framebuffer scale, so fonts must be
	// re-rasterized at the new device size to stay crisp.
	if (m_window)
	{
		const float fbScale = queryFramebufferScale(m_window);
		if (std::fabs(fbScale - m_framebufferScale) > 0.001f)
		{
			m_framebufferScale = fbScale;
			ui::rebuildFontAtlas(m_uiScale, m_framebufferScale);
			if (m_vulkanInitialized)
				ImGui_ImplVulkan_CreateFontsTexture();
		}
	}

	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
}

void ImGuiLayer::endFrame()
{
	if (!m_initialized)
		return;
	ImGui::Render();
}

void ImGuiLayer::recordDraw(VkCommandBuffer cmd)
{
	if (!m_initialized)
		return;
	ImDrawData *drawData = ImGui::GetDrawData();
	if (drawData && drawData->CmdListsCount > 0)
		ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
}

bool ImGuiLayer::wantCaptureKeyboard() const
{
	return m_initialized && ImGui::GetIO().WantCaptureKeyboard;
}

bool ImGuiLayer::wantCaptureMouse() const
{
	return m_initialized && ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::editingText() const
{
	if (!m_initialized)
		return false;
	const auto &g = *ImGui::GetCurrentContext();
	return g.ActiveId != 0 && g.InputTextState.ID == g.ActiveId;
}

bool ImGuiLayer::hasModal() const
{
	return m_initialized && ImGui::GetTopMostPopupModal() != nullptr;
}

bool ImGuiLayer::ownsActiveInput() const
{
	if (!m_initialized)
		return false;
	const auto &g = *ImGui::GetCurrentContext();
	return g.ActiveId != 0 || !g.OpenPopupStack.empty();
}

int ImGuiLayer::inputFrame() const
{
	return m_initialized ? ImGui::GetFrameCount() : -1;
}

bool ImGuiLayer::blocksGameUiPointer(float x, float y) const
{
	if (!m_initialized)
		return false;
	const ImGuiContext &ctx = *ImGui::GetCurrentContext();
	if (!ctx.OpenPopupStack.empty() || ctx.MovingWindow || ctx.DragDropActive ||
		(ctx.ActiveId && ctx.ActiveIdMouseButton == ImGuiMouseButton_Left && ctx.IO.MouseDown[0]))
		return true;

	const auto *viewport = ImGui::GetMainViewport();
	const ImVec2 pos{viewport->Pos.x + x, viewport->Pos.y + y};
	for (const ImGuiWindow *window : ctx.Windows)
	{
		// NewFrame's hover uses WasActive and the previous event position.
		// Active and OuterRectClipped describe the most recently drawn layout.
		if (!window->Active || window->Hidden || window->Viewport != viewport ||
			(window->IsFallbackWindow && !window->WriteAccessed))
			continue;
		const bool resizable = !(window->Flags & (ImGuiWindowFlags_NoResize |
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMouseInputs));
		const float border = resizable ? ctx.Style.WindowBorderHoverPadding : 0.f;
		const ImVec2 padding{std::max(border, ctx.Style.TouchExtraPadding.x),
			std::max(border, ctx.Style.TouchExtraPadding.y)};
		if (!window->OuterRectClipped.ContainsWithPad(pos, padding))
			continue;
		const ImVec2 hole{window->Pos.x + window->HitTestHoleOffset.x,
			window->Pos.y + window->HitTestHoleOffset.y};
		const ImRect passthrough(hole, ImVec2(hole.x + window->HitTestHoleSize.x,
			hole.y + window->HitTestHoleSize.y));
		if (!passthrough.Contains(pos))
			return true;
	}
	return false;
}
