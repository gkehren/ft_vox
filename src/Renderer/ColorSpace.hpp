#pragma once

#include <volk.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

namespace colorspace
{

/// Standard IEC 61966-2-1 sRGB decode: non-linear sRGB [0, 1] to linear light [0, 1].
inline float srgbToLinear(float c)
{
	c = std::clamp(c, 0.0f, 1.0f);
	if (c <= 0.04045f)
		return c / 12.92f;
	return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline glm::vec3 srgbToLinear(const glm::vec3 &rgb)
{
	return glm::vec3(srgbToLinear(rgb.r), srgbToLinear(rgb.g), srgbToLinear(rgb.b));
}

/// Standard IEC 61966-2-1 sRGB encode: linear light [0, 1] to non-linear sRGB [0, 1].
inline float linearToSrgb(float c)
{
	c = std::clamp(c, 0.0f, 1.0f);
	if (c <= 0.0031308f)
		return c * 12.92f;
	return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline glm::vec3 linearToSrgb(const glm::vec3 &rgb)
{
	return glm::vec3(linearToSrgb(rgb.r), linearToSrgb(rgb.g), linearToSrgb(rgb.b));
}

/// Fast perceptual luminance approximation for display-linear values (e.g. for FXAA edge detection).
inline float linearToPerceptualLuminance(const glm::vec3 &linearRgb)
{
	// Gamma 2.0 / sqrt approximation gives smooth perceptual contrast for edge detection
	const glm::vec3 perceptual = glm::sqrt(glm::max(linearRgb, glm::vec3(0.0f)));
	return glm::dot(perceptual, glm::vec3(0.299f, 0.587f, 0.114f));
}

/// Check if a Vulkan format is an sRGB format.
inline bool isSrgbFormat(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_R8_SRGB:
	case VK_FORMAT_R8G8_SRGB:
	case VK_FORMAT_R8G8B8_SRGB:
	case VK_FORMAT_B8G8R8_SRGB:
	case VK_FORMAT_R8G8B8A8_SRGB:
	case VK_FORMAT_B8G8R8A8_SRGB:
	case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
	case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
	case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
	case VK_FORMAT_BC2_SRGB_BLOCK:
	case VK_FORMAT_BC3_SRGB_BLOCK:
	case VK_FORMAT_BC7_SRGB_BLOCK:
		return true;
	default:
		return false;
	}
}

/// Format policy for albedo color textures (blocks and mobs).
/// Must be an sRGB format so Vulkan hardware performs sRGB-to-linear decode on sample.
inline constexpr VkFormat kAlbedoTextureFormat = VK_FORMAT_R8G8B8A8_SRGB;

inline bool isAlbedoColorFormat(VkFormat format)
{
	return isSrgbFormat(format);
}

/// Format policy for non-color data textures (depth, AO, masks, normals, HDR targets).
/// Must remain linear / unorm / float, NEVER sRGB.
inline bool isNonColorDataFormat(VkFormat format)
{
	return !isSrgbFormat(format);
}

/// Swapchain transfer policy:
/// Returns true if the composite fragment shader must explicitly encode linear -> sRGB.
/// When the swapchain image format is sRGB, writing to the color attachment automatically
/// performs the linear -> sRGB hardware conversion. The composite shader must NOT apply
/// a second output transfer (doing so results in double-gamma).
/// When the swapchain image format is UNORM (fallback), the hardware does not convert,
/// so the composite shader MUST perform the explicit sRGB encode.
inline bool swapchainRequiresShaderOutputTransfer(VkFormat swapchainFormat)
{
	return !isSrgbFormat(swapchainFormat);
}

/// Creative midtone gamma grade:
/// Redefined from display-transfer operator to an artistic midtone grading operator.
/// A value of 1.0f represents neutral linear output (no artistic adjustment).
inline constexpr float kNeutralGamma = 1.0f;

inline float applyArtisticGamma(float linearDisplayVal, float gamma)
{
	const float g = std::max(gamma, 0.001f);
	if (std::abs(g - 1.0f) < 1e-4f)
		return linearDisplayVal;
	return std::pow(std::max(linearDisplayVal, 0.0f), 1.0f / g);
}

inline glm::vec3 applyArtisticGamma(const glm::vec3 &linearDisplayRgb, float gamma)
{
	const float g = std::max(gamma, 0.001f);
	if (std::abs(g - 1.0f) < 1e-4f)
		return linearDisplayRgb;
	return glm::pow(glm::max(linearDisplayRgb, glm::vec3(0.0f)), glm::vec3(1.0f / g));
}

/// Full display pipeline simulation for validation / unit tests:
/// toneMappedLinear -> [creative gamma] -> [shader transfer if needed] -> [framebuffer transfer if needed]
inline glm::vec3 simulateDisplayOutput(const glm::vec3 &toneMappedLinear,
									  float gamma,
									  VkFormat swapchainFormat)
{
	// Step 1: Creative grading (neutral when gamma == 1.0)
	glm::vec3 graded = applyArtisticGamma(toneMappedLinear, gamma);

	// Step 2: Shader output
	bool shaderTransfer = swapchainRequiresShaderOutputTransfer(swapchainFormat);
	glm::vec3 shaderOut = shaderTransfer ? linearToSrgb(graded) : graded;

	// Step 3: Framebuffer attachment write
	glm::vec3 displayOut = isSrgbFormat(swapchainFormat) ? linearToSrgb(shaderOut) : shaderOut;

	return displayOut;
}

} // namespace colorspace
