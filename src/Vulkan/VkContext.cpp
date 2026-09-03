#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkAllocator.hpp"

#include <iostream>
#include <set>
#include <cstring>
#include <array>
#include <cstdlib>

namespace
{
VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT /*types*/,
	const VkDebugUtilsMessengerCallbackDataEXT *callbackData,
	void * /*userData*/)
{
	if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		std::cerr << "[Vulkan] " << callbackData->pMessage << "\n";
	}
	return VK_FALSE;
}

bool checkLayerSupport(const char *layerName)
{
	uint32_t layerCount = 0;
	vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
	std::vector<VkLayerProperties> available(layerCount);
	vkEnumerateInstanceLayerProperties(&layerCount, available.data());
	for (const auto &layer : available)
	{
		if (std::strcmp(layer.layerName, layerName) == 0)
			return true;
	}
	return false;
}

bool checkExtensionSupport(VkPhysicalDevice device, const std::vector<const char *> &required)
{
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> available(count);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &count, available.data());

	for (const char *req : required)
	{
		bool found = false;
		for (const auto &ext : available)
		{
			if (std::strcmp(ext.extensionName, req) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}
	return true;
}
} // namespace

VkContext::~VkContext()
{
	shutdown();
}

void VkContext::waitIdle() const
{
	if (m_device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_device);
}

void VkContext::init(SDL_Window *window)
{
	auto getProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
	if (!getProcAddr)
		throw std::runtime_error("Failed to get vkGetInstanceProcAddr from SDL: " + std::string(SDL_GetError()));
	volkInitializeCustom(getProcAddr);

	createInstance(window);
	volkLoadInstance(m_instance);
	setupDebugMessenger();
	createSurface(window);
	pickPhysicalDevice();
	createLogicalDevice();
	volkLoadDevice(m_device);
	if ((!vkCmdBeginRendering && !vkCmdBeginRenderingKHR) ||
		(!vkCmdEndRendering && !vkCmdEndRenderingKHR))
	{
		throw std::runtime_error(
			"Dynamic rendering was enabled, but its Vulkan entry points could not be loaded");
	}
	createAllocator();

	std::cout << "Vulkan device: " << m_deviceProperties.deviceName << "\n";
	std::cout << "Vulkan API: "
			  << VK_VERSION_MAJOR(m_deviceProperties.apiVersion) << "."
			  << VK_VERSION_MINOR(m_deviceProperties.apiVersion) << "."
			  << VK_VERSION_PATCH(m_deviceProperties.apiVersion) << "\n";
	std::cout << "  validation=" << (m_validationEnabled ? "yes" : "no")
			  << " dynamicRendering=" << (m_dynamicRendering ? "yes" : "no")
			  << " timelineSemaphores=" << (m_timelineSemaphores ? "yes" : "no")
			  << " portabilitySubset=" << (m_portabilitySubset ? "yes" : "no") << "\n";
	std::cout << "  VMA allocator: ready\n";
}

void VkContext::createAllocator()
{
	m_allocator = std::make_unique<VkAllocator>();
	m_allocator->init(m_instance, m_physicalDevice, m_device, kApiVersion);
}

VmaAllocator VkContext::getAllocator() const
{
	if (!m_allocator || !m_allocator->isValid())
		throw std::runtime_error("VMA allocator not initialized");
	return m_allocator->handle();
}

VkAllocator &VkContext::getAllocatorOwner()
{
	if (!m_allocator)
		throw std::runtime_error("VMA allocator not initialized");
	return *m_allocator;
}

void VkContext::shutdown()
{
	if (m_device != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_device);

	// Allocator must outlive all VMA resources but be destroyed before the device.
	if (m_allocator)
	{
		m_allocator->shutdown();
		m_allocator.reset();
	}

	if (m_device != VK_NULL_HANDLE)
	{
		vkDestroyDevice(m_device, nullptr);
		m_device = VK_NULL_HANDLE;
	}

	if (m_instance != VK_NULL_HANDLE && m_surface != VK_NULL_HANDLE)
	{
		vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
		m_surface = VK_NULL_HANDLE;
	}

	if (m_debugMessenger != VK_NULL_HANDLE)
	{
		vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
		m_debugMessenger = VK_NULL_HANDLE;
	}

	if (m_instance != VK_NULL_HANDLE)
	{
		vkDestroyInstance(m_instance, nullptr);
		m_instance = VK_NULL_HANDLE;
	}

	m_physicalDevice = VK_NULL_HANDLE;
	m_graphicsQueue = VK_NULL_HANDLE;
	m_presentQueue = VK_NULL_HANDLE;
}

void VkContext::createInstance(SDL_Window *window)
{
	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "ft_vox";
	appInfo.applicationVersion = VK_MAKE_VERSION(0, 2, 0);
	appInfo.pEngineName = "ft_vox";
	appInfo.engineVersion = VK_MAKE_VERSION(0, 2, 0);
	appInfo.apiVersion = kApiVersion;

	Uint32 sdlExtCount = 0;
	const char *const *sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
	if (!sdlExts)
		throw std::runtime_error(std::string("SDL_Vulkan_GetInstanceExtensions failed: ") + SDL_GetError());

	std::vector<const char *> extensions(sdlExts, sdlExts + sdlExtCount);

#if defined(__APPLE__)
	extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#endif

	// Validation: on by default in Debug. Force with FT_VOX_VALIDATION=1 (or =0 to disable).
	// Layers are discovered at runtime via VK_LAYER_PATH (not linked into the binary).
	const char *validationEnv = std::getenv("FT_VOX_VALIDATION");
	bool requestValidation =
#ifndef NDEBUG
		true;
#else
		false;
#endif
	if (validationEnv != nullptr)
	{
		if (std::strcmp(validationEnv, "0") == 0 || std::strcmp(validationEnv, "false") == 0)
			requestValidation = false;
		else if (std::strcmp(validationEnv, "1") == 0 || std::strcmp(validationEnv, "true") == 0)
			requestValidation = true;
	}

	if (requestValidation)
	{
		const bool haveLayer = checkLayerSupport("VK_LAYER_KHRONOS_validation");
		m_validationEnabled = haveLayer;
		if (haveLayer)
		{
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}
		else
		{
			std::cerr << "Warning: VK_LAYER_KHRONOS_validation not available.\n"
					  << "  Install layers and set VK_LAYER_PATH, e.g. on macOS:\n"
					  << "    brew install vulkan-validationlayers\n"
					  << "    export VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d\n"
					  << "  Set FT_VOX_VALIDATION=0 to silence this warning.\n";
		}
	}
	else
	{
		m_validationEnabled = false;
	}

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();

#if defined(__APPLE__)
	createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif

	const char *validationLayer = "VK_LAYER_KHRONOS_validation";
	VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
	if (m_validationEnabled)
	{
		createInfo.enabledLayerCount = 1;
		createInfo.ppEnabledLayerNames = &validationLayer;

		debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		debugCreateInfo.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debugCreateInfo.messageType =
			VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debugCreateInfo.pfnUserCallback = debugCallback;
		createInfo.pNext = &debugCreateInfo;
	}

	const VkResult instResult = vkCreateInstance(&createInfo, nullptr, &m_instance);
	if (instResult != VK_SUCCESS)
	{
		std::string msg = "Failed to create Vulkan instance (VkResult=" + std::to_string(static_cast<int>(instResult)) + ")";
		if (instResult == VK_ERROR_INCOMPATIBLE_DRIVER)
		{
			msg += "\n  Incompatible driver — on macOS ensure portability enum is enabled and "
				   "VK_ICD_FILENAMES points at MoltenVK_icd.json";
		}
		else if (instResult == VK_ERROR_EXTENSION_NOT_PRESENT)
		{
			msg += "\n  A requested instance extension is missing. Extensions:";
			for (const char *e : extensions)
				msg += std::string("\n    - ") + e;
		}
		else if (instResult == VK_ERROR_LAYER_NOT_PRESENT)
		{
			msg += "\n  Validation layer missing. Set FT_VOX_VALIDATION=0 or install layers:\n"
				   "    brew install vulkan-validationlayers\n"
				   "    export VK_LAYER_PATH=/opt/homebrew/opt/vulkan-validationlayers/share/vulkan/explicit_layer.d";
		}
		else if (instResult == VK_ERROR_INITIALIZATION_FAILED)
		{
			msg += "\n  Loader init failed — check VK_ICD_FILENAMES (MoltenVK ICD) and that libvulkan "
				   "was loaded (not a broken dylib).";
		}
		throw std::runtime_error(msg);
	}
}

void VkContext::setupDebugMessenger()
{
	if (!m_validationEnabled)
		return;

	VkDebugUtilsMessengerCreateInfoEXT createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
	createInfo.messageSeverity =
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	createInfo.messageType =
		VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	createInfo.pfnUserCallback = debugCallback;

	if (vkCreateDebugUtilsMessengerEXT(m_instance, &createInfo, nullptr, &m_debugMessenger) != VK_SUCCESS)
		throw std::runtime_error("Failed to set up Vulkan debug messenger");
}

void VkContext::createSurface(SDL_Window *window)
{
	if (!SDL_Vulkan_CreateSurface(window, m_instance, nullptr, &m_surface))
		throw std::runtime_error(std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError());
}

QueueFamilyIndices VkContext::findQueueFamilies(VkPhysicalDevice device) const
{
	QueueFamilyIndices indices;
	uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

	for (uint32_t i = 0; i < count; ++i)
	{
		if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			indices.graphicsFamily = i;

		VkBool32 presentSupport = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, m_surface, &presentSupport);
		if (presentSupport)
			indices.presentFamily = i;

		if (indices.isComplete())
			break;
	}
	return indices;
}

std::vector<const char *> VkContext::getRequiredDeviceExtensions() const
{
	std::vector<const char *> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	if (kApiVersion < VK_API_VERSION_1_3)
		exts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
#if defined(__APPLE__)
	exts.push_back("VK_KHR_portability_subset");
#endif
	return exts;
}

bool VkContext::isDeviceSuitable(VkPhysicalDevice device) const
{
	QueueFamilyIndices indices = findQueueFamilies(device);
	if (!indices.isComplete())
		return false;

	auto required = getRequiredDeviceExtensions();
	if (!checkExtensionSupport(device, required))
		return false;

	uint32_t formatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device, m_surface, &formatCount, nullptr);
	uint32_t presentModeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(device, m_surface, &presentModeCount, nullptr);
	return formatCount > 0 && presentModeCount > 0;
}

void VkContext::pickPhysicalDevice()
{
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
	if (deviceCount == 0)
		throw std::runtime_error("No Vulkan-capable GPUs found (is MoltenVK/ICD installed?)");

	std::vector<VkPhysicalDevice> devices(deviceCount);
	vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());

	// Prefer discrete GPU, then any suitable device
	VkPhysicalDevice best = VK_NULL_HANDLE;
	int bestScore = -1;
	for (VkPhysicalDevice device : devices)
	{
		if (!isDeviceSuitable(device))
			continue;

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(device, &props);
		int score = 0;
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			score += 1000;
		else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
			score += 100;
		score += static_cast<int>(props.limits.maxImageDimension2D / 1024);

		if (score > bestScore)
		{
			bestScore = score;
			best = device;
		}
	}

	if (best == VK_NULL_HANDLE)
		throw std::runtime_error("No suitable Vulkan GPU found for presentation");

	m_physicalDevice = best;
	vkGetPhysicalDeviceProperties(m_physicalDevice, &m_deviceProperties);
	m_queueFamilies = findQueueFamilies(m_physicalDevice);
}

void VkContext::createLogicalDevice()
{
	std::set<uint32_t> uniqueFamilies = {
		m_queueFamilies.graphicsFamily.value(),
		m_queueFamilies.presentFamily.value()};

	float priority = 1.0f;
	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	for (uint32_t family : uniqueFamilies)
	{
		VkDeviceQueueCreateInfo qci{};
		qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qci.queueFamilyIndex = family;
		qci.queueCount = 1;
		qci.pQueuePriorities = &priority;
		queueCreateInfos.push_back(qci);
	}

	// Query available features / extensions for optional 1.3 paths
	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, nullptr);
	std::vector<VkExtensionProperties> availableExts(extCount);
	vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extCount, availableExts.data());

	auto hasExt = [&](const char *name) {
		for (const auto &e : availableExts)
			if (std::strcmp(e.extensionName, name) == 0)
				return true;
		return false;
	};

	std::vector<const char *> deviceExtensions = getRequiredDeviceExtensions();

	// Dynamic rendering is core only when both the application and device use
	// Vulkan 1.3+. A newer device version cannot promote commands for an instance
	// created with the engine's Vulkan 1.2 API target.
	const bool dynRenderExt = hasExt(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
	const bool dynRenderCore =
		kApiVersion >= VK_API_VERSION_1_3 &&
		m_deviceProperties.apiVersion >= VK_API_VERSION_1_3;
	m_dynamicRendering = dynRenderCore || dynRenderExt;
	if (!m_dynamicRendering)
		throw std::runtime_error(
			"Selected Vulkan device lacks dynamic rendering (requires Vulkan 1.3 or VK_KHR_dynamic_rendering)");
	// Timeline semaphores: core 1.2
	m_timelineSemaphores = true;

	m_portabilitySubset = hasExt("VK_KHR_portability_subset");

	VkPhysicalDeviceFeatures deviceFeatures{};
	deviceFeatures.samplerAnisotropy = VK_TRUE;
	deviceFeatures.fillModeNonSolid = VK_TRUE; // wireframe-style polygon mode when available

	VkPhysicalDeviceVulkan12Features features12{};
	features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	features12.timelineSemaphore = VK_TRUE;

	VkPhysicalDeviceDynamicRenderingFeatures dynFeatures{};
	dynFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
	dynFeatures.dynamicRendering = m_dynamicRendering ? VK_TRUE : VK_FALSE;

	if (m_dynamicRendering)
		features12.pNext = &dynFeatures;

	// On some MoltenVK builds, enabling unsupported feature bits fails device creation.
	// Query first and only enable what is available.
	VkPhysicalDeviceVulkan12Features available12{};
	available12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	VkPhysicalDeviceDynamicRenderingFeatures availableDyn{};
	availableDyn.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
	available12.pNext = &availableDyn;

	VkPhysicalDeviceFeatures2 features2{};
	features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	features2.pNext = &available12;
	vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features2);

	features12.timelineSemaphore = available12.timelineSemaphore;
	m_timelineSemaphores = available12.timelineSemaphore == VK_TRUE;

	if (m_dynamicRendering)
	{
		dynFeatures.dynamicRendering = availableDyn.dynamicRendering;
		m_dynamicRendering = dynFeatures.dynamicRendering == VK_TRUE;
		if (!m_dynamicRendering)
			throw std::runtime_error(
				"Selected Vulkan device exposes dynamic rendering but not its required feature");
	}

	// Only request features the device actually has
	deviceFeatures = features2.features;
	// Keep a conservative subset we rely on later
	VkPhysicalDeviceFeatures enabledFeatures{};
	enabledFeatures.samplerAnisotropy = features2.features.samplerAnisotropy;
	enabledFeatures.fillModeNonSolid = features2.features.fillModeNonSolid;

	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.pEnabledFeatures = &enabledFeatures;
	createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
	createInfo.ppEnabledExtensionNames = deviceExtensions.data();
	createInfo.pNext = &features12;

	if (vkCreateDevice(m_physicalDevice, &createInfo, nullptr, &m_device) != VK_SUCCESS)
		throw std::runtime_error("Failed to create Vulkan logical device");

	vkGetDeviceQueue(m_device, m_queueFamilies.graphicsFamily.value(), 0, &m_graphicsQueue);
	vkGetDeviceQueue(m_device, m_queueFamilies.presentFamily.value(), 0, &m_presentQueue);
}
