#include "Engine/ImGuiLayer.hpp"

#include <imgui/imgui.h>
#include <imgui/imgui_impl_sdl3.h>
#include <imgui/imgui_impl_vulkan.h>

#include <algorithm>
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
} // namespace

ImGuiLayer::~ImGuiLayer()
{
	shutdown();
}

void ImGuiLayer::init(SDL_Window *window, VkContext &context, VkSwapchain &swapchain, ImmediateCommands &imm)
{
	if (m_initialized)
		return;

	m_device = context.getDevice();

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	ImGui::StyleColorsDark();

	if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_2, imguiVulkanLoader, &context))
		throw std::runtime_error("ImGui_ImplVulkan_LoadFunctions failed");

	if (!ImGui_ImplSDL3_InitForVulkan(window))
		throw std::runtime_error("ImGui_ImplSDL3_InitForVulkan failed");

	ImGui_ImplVulkan_InitInfo initInfo{};
	initInfo.ApiVersion = VK_API_VERSION_1_2;
	initInfo.Instance = context.getInstance();
	initInfo.PhysicalDevice = context.getPhysicalDevice();
	initInfo.Device = context.getDevice();
	initInfo.QueueFamily = context.getGraphicsQueueFamily();
	initInfo.Queue = context.getGraphicsQueue();
	initInfo.DescriptorPool = VK_NULL_HANDLE;
	initInfo.DescriptorPoolSize = 64; // let backend create pool
	initInfo.MinImageCount = 2;
	initInfo.ImageCount = std::max(2u, swapchain.getImageCount());
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

	// Font upload via one-shot commands
	imm.submitAndWait([&](VkCommandBuffer cmd) {
		// CreateFontsTexture may queue uploads using internal helpers in some versions;
		// 1.91.9 creates device objects that are ready after first NewFrame if DescriptorPoolSize is set.
		(void)cmd;
	});
	ImGui_ImplVulkan_CreateFontsTexture();

	// Destroy staging objects if the API provides it (present in 1.91.x)
	// After create, some versions require a submit — CreateFontsTexture already uses host-visible
	// uploads in recent backends. If not, first frame still works once font atlas is ready.

	m_initialized = true;
}

void ImGuiLayer::shutdown()
{
	if (!m_initialized)
		return;
	if (m_device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_device);

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
	m_initialized = false;
	m_device = VK_NULL_HANDLE;
}

void ImGuiLayer::processEvent(const SDL_Event &event)
{
	if (m_initialized)
		ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::beginFrame()
{
	if (!m_initialized)
		return;
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
