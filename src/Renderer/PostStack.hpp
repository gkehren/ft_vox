#pragma once

#include "Vulkan/VkContext.hpp"
#include "Vulkan/VkSwapchain.hpp"
#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/VkImage.hpp"
#include "Vulkan/VkCommands.hpp"
#include "Vulkan/VkGpuProfiler.hpp"
#include "Engine/EngineDefs.hpp"
#include "Renderer/AutoExposure.hpp"
#include "Renderer/ColorSpace.hpp"
#include "Renderer/PostDefaults.hpp"

#include <glm/glm.hpp>
#include <volk.h>
#include <cstdint>

/// HDR targets + SSAO (half-res AO+normals → full-res bilateral upsample) /
/// bloom / god rays / composite.
/// Sky is owned by SkyPass; this stack only owns fullscreen post + scene HDR/depth.
class PostStack
{
public:
	PostStack() = default;
	~PostStack();

	void init(VkContext &context, ImmediateCommands &imm, VkDescriptorSetLayout frameSetLayout,
			  VkFormat swapchainFormat, VkColorSpaceKHR swapchainColorSpace,
			  uint32_t width, uint32_t height);
	void shutdown();
	void resize(uint32_t width, uint32_t height, VkFormat swapchainFormat,
				VkColorSpaceKHR swapchainColorSpace);

	AllocatedImage &hdrColor() { return m_hdr; }
	AllocatedImage &godSource() { return m_godSource; }
	AllocatedImage &sceneDepth() { return m_sceneDepth; }
	VkFormat depthFormat() const { return m_depthFormat; }
	VkFormat hdrFormat() const { return m_hdrFormat; }
	VkFormat swapchainFormat() const { return m_swapchainFormat; }
	VkColorSpaceKHR swapchainColorSpace() const { return m_swapchainColorSpace; }
	colorspace::OutputTransfer outputTransfer() const { return m_outputTransfer; }
	bool swapchainRequiresSrgbEncode() const { return m_swapchainRequiresSrgbEncode; }

	/// Fullscreen post: exposure metering → SSAO → AO upsample → bloom →
	/// depth-aware god rays → composite. frameDt drives the auto-exposure
	/// adaptation; profiler (optional) times the Exposure sub-pass.
	void recordPost(VkCommandBuffer cmd,
					VkImage swapchainImage,
					VkImageView swapchainView,
					VkExtent2D extent,
					uint32_t frameIndex,
					VkDescriptorSet frameSet0,
					const PostProcessSettings &settings,
					const glm::vec2 &sunScreen,
					float sunVisibility,
					float time,
					const glm::mat4 &projection,
					float frameDt = 1.f / 60.f,
					VkGpuProfiler *profiler = nullptr);

	/// Debug readout: CPU copy of the current frame slot's exposure snapshot.
	/// The slot's fence was already waited before recording, so this never
	/// synchronizes; values are those of the previous use of the slot.
	const autoexposure::ExposureGpuState &exposureReadout() const { return m_exposureReadout; }
	/// 1x1 R32F adapted-exposure target (last recorded frame), for tooling
	/// readback. Layout is SHADER_READ_ONLY_OPTIMAL between frames.
	AllocatedImage &exposureTarget() { return m_lum1; }

	/// Tooling only (synthetic-meter tests): runs the metering + adaptation
	/// chain on the CURRENT HDR content. The caller owns the HDR image — it
	/// must be in SHADER_READ_ONLY_OPTIMAL and already carry the desired
	/// synthetic pattern (cleared/copied by the caller) — and submits the
	/// command buffer synchronously so the CPU readout can observe the state
	/// right after. Uses dt = 0: the temporal state is untouched, only
	/// metered/target/clamp values are refreshed. No-op when
	/// fragmentStoresAndAtomics is unsupported.
	void recordExposureProbe(VkCommandBuffer cmd, uint32_t frameIndex,
							 const PostProcessSettings &settings);

private:
	void createTargets(uint32_t w, uint32_t h);
	void destroyTargets();
	void createDefaultImages(ImmediateCommands &imm);
	void destroyDefaultImages();
	void createPipelines(VkFormat swapchainFormat, VkColorSpaceKHR swapchainColorSpace);
	void destroyPipelines();
	void createFullscreenQuad(ImmediateCommands &imm);
	void createSamplers();
	void createExposureBuffers();
	void destroyExposureBuffers();
	void writeExposureDescriptors();
	/// HDR → 64² → 16² → 4² log-luminance reduction + adaptation into the
	/// shared history buffer (auto mode only; updates the seed tracking).
	void recordExposure(VkCommandBuffer cmd, uint32_t frameIndex,
						const PostProcessSettings &settings, float frameDt);
	bool autoExposureActive(const PostProcessSettings &settings) const;
	/// Fullscreen quad draw used by every post pass (dynamic rendering).
	void fsDraw(VkCommandBuffer cmd, VkPipeline pipe, VkPipelineLayout layout,
				VkDescriptorSet set, VkImageView outView, VkExtent2D outExt,
				const void *pc, uint32_t pcSize);
	void writeEffectDescriptors();
	void writeCompositeDescriptors(const PostCompositeSources &src,
								   uint32_t frameIndex);

	VkContext *m_context{nullptr};
	VkDescriptorSetLayout m_frameSetLayout{VK_NULL_HANDLE};

	VkFormat m_hdrFormat{VK_FORMAT_R16G16B16A16_SFLOAT};
	VkFormat m_depthFormat{VK_FORMAT_D32_SFLOAT};
	uint32_t m_width{0}, m_height{0};

	AllocatedImage m_hdr{};
	AllocatedImage m_godSource{};
	AllocatedImage m_sceneDepth{};
	AllocatedImage m_bloom[2]{};
	AllocatedImage m_godRays{};
	AllocatedImage m_ssao{};	  ///< Half-res RGBA8: r = raw AO, gb = encoded view normal.
	AllocatedImage m_ssaoUp{};	 ///< Full-res R8: bilateral-upsampled final AO.

	/// Auto-exposure metering chain (issue #140): 64x64 / 16x16 / 4x4 R32F
	/// log-luminance reduction + 1x1 adapted-exposure debug target.
	AllocatedImage m_lum[3]{};
	AllocatedImage m_lum1{};
	static constexpr uint32_t kFramesInFlight = 2;
	/// ONE shared adaptation history: the temporal state is a single logical
	/// value, not a per-frame-in-flight one (see recordExposure for the
	/// ordering argument). Host-visible: GPU-written by exposure_adapt.frag.
	AllocatedBuffer m_exposureHistory{};
	/// Per-frame-in-flight CPU-visible snapshots of the history, written by
	/// the adapt pass and read by the debug UI after the slot fence wait.
	AllocatedBuffer m_exposureSnapshot[kFramesInFlight]{};
	autoexposure::ExposureGpuState m_exposureReadout{};
	bool m_lastAutoEnabled{false}; ///< auto-mode edge detection (seed on rising edge)
	bool m_forceSeed{true};		   ///< seed on first frame / after resource creation

	/// Always-valid 1×1 fallbacks for disabled effects (SHADER_READ_ONLY).
	AllocatedImage m_defaultBlack{};	// HDR black — bloom / god rays off
	AllocatedImage m_defaultWhiteR8{}; // R8 white — SSAO off (ao=1)

	VkSampler m_linearSampler{VK_NULL_HANDLE};
	VkSampler m_nearestSampler{VK_NULL_HANDLE};

	AllocatedBuffer m_quadVBO{};

	VkDescriptorSetLayout m_postSetLayout{VK_NULL_HANDLE};
	VkDescriptorSetLayout m_godSetLayout{VK_NULL_HANDLE};
	VkDescriptorSetLayout m_compositeSetLayout{VK_NULL_HANDLE};
	VkDescriptorSetLayout m_ssaoUpLayout{VK_NULL_HANDLE}; ///< ssaoUpsample: b0 half-res AO+normals (linear), b1 full-res depth (nearest).
	VkDescriptorSetLayout m_exposureSetLayout{VK_NULL_HANDLE}; ///< exposure_adapt: b0 4x4 log-lum, b1 history SSBO, b2 snapshot SSBO.
	VkDescriptorPool m_postPool{VK_NULL_HANDLE};
	VkDescriptorSet m_setExtract{VK_NULL_HANDLE};
	VkDescriptorSet m_setBlur[2]{};
	VkDescriptorSet m_setGodRays{VK_NULL_HANDLE};
	VkDescriptorSet m_setSsao{VK_NULL_HANDLE};
	VkDescriptorSet m_setSsaoUp{VK_NULL_HANDLE};
	/// Luminance reduction source sets (bind m_lum[0] / m_lum[1]).
	VkDescriptorSet m_setLumSrc[2]{};
	VkDescriptorSet m_setComposite[kFramesInFlight]{};
	/// Adaptation sets: m_lum[2] sampler + shared history SSBO + per-slot
	/// snapshot SSBO.
	VkDescriptorSet m_exposureSets[kFramesInFlight]{};

	VkPipelineLayout m_postLayout1{VK_NULL_HANDLE};
	VkPipelineLayout m_godLayout{VK_NULL_HANDLE};
	VkPipelineLayout m_ssaoLayout{VK_NULL_HANDLE};
	VkPipelineLayout m_ssaoUpPipeLayout{VK_NULL_HANDLE};
	VkPipelineLayout m_compositeLayout{VK_NULL_HANDLE};
	VkPipelineLayout m_exposureLayout{VK_NULL_HANDLE};

	VkPipeline m_extractPipe{VK_NULL_HANDLE};
	VkPipeline m_blurPipe{VK_NULL_HANDLE};
	VkPipeline m_godRaysPipe{VK_NULL_HANDLE};
	VkPipeline m_ssaoPipe{VK_NULL_HANDLE};
	VkPipeline m_ssaoUpPipe{VK_NULL_HANDLE};
	VkPipeline m_compositePipe{VK_NULL_HANDLE};
	VkPipeline m_downsamplePipe{VK_NULL_HANDLE};
	VkPipeline m_adaptPipe{VK_NULL_HANDLE};

	VkFormat m_swapchainFormat{VK_FORMAT_UNDEFINED};
	VkColorSpaceKHR m_swapchainColorSpace{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
	colorspace::OutputTransfer m_outputTransfer{colorspace::OutputTransfer::HardwareSrgb};
	bool m_swapchainRequiresSrgbEncode{false};
	PostCompositeSources m_lastCompositeSrc[kFramesInFlight] = {
		{true, true, true}, {true, true, true}}; // force first write per frame
};
