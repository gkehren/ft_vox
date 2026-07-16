#pragma once

#include <SDL3/SDL_vulkan.h>

#include <cstdlib>
#include <iostream>
#include <ostream>

/// Load the Vulkan shared library for SDL (volk/device create).
/// On macOS, Homebrew's libvulkan is often not on dyld's default path, so
/// SDL_Vulkan_LoadLibrary(nullptr) fails even when vulkaninfo works.
/// Order: FT_VOX_VULKAN_LIB → well-known Homebrew paths → nullptr (default).
/// Returns true if a library was loaded successfully.
inline bool loadVulkanLibrary(std::ostream *log = &std::cout)
{
	const char *explicitPath = std::getenv("FT_VOX_VULKAN_LIB");
	if (explicitPath && explicitPath[0] != '\0')
	{
		if (SDL_Vulkan_LoadLibrary(explicitPath))
		{
			if (log)
				*log << "Vulkan library: " << explicitPath << " (FT_VOX_VULKAN_LIB)\n";
			return true;
		}
		if (log)
			*log << "FT_VOX_VULKAN_LIB failed (" << explicitPath << "): " << SDL_GetError() << "\n";
	}

#if defined(__APPLE__)
	static const char *kMacCandidates[] = {
		"/opt/homebrew/lib/libvulkan.1.dylib", // Apple Silicon Homebrew
		"/opt/homebrew/lib/libvulkan.dylib",
		"/usr/local/lib/libvulkan.1.dylib", // Intel Homebrew
		"/usr/local/lib/libvulkan.dylib",
	};
	for (const char *path : kMacCandidates)
	{
		if (SDL_Vulkan_LoadLibrary(path))
		{
			if (log)
				*log << "Vulkan library: " << path << "\n";
			return true;
		}
	}
#endif

	if (SDL_Vulkan_LoadLibrary(nullptr))
		return true;
	return false;
}

inline const char *vulkanLibraryLoadHint()
{
	return "On macOS install MoltenVK + loader and set:\n"
		   "  export VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json\n"
		   "Optional: export FT_VOX_VULKAN_LIB=/opt/homebrew/lib/libvulkan.1.dylib";
}
