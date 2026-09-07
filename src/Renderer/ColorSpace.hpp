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
/// Rec. 601 weights on gamma-2.0-encoded values, mirroring composite.frag (FXAA convention);
/// physical luminance of *linear* values must use kRec709Luma instead.
inline float linearToPerceptualLuminance(const glm::vec3 &linearRgb)
{
	// Gamma 2.0 / sqrt approximation gives smooth perceptual contrast for edge detection
	const glm::vec3 perceptual = glm::sqrt(glm::max(linearRgb, glm::vec3(0.0f)));
	return glm::dot(perceptual, glm::vec3(0.299f, 0.587f, 0.114f));
}

/// Rec. 709 linear-light luminance weights — the correct weights for any
/// luminance computed on linear-light RGB (terrain.frag saturation, night grade, …).
inline constexpr glm::vec3 kRec709Luma{0.2126f, 0.7152f, 0.0722f};

/// True for every sRGB-encoding format in the core VkFormat enum:
/// 8-bit packs, BC1/2/3/7, ETC2, ASTC and PVRTC sRGB block variants.
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
	case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
	case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
	case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
	case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
	case VK_FORMAT_ASTC_5x4_SRGB_BLOCK:
	case VK_FORMAT_ASTC_5x5_SRGB_BLOCK:
	case VK_FORMAT_ASTC_6x5_SRGB_BLOCK:
	case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
	case VK_FORMAT_ASTC_8x5_SRGB_BLOCK:
	case VK_FORMAT_ASTC_8x6_SRGB_BLOCK:
	case VK_FORMAT_ASTC_8x8_SRGB_BLOCK:
	case VK_FORMAT_ASTC_10x5_SRGB_BLOCK:
	case VK_FORMAT_ASTC_10x6_SRGB_BLOCK:
	case VK_FORMAT_ASTC_10x8_SRGB_BLOCK:
	case VK_FORMAT_ASTC_10x10_SRGB_BLOCK:
	case VK_FORMAT_ASTC_12x10_SRGB_BLOCK:
	case VK_FORMAT_ASTC_12x12_SRGB_BLOCK:
	case VK_FORMAT_PVRTC1_2BPP_SRGB_BLOCK_IMG:
	case VK_FORMAT_PVRTC1_4BPP_SRGB_BLOCK_IMG:
	case VK_FORMAT_PVRTC2_2BPP_SRGB_BLOCK_IMG:
	case VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG:
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

/// True when the format stores values as-is (no transfer-function conversion on
/// sample/write): UNORM/SNORM/float/depth encodings. Naming is about the *encoding*,
/// not texture semantics — an R16G16B16A16_SFLOAT HDR color buffer is linear-encoded
/// color data, not "non-color data".
inline bool isLinearEncodingFormat(VkFormat format)
{
	return !isSrgbFormat(format);
}

/// How the final display transfer is performed for a given swapchain
/// {format, colorSpace} pair. The transfer depends on BOTH: the presentation
/// engine interprets pixel values through the color space, while only an sRGB
/// *image format* makes the color-attachment write convert linear→sRGB.
enum class OutputTransfer
{
	/// sRGB image + SRGB_NONLINEAR presentation: the composite shader writes
	/// display-linear values, the attachment write performs the hardware encode.
	HardwareSrgb,
	/// UNORM image + SRGB_NONLINEAR presentation: the hardware does not convert,
	/// the composite shader must encode linear→sRGB itself.
	ShaderSrgb,
	/// Any other color space (HDR10_ST2084, DISPLAY_P3, extended/linear sRGB, …):
	/// out of scope for the SDR pipeline — refuse instead of guessing a transfer.
	Unsupported,
};

inline OutputTransfer classifyOutputTransfer(VkFormat format, VkColorSpaceKHR colorSpace)
{
	if (colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		return OutputTransfer::Unsupported;
	if (isSrgbFormat(format))
		return OutputTransfer::HardwareSrgb;
	return OutputTransfer::ShaderSrgb;
}

/// True when the composite shader must apply the explicit linear→sRGB encode
/// (push constant p4.z in composite.frag).
inline bool outputTransferRequiresShaderEncode(OutputTransfer transfer)
{
	return transfer == OutputTransfer::ShaderSrgb;
}

/// Creative midtone gamma grade:
/// Redefined from display-transfer operator to an artistic midtone grading operator.
/// A value of 1.0f represents neutral linear output (no artistic adjustment).
/// Must match composite.frag: the grade is skipped when |gamma - 1| <= 0.001.
inline constexpr float kNeutralGamma = 1.0f;
inline constexpr float kGammaNeutralEpsilon = 1e-3f;

inline float applyArtisticGamma(float linearDisplayVal, float gamma)
{
	const float g = std::max(gamma, 0.001f);
	if (std::abs(g - 1.0f) <= kGammaNeutralEpsilon)
		return linearDisplayVal;
	return std::pow(std::max(linearDisplayVal, 0.0f), 1.0f / g);
}

inline glm::vec3 applyArtisticGamma(const glm::vec3 &linearDisplayRgb, float gamma)
{
	const float g = std::max(gamma, 0.001f);
	if (std::abs(g - 1.0f) <= kGammaNeutralEpsilon)
		return linearDisplayRgb;
	return glm::pow(glm::max(linearDisplayRgb, glm::vec3(0.0f)), glm::vec3(1.0f / g));
}

/// Full display pipeline simulation for validation / unit tests:
/// toneMappedLinear -> [creative gamma] -> [shader transfer if needed] -> [framebuffer transfer if needed].
/// Both supported SDR paths converge on exactly one linear→sRGB encode.
inline glm::vec3 simulateDisplayOutput(const glm::vec3 &toneMappedLinear,
									   float gamma,
									   OutputTransfer transfer)
{
	// Step 1: Creative grading (neutral when gamma == 1.0)
	const glm::vec3 graded = applyArtisticGamma(toneMappedLinear, gamma);

	// Step 2: Shader output transfer (UNORM fallback only)
	const glm::vec3 shaderOut =
		outputTransferRequiresShaderEncode(transfer) ? linearToSrgb(graded) : graded;

	// Step 3: Framebuffer attachment write (sRGB image formats only)
	const glm::vec3 displayOut =
		transfer == OutputTransfer::HardwareSrgb ? linearToSrgb(shaderOut) : shaderOut;

	return displayOut;
}

} // namespace colorspace
