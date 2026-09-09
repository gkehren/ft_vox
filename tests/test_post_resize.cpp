// Regression test for issue #156: PostStack::resize() with a CHANGED swapchain
// {format, color space} destroys and recreates the fullscreen post pipelines.
// The descriptor set layouts, the descriptor pool/sets and the pipeline
// layouts are format-independent and must stay alive for the whole PostStack
// lifetime — before the fix, destroyPipelines() also destroyed the pipeline
// layouts while createPipelines() skipped their recreation (its guard keyed
// on the still-alive m_postSetLayout), so every rebuilt pipeline referenced a
// null layout (validation errors / crash at first bind).
//
// The test drives PostStack directly through the full pass chain (exposure →
// SSAO → bloom → god rays → composite → FXAA):
//   A (initial pair) → B (different supported pair) → A → extent-only resize
// renders real frames at every step into format-matched offscreen targets and
// REQUIRES the Vulkan validation layer: it runs only when Khronos validation
// is active and fails unless the layer stays silent through rendering AND the
// full teardown.
//
// Exit codes: 0 = pass, 1 = failure, 77 = environment skip (no Vulkan device,
// no second supported {format, color space} pair, or no validation layer).

#include "Renderer/PostStack.hpp"
#include "Renderer/FrameUBO.hpp"
#include "Renderer/ColorSpace.hpp"
#include "Vulkan/ImageBarrier.hpp"
#include "Vulkan/VkBuffer.hpp"
#include "Vulkan/VkImage.hpp"
#include "Vulkan/VkLoadLibrary.hpp"
#include "Vulkan/VkSwapchain.hpp"

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr uint32_t kW = 320;
constexpr uint32_t kH = 240;
constexpr uint32_t kBigW = kW * 2;
constexpr uint32_t kBigH = kH * 2;

/// Minimal stand-in for WorldRenderer's set0: FrameUBO x2 + draw-data SSBO.
/// The composite pass only reads binding 0 (frame.projection for the
/// underwater transport), but the set must be fully populated for
/// validation-clean binds against the composite pipeline layout.
struct FrameSet
{
	VkDescriptorSetLayout layout{VK_NULL_HANDLE};
	VkDescriptorPool pool{VK_NULL_HANDLE};
	VkDescriptorSet set{VK_NULL_HANDLE};
	AllocatedBuffer ubo0{};
	AllocatedBuffer ubo1{};
	AllocatedBuffer draws{};
};

FrameSet createFrameSet(VkContext &context)
{
	VkDevice device = context.getDevice();
	FrameSet fs{};

	std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
	bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
				   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
				   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
	bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
	VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	li.bindingCount = static_cast<uint32_t>(bindings.size());
	li.pBindings = bindings.data();
	if (vkCreateDescriptorSetLayout(device, &li, nullptr, &fs.layout) != VK_SUCCESS)
		throw std::runtime_error("frame set layout failed");

	std::array<VkDescriptorPoolSize, 2> ps{
		{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
	VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pi.poolSizeCount = 2;
	pi.pPoolSizes = ps.data();
	pi.maxSets = 1;
	if (vkCreateDescriptorPool(device, &pi, nullptr, &fs.pool) != VK_SUCCESS)
		throw std::runtime_error("frame set pool failed");
	VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	ai.descriptorPool = fs.pool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &fs.layout;
	if (vkAllocateDescriptorSets(device, &ai, &fs.set) != VK_SUCCESS)
		throw std::runtime_error("frame set alloc failed");

	auto makeBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage) {
		return createBuffer(context.getAllocator(), size, usage, VMA_MEMORY_USAGE_AUTO,
							VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
								VMA_ALLOCATION_CREATE_MAPPED_BIT);
	};
	fs.ubo0 = makeBuffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
	fs.ubo1 = makeBuffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
	fs.draws = makeBuffer(256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

	// Neutral frame state: identity transforms, no underwater, no fog.
	FrameUBO frame{};
	frame.view = glm::mat4(1.f);
	frame.projection = glm::mat4(1.f);
	std::memcpy(fs.ubo0.info.pMappedData, &frame, sizeof(frame));
	std::memcpy(fs.ubo1.info.pMappedData, &frame, sizeof(frame));
	std::memset(fs.draws.info.pMappedData, 0, 256);

	VkDescriptorBufferInfo b0{fs.ubo0.buffer, 0, sizeof(FrameUBO)};
	VkDescriptorBufferInfo b1{fs.ubo1.buffer, 0, sizeof(FrameUBO)};
	VkDescriptorBufferInfo b2{fs.draws.buffer, 0, VK_WHOLE_SIZE};
	const VkDescriptorType types[3] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
									   VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
									   VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
	VkDescriptorBufferInfo *infos[3] = {&b0, &b1, &b2};
	std::array<VkWriteDescriptorSet, 3> ws{};
	for (int i = 0; i < 3; ++i)
	{
		ws[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
		ws[i].dstSet = fs.set;
		ws[i].dstBinding = static_cast<uint32_t>(i);
		ws[i].descriptorCount = 1;
		ws[i].descriptorType = types[i];
		ws[i].pBufferInfo = infos[i];
	}
	vkUpdateDescriptorSets(device, 3, ws.data(), 0, nullptr);
	return fs;
}

void destroyFrameSet(VkContext &context, FrameSet &fs)
{
	if (fs.ubo0.buffer)
		destroyBuffer(context.getAllocator(), fs.ubo0);
	if (fs.ubo1.buffer)
		destroyBuffer(context.getAllocator(), fs.ubo1);
	if (fs.draws.buffer)
		destroyBuffer(context.getAllocator(), fs.draws);
	if (fs.pool)
		vkDestroyDescriptorPool(context.getDevice(), fs.pool, nullptr);
	if (fs.layout)
		vkDestroyDescriptorSetLayout(context.getDevice(), fs.layout, nullptr);
	fs = {};
}

/// Surface {format, color space} pairs the SDR post pipeline supports
/// (strict allowlist, Renderer/ColorSpace.hpp).
std::vector<VkSurfaceFormatKHR> supportedOutputPairs(VkContext &context)
{
	const SwapchainSupportDetails support =
		VkSwapchain::querySupport(context.getPhysicalDevice(), context.getSurface());
	std::vector<VkSurfaceFormatKHR> out;
	for (const VkSurfaceFormatKHR &f : support.formats)
	{
		if (colorspace::classifyOutputTransfer(f.format, f.colorSpace) !=
			colorspace::OutputTransfer::Unsupported)
			out.push_back(f);
	}
	return out;
}

/// Mean normalized RGB of a color-attachment image, via transfer readback.
/// Leaves the image in TRANSFER_SRC_OPTIMAL (it is discarded afterwards).
/// The GPU write is made visible to the CPU with vmaInvalidateAllocation
/// before any read of the mapped memory (correct for coherent and
/// non-coherent HOST memory alike).
double meanBrightness(VkContext &context, ImmediateCommands &imm,
					  AllocatedImage &img, uint32_t w, uint32_t h,
					  AllocatedBuffer &readback)
{
	imm.submitAndWait([&](VkCommandBuffer cmd) {
		vkbar::cmdTransitionColor(cmd, img.image,
								  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
								  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
								  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
								  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
								  VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy region{};
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {w, h, 1};
		vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
							   readback.buffer, 1, &region);
	});
	if (!readback.info.pMappedData)
		return -1.0;
	vmaInvalidateAllocation(context.getAllocator(), readback.allocation, 0, VK_WHOLE_SIZE);
	const uint8_t *px = static_cast<const uint8_t *>(readback.info.pMappedData);
	double sum = 0.0;
	const size_t texels = size_t(w) * h;
	for (size_t i = 0; i < texels; ++i)
		sum += (px[i * 4 + 0] + px[i * 4 + 1] + px[i * 4 + 2]) / (3.0 * 255.0);
	return sum / double(texels);
}

} // namespace

int main()
{
	// Single failure ledger: every FAIL: message increments it, so the exit
	// code always reflects what was printed (a FAIL: can never coexist with
	// an OK).
	int failures = 0;
	auto failTest = [&](const std::string &msg) {
		std::cerr << "FAIL: " << msg << '\n';
		++failures;
	};

	bool sdlUp = false, libraryUp = false, deviceUp = false;
	SDL_Window *window = nullptr;
	VkContext context;
	ImmediateCommands imm;
	PostStack post;
	FrameSet frameSet{};
	AllocatedImage imgA{}, imgB{}, imgBig{};
	AllocatedBuffer readback{};

	auto teardown = [&]() {
		if (deviceUp)
		{
			context.waitIdle();
			post.shutdown();
			for (AllocatedImage *img : {&imgA, &imgB, &imgBig})
				if (img->image)
					destroyImage(context.getAllocator(), context.getDevice(), *img);
			if (readback.buffer)
				destroyBuffer(context.getAllocator(), readback);
			destroyFrameSet(context, frameSet);
			imm.shutdown();
			deviceUp = false;
		}
		// Always destroy any partially initialized Vulkan context before
		// unloading the shared library: VkContext::shutdown() is null-guarded,
		// so this is a no-op when init() never ran and cleans up instance/
		// surface when init() threw halfway (issue #163 review).
		context.shutdown();

		if (window)
		{
			SDL_DestroyWindow(window);
			window = nullptr;
		}
		if (libraryUp)
		{
			SDL_Vulkan_UnloadLibrary();
			libraryUp = false;
		}
		if (sdlUp)
		{
			SDL_Quit();
			sdlUp = false;
		}
	};
	auto skip = [&](const std::string &why) {
		teardown();
		std::cout << "SKIP: " << why << "\n";
		return 77;
	};

	if (!SDL_Init(SDL_INIT_VIDEO))
		return skip(std::string("SDL video init failed: ") + SDL_GetError());
	sdlUp = true;
	if (!loadVulkanLibrary(&std::cout))
		return skip("no Vulkan loader available");
	libraryUp = true;
	window = SDL_CreateWindow("ft_vox post resize test", kW, kH,
							  SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
	if (!window)
		return skip(std::string("window creation failed: ") + SDL_GetError());
	try
	{
		context.init(window);
	}
	catch (const std::exception &e)
	{
		return skip(std::string("Vulkan device unavailable (") + e.what() + ")");
	}
	deviceUp = true;
	// The whole point of this test is a SILENT validation layer across the
	// destroy/recreate path — without the layer it loses its value, so skip
	// instead of reporting a hollow "pass" (FT_VOX_VALIDATION=1 requests the
	// layer, but does not guarantee it is installed).
	if (!context.isValidationEnabled())
		return skip("PostResizeFormats requires the Khronos validation layer");

	try
	{
		imm.init(context);
		std::cout << "Device: " << context.getDeviceProperties().deviceName
				  << " (validation=yes"
				  << ", fragmentStoresAndAtomics=" << context.fragmentStoresAndAtomics() << ")\n";

		// --- Pair selection: A = initial, B = a DIFFERENT supported pair ----
		const std::vector<VkSurfaceFormatKHR> supported = supportedOutputPairs(context);
		if (supported.empty())
			return skip("surface exposes no SDR-supported {format, color space} pair");
		VkSurfaceFormatKHR pairA = supported.front();
		for (const VkSurfaceFormatKHR &f : supported)
			if (colorspace::classifyOutputTransfer(f.format, f.colorSpace) ==
				colorspace::OutputTransfer::HardwareSrgb)
			{
				pairA = f;
				break;
			}
		VkSurfaceFormatKHR pairB{};
		bool haveB = false;
		for (const VkSurfaceFormatKHR &f : supported)
		{
			if (f.format == pairA.format && f.colorSpace == pairA.colorSpace)
				continue;
			if (!haveB || f.format != pairB.format)
				pairB = f; // prefer an actual format change over color-space-only
			haveB = true;
		}
		if (!haveB)
			return skip("surface exposes only one supported {format, color space} pair");
		std::cout << "Pairs: A=" << pairA.format << "/" << pairA.colorSpace
				  << " B=" << pairB.format << "/" << pairB.colorSpace << "\n";

		// --- Stand-ins: frame set + format-matched offscreen outputs -------
		frameSet = createFrameSet(context);
		auto makeOutput = [&](uint32_t w, uint32_t h, VkFormat fmt) {
			return createImage2D(context.getAllocator(), context.getDevice(), w, h, fmt,
								 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		};
		imgA = makeOutput(kW, kH, pairA.format);
		imgB = makeOutput(kW, kH, pairB.format);
		imgBig = makeOutput(kBigW, kBigH, pairA.format);
		readback = createBuffer(context.getAllocator(), size_t(kBigW) * kBigH * 4,
								VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
								VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
									VMA_ALLOCATION_CREATE_MAPPED_BIT);

		// --- PostStack on pair A, full post frames --------------------------
		post.init(context, imm, frameSet.layout, pairA.format, pairA.colorSpace, kW, kH);

		// Every recorded output state must match the requested pair; returns
		// false so callers can treat a mismatch as fatal for the sequence.
		auto requireState = [&](const char *when, VkFormat fmt, VkColorSpaceKHR cs,
								colorspace::OutputTransfer transfer) {
			if (post.swapchainFormat() == fmt && post.swapchainColorSpace() == cs &&
				post.outputTransfer() == transfer &&
				post.swapchainRequiresSrgbEncode() ==
					(transfer == colorspace::OutputTransfer::ShaderSrgb))
				return true;
			failTest(std::string(when) + ": PostStack output state mismatch");
			return false;
		};
		if (!requireState("after init", pairA.format, pairA.colorSpace,
						  colorspace::classifyOutputTransfer(pairA.format, pairA.colorSpace)))
			throw std::runtime_error("init must record the requested swapchain format/color space");

		PostProcessSettings settings{}; // bloom/SSAO/god rays/FXAA/auto-exposure on
		const glm::vec2 sunScreen{0.5f, 0.35f};
		const glm::mat4 projection =
			glm::perspective(glm::radians(70.f), float(kW) / float(kH), 0.1f, 1000.f);
		uint32_t frameSlot = 0;
		// First frame of each target batch seeds the scene targets with valid
		// content; every later frame re-enters with them in
		// SHADER_READ_ONLY_OPTIMAL, exactly as the production pass graph
		// leaves them.
		bool sceneFresh = true;
		auto recordFrame = [&](AllocatedImage &out, VkExtent2D extent) {
			imm.submitAndWait([&](VkCommandBuffer cmd) {
				const bool fresh = sceneFresh;
				sceneFresh = false;
				const VkImageLayout sceneLayout = fresh ? VK_IMAGE_LAYOUT_UNDEFINED
														: VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				const VkPipelineStageFlags srcStage = fresh
														  ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
														  : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
				const VkAccessFlags srcAccess = fresh ? VK_ACCESS_NONE
													  : VK_ACCESS_SHADER_READ_BIT;
				for (VkImage image : {post.hdrColor().image, post.godSource().image})
				{
					vkbar::cmdTransitionColor(cmd, image, sceneLayout,
											  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
											  srcAccess, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
											  srcStage, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
				}
				vkbar::cmdTransitionDepth(cmd, post.sceneDepth().image, sceneLayout,
										  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
										  srcAccess, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
										  srcStage, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
				if (fresh)
				{
					// Seed content through rendering-scope clears (loadOp =
					// CLEAR): the PostStack targets have no TRANSFER_DST
					// usage, so transfer clears would be invalid.
					const auto beginRendering = vkCmdBeginRendering ? vkCmdBeginRendering
																	: vkCmdBeginRenderingKHR;
					const auto endRendering = vkCmdEndRendering ? vkCmdEndRendering
																: vkCmdEndRenderingKHR;
					const VkExtent2D sceneExtent{post.hdrColor().width, post.hdrColor().height};
					VkClearValue gray{};
					gray.color = {{0.6f, 0.7f, 0.8f, 1.f}};
					for (VkImageView view : {post.hdrColor().view, post.godSource().view})
					{
						VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
						ca.imageView = view;
						ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
						ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
						ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
						ca.clearValue = gray;
						VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
						ri.renderArea = {{0, 0}, sceneExtent};
						ri.layerCount = 1;
						ri.colorAttachmentCount = 1;
						ri.pColorAttachments = &ca;
						beginRendering(cmd, &ri);
						endRendering(cmd);
					}
					VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
					da.imageView = post.sceneDepth().view;
					da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
					da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
					da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
					da.clearValue.depthStencil = {1.f, 0};
					VkRenderingInfo dri{VK_STRUCTURE_TYPE_RENDERING_INFO};
					dri.renderArea = {{0, 0}, sceneExtent};
					dri.layerCount = 1;
					dri.pDepthAttachment = &da;
					beginRendering(cmd, &dri);
					endRendering(cmd);
				}
				post.recordPost(cmd, out.image, out.view, extent, frameSlot % 2,
								frameSet.set, settings, sunScreen, 1.0f, 12.0f,
								projection, 1.f / 60.f);
			});
			++frameSlot;
		};

		// 1. Two frames on the initial pair A.
		recordFrame(imgA, {kW, kH});
		recordFrame(imgA, {kW, kH});
		const double meanA0 = meanBrightness(context, imm, imgA, kW, kH, readback);

		// 2. Format/color-space change — the destroy/recreate branch under
		// test (issue #156). Every pipeline must come back valid.
		post.resize(kW, kH, pairB.format, pairB.colorSpace);
		sceneFresh = true; // recreated targets start UNDEFINED
		if (!requireState("after A->B resize", pairB.format, pairB.colorSpace,
						  colorspace::classifyOutputTransfer(pairB.format, pairB.colorSpace)))
			throw std::runtime_error("resize did not switch the recorded output pair");
		recordFrame(imgB, {kW, kH});
		recordFrame(imgB, {kW, kH});
		const double meanB = meanBrightness(context, imm, imgB, kW, kH, readback);

		// 3. Back to A.
		post.resize(kW, kH, pairA.format, pairA.colorSpace);
		sceneFresh = true;
		if (!requireState("after B->A resize", pairA.format, pairA.colorSpace,
						  colorspace::classifyOutputTransfer(pairA.format, pairA.colorSpace)))
			throw std::runtime_error("resize back did not switch the recorded output pair");
		recordFrame(imgA, {kW, kH});
		const double meanA1 = meanBrightness(context, imm, imgA, kW, kH, readback);

		// 4. Extent-only resize must keep working unchanged (same format).
		post.resize(kBigW, kBigH, pairA.format, pairA.colorSpace);
		sceneFresh = true;
		if (post.hdrColor().width != kBigW || post.hdrColor().height != kBigH)
			throw std::runtime_error("extent-only resize must recreate the post targets");
		recordFrame(imgBig, {kBigW, kBigH});

		// 5. Rendered frames must carry actual content (a null-layout or
		// mismatched pipeline leaves the target cleared/garbage-free).
		for (const auto &[when, mean] : {std::pair<const char *, double>{"frame at A", meanA0},
										 {"frame at B", meanB},
										 {"frame back at A", meanA1}})
		{
			if (!(mean > 0.01))
				failTest(std::string(when) + " produced a near-black frame (mean " +
						 std::to_string(mean) + ")");
		}

		// Validation errors observed DURING this body are counted after the
		// teardown below, so destruction-time errors are covered too.
	}
	catch (const std::exception &e)
	{
		failTest(std::string("exception: ") + e.what());
	}

	// Explicit nominal teardown, THEN the validation verdict: VkContext's
	// error counter survives shutdown(), so errors raised while destroying
	// the post pipelines, the descriptors/layouts or the device itself are
	// caught here as well (issue #156 review).
	teardown();
	if (context.validationErrorCount() != 0)
		failTest("Vulkan validation reported " +
				 std::to_string(context.validationErrorCount()) + " error(s)");

	if (failures == 0)
		std::cout << "test_post_resize: OK (A->B->A format/color-space resize + extent resize, "
					 "full post frames, validation clean)\n";
	return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
