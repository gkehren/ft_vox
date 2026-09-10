#include <Renderer/MobRenderer.hpp>
#include <Renderer/TextureManager.hpp>
#include <Renderer/ColorSpace.hpp>
#include <Vulkan/VkLoadLibrary.hpp>
#include <Vulkan/VkGpuProfiler.hpp>
#include <Vulkan/ImageBarrier.hpp>
#include <SDL3/SDL.h>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <miniz.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <map>
#include <numeric>

namespace fs = std::filesystem;
// Minimal RGBA PNG encoder (same scheme as test_mob_textures) for synthetic
// resource-pack skins.
static std::vector<uint8_t> encodePng(int w, int h, const std::vector<uint8_t> &rgba)
{
    std::vector<uint8_t> out{137, 80, 78, 71, 13, 10, 26, 10};
    auto be = [](std::vector<uint8_t> &v, uint32_t n) {
        for (int s : {24, 16, 8, 0})
            v.push_back(uint8_t(n >> s));
    };
    auto chunk = [&](const char *type, const std::vector<uint8_t> &payload) {
        be(out, uint32_t(payload.size()));
        auto start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), payload.begin(), payload.end());
        be(out, uint32_t(mz_crc32(0, out.data() + start, out.size() - start)));
    };
    std::vector<uint8_t> header;
    be(header, w);
    be(header, h);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", header);
    std::vector<uint8_t> raw(size_t(h) * (w * 4 + 1), 0);
    for (int y = 0; y < h; ++y)
    {
        raw[size_t(y) * (w * 4 + 1)] = 0; // filter none
        memcpy(&raw[size_t(y) * (w * 4 + 1) + 1], &rgba[size_t(y) * w * 4], size_t(w) * 4);
    }
    mz_ulong size = mz_compressBound(raw.size());
    std::vector<uint8_t> compressed(size);
    if (mz_compress(compressed.data(), &size, raw.data(), raw.size()) != MZ_OK)
        throw std::runtime_error("png compress failed");
    compressed.resize(size);
    chunk("IDAT", compressed);
    chunk("IEND", {});
    return out;
}
static PFN_vkAllocateDescriptorSets realAllocate{};
static int allocationCalls{};
static VKAPI_ATTR VkResult VKAPI_CALL failAllocate(VkDevice d, const VkDescriptorSetAllocateInfo *i,
                                                   VkDescriptorSet *s)
{
    if (++allocationCalls == 2)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return realAllocate(d, i, s);
}
// Issue #130 submission counters: every vkCmdDraw / vkCmdBindDescriptorSets
// issued while the hooks are armed, across all four mob render passes.
struct RecordedDraw
{
    uint32_t vertexCount, instanceCount, firstVertex, firstInstance;
};
static std::vector<RecordedDraw> recordedDraws;
static PFN_vkCmdDraw realCmdDraw{};
static uint32_t cmdDrawCalls{};
static uint32_t cmdDrawMinInstances{~0u}, cmdDrawMaxInstances{};
static VKAPI_ATTR void VKAPI_CALL countingCmdDraw(VkCommandBuffer cb, uint32_t vertexCount,
                                                  uint32_t instanceCount, uint32_t firstVertex,
                                                  uint32_t firstInstance)
{
    ++cmdDrawCalls;
    cmdDrawMinInstances = std::min(cmdDrawMinInstances, instanceCount);
    cmdDrawMaxInstances = std::max(cmdDrawMaxInstances, instanceCount);
    recordedDraws.push_back({vertexCount, instanceCount, firstVertex, firstInstance});
    realCmdDraw(cb, vertexCount, instanceCount, firstVertex, firstInstance);
}
static PFN_vkCmdBindDescriptorSets realBindSets{};
static uint32_t setBindCalls{};
static VKAPI_ATTR void VKAPI_CALL
countingBindSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first,
                 uint32_t count, const VkDescriptorSet *sets, uint32_t dynamicOffsets,
                 const uint32_t *pDynamicOffsets)
{
    ++setBindCalls;
    realBindSets(cb, bp, layout, first, count, sets, dynamicOffsets, pDynamicOffsets);
}
// Steady-state heap-allocation counter for prepare()/record() (same scheme as
// test_mobs' simulation profile).
static bool countingAllocations = false;
static size_t heapAllocations = 0;
void *operator new(size_t n)
{
    if (countingAllocations)
        ++heapAllocations;
    if (auto p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](size_t n)
{
    return ::operator new(n);
}
void operator delete(void *p) noexcept
{
    std::free(p);
}
void operator delete[](void *p) noexcept
{
    std::free(p);
}
void operator delete(void *p, size_t) noexcept
{
    std::free(p);
}
void operator delete[](void *p, size_t) noexcept
{
    std::free(p);
}
// Mirror of MobRenderer's per-pass frustum cull so the tests can derive the
// expected per-pass instance counts from the FrameUBO they submit.
static bool frustumVisible(const glm::mat4 &matrix, glm::vec3 position)
{
    const glm::vec4 center(position + glm::vec3(0, 0.8f, 0), 1);
    const auto r3 = glm::row(matrix, 3);
    const std::array<glm::vec4, 6> planes{r3 + glm::row(matrix, 0), r3 - glm::row(matrix, 0),
                                          r3 + glm::row(matrix, 1), r3 - glm::row(matrix, 1),
                                          glm::row(matrix, 2),      r3 - glm::row(matrix, 2)};
    for (auto &p : planes)
        if (glm::dot(p, center) < -1.6f * glm::length(glm::vec3(p)))
            return false;
    return true;
}
static uint8_t visibilityMask(const FrameUBO &u, glm::vec3 position)
{
    const std::array<glm::mat4, 4> matrices{u.projection * u.view, u.cascadeMatrix0, u.cascadeMatrix1,
                                            u.cascadeMatrix2};
    uint8_t mask = 0;
    for (uint32_t pass = 0; pass < 4; ++pass)
        if (frustumVisible(matrices[pass], position))
            mask |= uint8_t(1 << pass);
    return mask;
}
// Expected per-pass submission for `states`: draws = populated static batches
// (one per MobPart of every species with at least one visible mob), instances
// = visible mob parts. This is the contract record() must satisfy.
struct ExpectedPasses
{
    std::array<uint32_t, 4> draws{}, instances{};
};
static ExpectedPasses expectedPasses(const entities::MobModels &baked,
                                     const std::vector<entities::MobRenderState> &states, const FrameUBO &u)
{
    std::array<std::array<uint32_t, 4>, entities::kMobSpeciesCount> mobCount{};
    for (const auto &s : states)
    {
        const uint8_t mask = visibilityMask(u, s.position);
        for (uint32_t pass = 0; pass < 4; ++pass)
            if (mask & (1 << pass))
                ++mobCount[size_t(s.species)][pass];
    }
    ExpectedPasses e;
    for (size_t sp = 0; sp < entities::kMobSpeciesCount; ++sp)
    {
        const uint32_t parts = uint32_t(baked.models[sp].parts.size());
        for (uint32_t pass = 0; pass < 4; ++pass)
            if (mobCount[sp][pass])
            {
                e.draws[pass] += parts;
                e.instances[pass] += mobCount[sp][pass] * parts;
            }
    }
    return e;
}
// Proof that the vkCmdDraw commands actually recorded for one pass are the
// expected batch stream (issue #130 review): no empty draws, no gap/overlap in
// firstInstance, instance counts summing to the expected visible part
// instances, and geometry matching the static batch table entry. Requires the
// pass's populated batches to be `batchBegin .. batchBegin + count - 1` in
// batch order (true for single-species and all-populated fixtures).
// expectedPerBatch > 0 additionally pins every batch's instance count.
static void verifyRecordedPassDraws(const MobRenderer &renderer, const RecordedDraw *draws,
                                    uint32_t count, uint32_t expectedInstances, uint32_t batchBegin,
                                    uint32_t expectedPerBatch)
{
    uint32_t running = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const RecordedDraw &d = draws[i];
        if (d.instanceCount == 0)
            throw std::runtime_error("recorded vkCmdDraw has instanceCount == 0");
        if (expectedPerBatch && d.instanceCount != expectedPerBatch)
            throw std::runtime_error("recorded vkCmdDraw instance count != populated batch size");
        if (d.firstInstance != running)
            throw std::runtime_error("recorded vkCmdDraw firstInstance gap/overlap");
        running += d.instanceCount;
        if (running > expectedInstances)
            throw std::runtime_error("recorded vkCmdDraw instances exceed the pass slice");
        const auto batch = renderer.batchInfo(batchBegin + i);
        if (d.firstVertex != batch.firstVertex || d.vertexCount != batch.vertexCount)
            throw std::runtime_error("recorded vkCmdDraw geometry does not match its static batch");
    }
    if (running != expectedInstances)
        throw std::runtime_error("recorded vkCmdDraw instance sum != expected visible parts");
}
struct Fixture
{
    VkContext context;
    ImmediateCommands imm;
    MobRenderer renderer;
    VkGpuProfiler gpu;
    AllocatedImage color{}, depth{}, shadow{};
    std::array<VkImageView, 3> shadowViews{};
    VkSampler shadowSampler{};
    VkDescriptorSetLayout frameLayout{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    AllocatedBuffer uniform{}, readback{};
    uint32_t width = 1200, height = 600;
    double lastRecordMs{}; // CPU command-recording cost of the last render()
    double lastPrepareMs{}; // CPU MobPrepare cost of the last render()
    Fixture(SDL_Window *window)
    {
        context.init(window);
        imm.init(context);
        gpu.init(context, 2);
        auto device = context.getDevice();
        auto allocator = context.getAllocator();
        shadow = createImage2DArray(allocator, device, 1024, 1024, 3, VK_FORMAT_D32_SFLOAT,
                                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, VK_IMAGE_ASPECT_DEPTH_BIT);
        for (int i = 0; i < 3; ++i)
            shadowViews[i] = createImageView2DLayer(device, shadow.image, VK_FORMAT_D32_SFLOAT,
                                                    VK_IMAGE_ASPECT_DEPTH_BIT, i);
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        // Production shadow-sampler contract (issue #137): mob.frag samples a
        // sampler2DArrayShadow — Dref ops are UB with compareEnable == FALSE.
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        si.compareEnable = VK_TRUE;
        si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        if (vkCreateSampler(device, &si, nullptr, &shadowSampler) != VK_SUCCESS)
            throw std::runtime_error("sampler");
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        if (vkCreateDescriptorSetLayout(device, &li, nullptr, &frameLayout) != VK_SUCCESS)
            throw std::runtime_error("layout");
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(device, &pi, nullptr, &pool) != VK_SUCCESS)
            throw std::runtime_error("pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &frameLayout;
        if (vkAllocateDescriptorSets(device, &ai, &set) != VK_SUCCESS)
            throw std::runtime_error("set");
        uniform = createBuffer(
            allocator, sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        VkDescriptorBufferInfo bi{uniform.buffer, 0, sizeof(FrameUBO)};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bi;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        resize(width, height);
        renderer.init(context, imm, frameLayout, shadow.view, shadowSampler, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_FORMAT_D32_SFLOAT, "");
    }
    void resize(uint32_t w, uint32_t h)
    {
        context.waitIdle();
        auto allocator = context.getAllocator();
        auto device = context.getDevice();
        if (color.image)
            destroyImage(allocator, device, color);
        if (depth.image)
            destroyImage(allocator, device, depth);
        if (readback.buffer)
            destroyBuffer(allocator, readback);
        width = w;
        height = h;
        color = createImage2D(allocator, device, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        depth = createImage2D(
            allocator, device, w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        readback = createBuffer(
            allocator, size_t(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }
    // CPU-side per-frame batching (MobRenderer::prepare) only.
    void prepare(uint32_t slot, const FrameUBO &ubo, const std::vector<entities::MobRenderState> &states)
    {
        const auto prepareStart = std::chrono::steady_clock::now();
        renderer.prepare(slot, ubo, states);
        lastPrepareMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart)
                .count();
    }
    // Records + submits the four mob passes WITHOUT re-preparing: whatever
    // prepare() last wrote for `slot` is exactly what gets drawn. Issue #130
    // review: frame-slot isolation must be observable without refreshing the
    // slot under test first.
    std::vector<uint8_t> renderPrepared(uint32_t slot, const FrameUBO &ubo, bool copy = true)
    {
        writeBuffer(context.getAllocator(), uniform, &ubo, sizeof(ubo));
        lastRecordMs = 0;
        const auto recordStart = std::chrono::steady_clock::now();
        imm.submitAndWait([&](VkCommandBuffer cmd) {
            gpu.beginRecording(cmd, slot, 0);
            auto begin = vkCmdBeginRendering ? vkCmdBeginRendering : vkCmdBeginRenderingKHR;
            auto end = vkCmdEndRendering ? vkCmdEndRendering : vkCmdEndRenderingKHR;
            vkbar::cmdTransitionDepth(cmd, shadow.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 3);
            for (int c = 0; c < 3; ++c)
            {
                VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                da.imageView = shadowViews[c];
                da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                da.clearValue.depthStencil = {1, 0};
                VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                ri.renderArea = {{0, 0}, {1024, 1024}};
                ri.layerCount = 1;
                ri.pDepthAttachment = &da;
                begin(cmd, &ri);
                VkViewport vp{0, 0, 1024, 1024, 0, 1};
                VkRect2D sc{{0, 0}, {1024, 1024}};
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &sc);
                auto pass = GpuPass(uint32_t(GpuPass::MobShadow0) + c);
                gpu.beginPass(cmd, pass);
                renderer.record(cmd, slot, set, c);
                gpu.endPass(cmd, pass);
                end(cmd);
            }
            vkbar::cmdTransitionDepth(cmd, shadow.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 3);
            cmdTransitionImageLayout(cmd, color.image, VK_IMAGE_LAYOUT_UNDEFINED,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            vkbar::cmdTransitionDepth(
                cmd, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
            VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ca.imageView = color.view;
            ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            ca.clearValue.color = {{0.12f, 0.17f, 0.23f, 1}};
            VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            da.imageView = depth.view;
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.clearValue.depthStencil = {1, 0};
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {width, height}};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &ca;
            ri.pDepthAttachment = &da;
            begin(cmd, &ri);
            VkViewport vp{0, float(height), float(width), -float(height), 0, 1};
            VkRect2D sc{{0, 0}, {width, height}};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            gpu.beginPass(cmd, GpuPass::Mobs);
            renderer.record(cmd, slot, set);
            gpu.endPass(cmd, GpuPass::Mobs);
            end(cmd);
            gpu.endRecording(cmd);
            if (copy)
            {
                cmdTransitionImageLayout(cmd, color.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {width, height, 1};
                vkCmdCopyImageToBuffer(cmd, color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       readback.buffer, 1, &region);
                VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                                     &barrier, 0, nullptr, 0, nullptr);
            }
            lastRecordMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - recordStart)
                    .count();
        });
        gpu.markSubmitted(slot);
        gpu.onSlotReady(slot);
        if (!copy)
            return {};
        vmaInvalidateAllocation(context.getAllocator(), readback.allocation, 0, VK_WHOLE_SIZE);
        auto *data = static_cast<uint8_t *>(readback.info.pMappedData);
        return {data, data + size_t(width) * height * 4};
    }
    std::vector<uint8_t> render(const std::vector<entities::MobRenderState> &states, FrameUBO ubo,
                                uint32_t slot, bool copy = true)
    {
        prepare(slot, ubo, states);
        return renderPrepared(slot, ubo, copy);
    }
    void save(const fs::path &path, const std::vector<uint8_t> &image)
    {
        std::ofstream f(path, std::ios::binary);
        f << "P6\n" << width << " " << height << "\n255\n";
        for (size_t i = 0; i < image.size(); i += 4)
            f.write(reinterpret_cast<const char *>(image.data() + i), 3);
    }
    ~Fixture()
    {
        context.waitIdle();
        renderer.shutdown();
        gpu.shutdown();
        auto d = context.getDevice();
        auto a = context.getAllocator();
        for (auto view : shadowViews)
            if (view)
                vkDestroyImageView(d, view, nullptr);
        if (pool)
            vkDestroyDescriptorPool(d, pool, nullptr);
        if (frameLayout)
            vkDestroyDescriptorSetLayout(d, frameLayout, nullptr);
        if (shadowSampler)
            vkDestroySampler(d, shadowSampler, nullptr);
        destroyBuffer(a, uniform);
        destroyBuffer(a, readback);
        destroyImage(a, d, color);
        destroyImage(a, d, depth);
        destroyImage(a, d, shadow);
        imm.shutdown();
    }
};
static FrameUBO frame(float aspect, bool crowded = false)
{
    FrameUBO u{};
    glm::vec3 eye = crowded ? glm::vec3(12, 14, -22) : glm::vec3(3.5f, 2.8f, -7.5f);
    u.view = glm::lookAt(eye, crowded ? glm::vec3(0, 0, 5) : glm::vec3(0, 0.7f, 0), glm::vec3(0, 1, 0));
    u.projection = glm::perspective(glm::radians(45.f), aspect, 0.1f, 100.f);
    u.viewPos = glm::vec4(eye, 1);
    u.lightDirection = glm::vec4(glm::normalize(glm::vec3(-3, 6, -4)), 0);
    u.lightParams = {0.5, 0.7, 0, 1}; // .z reserved (issue #161)
    u.visualParams = {1.0f, 1.0f, 0.0f, 0.0f}; // .z reserved, .w shadowDebug off (issue #161)
    u.skyParams = {0, 1, 0, 0};
    u.lightingParams = {1.0f, 1.0f, 64.0f, 0.0f};
    u.fogParams = {60, 100, 0, 0};
    u.fogColor = {0.12f, 0.17f, 0.23f, 1};
    u.cascadeMatrix0 = u.cascadeMatrix1 = u.cascadeMatrix2 =
        glm::ortho(-20.f, 20.f, -20.f, 20.f, 0.1f, 80.f) *
        glm::lookAt(glm::vec3(-15, 30, -20), glm::vec3(0), glm::vec3(0, 1, 0));
    u.cascadeSplits = {20, 40, 80, 3};
    // Non-zero receiver-bias scales (issue #137 contract): the shared CSM
    // include multiplies these by the dimensionless slope/base factors.
    u.cascadeBiasScales = {0.0005f, 0.001f, 0.002f, 1024.f};
    u.cascadeTexelWorldSizes = {0.06f, 0.12f, 0.24f, 0.f};
    return u;
}
int main(int argc, char **argv)
{
    const fs::path output = argc > 1 ? argv[1] : "mob-render-qa";
    fs::create_directories(output);
    if (!SDL_Init(SDL_INIT_VIDEO) || !loadVulkanLibrary(&std::cout))
        return 1;
    auto *window = SDL_CreateWindow("Mob Vulkan tests", 64, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window)
        return 1;
    int result = 0;
    try
    {
        Fixture f(window);
        // Issue #135 integration: the albedo images actually created on the GPU
        // must be sRGB so the hardware decodes them to linear on sample — asserts
        // the real wiring, not just the kAlbedoTextureFormat policy constant.
        if (f.renderer.textureFormat() != colorspace::kAlbedoTextureFormat)
            throw std::runtime_error("MobRenderer entity albedo textures must be created sRGB");
        // Issue #160 integration: every committed mob image must carry the
        // full mip chain on the GPU (floor(log2(64)) + 1 = 7 levels for both
        // the 64x64 and the 64x32 skins), not mip 0 only.
        for (size_t i = 0; i < 6; ++i)
            if (f.renderer.textureMipLevels(i) != 7)
                throw std::runtime_error("MobRenderer entity texture " + std::to_string(i) +
                                         " must carry a full 7-level mip chain");
        {
            TextureManager atlas;
            atlas.initialize(f.context, f.imm, "");
            if (!atlas.isValid() || atlas.getFormat() != colorspace::kAlbedoTextureFormat)
                throw std::runtime_error("TextureManager block atlas must be created sRGB");
        }
        std::vector<entities::MobRenderState> states;
        for (size_t i = 0; i < entities::kMobSpeciesCount; ++i)
            states.push_back({entities::MobSpecies(i), {(float(i) - 1.5f) * 2.0f, 0, 0}, 0, 0, 0, 0, 0});
        auto u = frame(2);
        auto image = f.render(states, u, 0);
        f.save(output / "four-species.ppm", image);
        if (f.renderer.visibleCount() != 4)
            throw std::runtime_error("four species not camera-visible");
        size_t changed = 0;
        for (size_t i = 0; i < image.size(); i += 4)
            if (image[i] != image[0] || image[i + 1] != image[1])
                ++changed;
        if (changed < 10000)
            throw std::runtime_error("mob image is empty");
        // Partial GPU allocation failure must preserve the previous visible result.
        realAllocate = vkAllocateDescriptorSets;
        vkAllocateDescriptorSets = failAllocate;
        allocationCalls = 0;
        bool failed = false;
        try
        {
            auto replacement = f.renderer.prepareTextures(f.imm, "");
            f.renderer.commitTextures(std::move(replacement));
        }
        catch (...)
        {
            failed = true;
        }
        vkAllocateDescriptorSets = realAllocate;
        if (!failed || f.render(states, u, 1) != image)
            throw std::runtime_error("failed texture reload changed live rendering");
        // Issue #160: time the full CPU mip generation + multi-level upload.
        const auto texReloadStart = std::chrono::steady_clock::now();
        f.renderer.commitTextures(f.renderer.prepareTextures(f.imm, ""));
        const double texReloadMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - texReloadStart)
                .count();
        if (f.render(states, u, 0) != image)
            throw std::runtime_error("default reload changed result");
        for (auto &s : states)
        {
            s.gait = 1;
            s.stride = 1;
            s.look = 0.25f;
            s.flap = 1;
        }
        auto animated = f.render(states, u, 1);
        f.save(output / "animated.ppm", animated);
        if (animated == image)
            throw std::runtime_error("animation did not change pixels");

        // Issue #128: local lighting variations (skylight and RGB block light)
        {
            std::vector<entities::MobRenderState> litMob = {
                {entities::MobSpecies::Cow, {0, 0, 0}, 0, 0, 0, 0, 0, 1.0f, glm::vec3(0.0f)}
            };
            std::vector<entities::MobRenderState> darkMob = {
                {entities::MobSpecies::Cow, {0, 0, 0}, 0, 0, 0, 0, 0, 0.0f, glm::vec3(0.0f)}
            };
            std::vector<entities::MobRenderState> redMob = {
                {entities::MobSpecies::Cow, {0, 0, 0}, 0, 0, 0, 0, 0, 0.0f, glm::vec3(1.0f, 0.0f, 0.0f)}
            };

            auto imgLit = f.render(litMob, u, 0);
            auto imgDark = f.render(darkMob, u, 1);
            auto imgRed = f.render(redMob, u, 0);

            uint64_t litSum = 0, darkSum = 0, redSumR = 0, redSumG = 0;
            size_t redMobPixels = 0;
            for (size_t i = 0; i < imgLit.size(); i += 4)
            {
                if (imgLit[i] != imgLit[0] || imgLit[i + 1] != imgLit[1])
                    litSum += imgLit[i] + imgLit[i + 1] + imgLit[i + 2];
                if (imgDark[i] != imgDark[0] || imgDark[i + 1] != imgDark[1])
                    darkSum += imgDark[i] + imgDark[i + 1] + imgDark[i + 2];
                if (imgRed[i] != imgRed[0] || imgRed[i + 1] != imgRed[1])
                {
                    redSumR += imgRed[i];
                    redSumG += imgRed[i + 1];
                    ++redMobPixels;
                }
            }
            if (litSum <= darkSum)
                throw std::runtime_error("sunlit mob must be brighter than cave-dark mob");
            if (redMobPixels == 0 || redSumR <= redSumG)
                throw std::runtime_error("red block-lit mob must have higher red channel than green");
        }

        // Issue #160: mobs ~65 blocks away exercise the deep mip levels of the
        // skin chain (a 64-px skin is a few pixels tall here — the old
        // nearest-only mip-0 path crawled exactly at this range). The scene
        // must still produce mob pixels and stay validation-clean.
        {
            std::vector<entities::MobRenderState> distant;
            for (size_t i = 0; i < entities::kMobSpeciesCount; ++i)
                distant.push_back({entities::MobSpecies(i), {(float(i) - 1.5f) * 1.5f, 0, 60.f}, 0, 0, 0, 0, 0});
            FrameUBO uf = frame(2);
            const glm::vec3 eye(0.f, 3.f, -6.f);
            uf.view = glm::lookAt(eye, glm::vec3(0, 1, 60), glm::vec3(0, 1, 0));
            uf.projection = glm::perspective(glm::radians(45.f), 2.f, 0.1f, 200.f);
            uf.viewPos = glm::vec4(eye, 1);
            uf.fogParams = {200.f, 400.f, 0.f, 0.f}; // keep distance fog from erasing the fixture
            auto farImage = f.render(distant, uf, 0);
            f.save(output / "far-species.ppm", farImage);
            size_t farChanged = 0;
            for (size_t i = 0; i < farImage.size(); i += 4)
                if (farImage[i] != farImage[0] || farImage[i + 1] != farImage[1])
                    ++farChanged;
            if (farChanged < 50)
                throw std::runtime_error("far-distance mobs produced no pixels");
        }

        // Issue #160 (review round 2): GPU seam/LOD regression. Every cow UV
        // face rect is painted ONE pure palette color on an aggressive
        // synthetic skin (unused texels: transparent magenta). UV-rect-aware
        // mip generation keeps every owned texel of every level pure, so any
        // impure rendered pixel can only come from GPU filtering across rect
        // borders or deep-level ownership collapse. The shipping sampler
        // (NEAREST intra-level + LINEAR mip selection) must keep face colors
        // dominant at every LOD and must beat the LinearMips reference, whose
        // intra-level LINEAR blends adjacent faces right at their shared
        // border — the failure mode this test exists to expose.
        {
            entities::MobModels baked;
            const auto rects = entities::mobTextureFaceRects(baked, 0, 64, 64);
            if (rects.size() < 20)
                throw std::runtime_error("unexpectedly few cow face rects for the seam fixture");
            std::vector<uint8_t> skin(64 * 64 * 4, 0);
            for (size_t i = 0; i < 64 * 64; ++i)
            {
                skin[i * 4] = 255; // unused: loud magenta, alpha 0
                skin[i * 4 + 2] = 255;
            }
            for (const auto &r : rects)
            {
                const size_t code = size_t(&r - rects.data());
                const uint8_t c[3] = {uint8_t(code & 1 ? 240 : 15), uint8_t(code & 2 ? 240 : 15),
                                      uint8_t(code & 4 ? 240 : 15)};
                for (uint32_t y = r.y; y < r.y + r.h; ++y)
                    for (uint32_t x = r.x; x < r.x + r.w; ++x)
                        for (int ch = 0; ch < 4; ++ch)
                            skin[(size_t(y) * 64 + x) * 4 + ch] = ch < 3 ? c[ch] : 255;
            }
            const fs::path pack = fs::temp_directory_path() / "ft-vox-mob-seam-pack";
            fs::create_directories(pack / "assets/minecraft/textures/entity/cow");
            {
                const std::vector<uint8_t> pngData = encodePng(64, 64, skin);
                std::ofstream pngOut(pack / "assets/minecraft/textures/entity/cow/cow_temperate.png",
                                     std::ios::binary);
                pngOut.write(reinterpret_cast<const char *>(pngData.data()), std::streamsize(pngData.size()));
            }

            // Flat-lighting frame: zero diffuse, ambient only, so every face
            // renders as exactly ONE flat color and any deviation is a
            // filtering artifact, not shading.
            const auto seamFrame = [&](float dist) {
                FrameUBO u = frame(float(f.width) / float(f.height));
                const glm::vec3 eye(0.f, 1.7f, -dist);
                u.view = glm::lookAt(eye, glm::vec3(0, 0.9f, 0), glm::vec3(0, 1, 0));
                u.projection = glm::perspective(glm::radians(45.f), float(f.width) / float(f.height), 0.1f, 400.f);
                u.viewPos = glm::vec4(eye, 1);
                u.lightParams = {0.9, 0.0, 16, 1.0};
                u.fogParams = {400.f, 800.f, 0.f, 0.f};
                std::vector<entities::MobRenderState> mob = {
                    {entities::MobSpecies::Cow, {0, 0, 0}, 0, 0, 0, 0, 0, 1.0f, glm::vec3(0.0f)}};
                return f.render(mob, u, 0);
            };
            const auto mobPixelIdx = [&](const std::vector<uint8_t> &px) {
                std::vector<size_t> idx;
                for (size_t p = 0; p < px.size(); p += 4)
                    if (std::abs(int(px[p]) - 31) + std::abs(int(px[p + 1]) - 43) + std::abs(int(px[p + 2]) - 59) > 24)
                        idx.push_back(p);
                return idx;
            };
            // Learn the flat face colors from the close (LOD ~0) render: under
            // NEAREST magnification every mob pixel IS one face's flat color.
            std::vector<std::array<int, 3>> flatColors;
            f.renderer.commitTextures(f.renderer.prepareTextures(f.imm, pack.string(),
                                                                 MobRenderer::SamplerPolicy::Mipmapped));
            {
                const auto closePx = seamFrame(12.f);
                const auto closeIdx = mobPixelIdx(closePx);
                std::map<std::array<int, 3>, int> counts;
                for (size_t p : closeIdx)
                    counts[{int(closePx[p]), int(closePx[p + 1]), int(closePx[p + 2])}]++;
                for (const auto &c : counts)
                    if (c.second >= 2)
                        flatColors.push_back(c.first);
            }
            if (flatColors.size() < 6)
                throw std::runtime_error("seam fixture learned too few flat face colors");

            const auto purityAt = [&](float dist) {
                const auto px = seamFrame(dist);
                const auto idx = mobPixelIdx(px);
                size_t pure = 0;
                for (size_t p : idx)
                    for (const auto &c : flatColors)
                        if (std::abs(int(px[p]) - c[0]) <= 2 && std::abs(int(px[p + 1]) - c[1]) <= 2 &&
                            std::abs(int(px[p + 2]) - c[2]) <= 2)
                        {
                            ++pure;
                            break;
                        }
                return idx.empty() ? 0.0 : double(pure) / double(idx.size());
            };
            const auto runStages = [&](MobRenderer::SamplerPolicy policy) {
                f.renderer.commitTextures(f.renderer.prepareTextures(f.imm, pack.string(), policy));
                return std::array<double, 3>{purityAt(12.f), purityAt(90.f), purityAt(180.f)};
            };
            const auto ship = runStages(MobRenderer::SamplerPolicy::Mipmapped);
            const auto linear = runStages(MobRenderer::SamplerPolicy::LinearMips);
            // Restore the default pack (shipping sampling) for the rest of the suite.
            f.renderer.commitTextures(f.renderer.prepareTextures(f.imm, ""));
            std::cout << "Mob seam LOD purity (close/LOD1/LOD2): shipping " << ship[0] << "/" << ship[1] << "/"
                      << ship[2] << ", linear-intra " << linear[0] << "/" << linear[1] << "/" << linear[2] << "\n";
            {
                std::ofstream report(output / "seam-lod.txt", std::ios::app);
                report << "Mob seam LOD purity (close/LOD1/LOD2): shipping " << ship[0] << "/" << ship[1] << "/"
                       << ship[2] << ", linear-intra " << linear[0] << "/" << linear[1] << "/" << linear[2] << "\n";
            }
            if (ship[0] < 0.95)
                throw std::runtime_error("close-range seam purity too low — face colors are not flat/pure");
            if (ship[1] < 0.70 || ship[2] < 0.60)
                throw std::runtime_error("shipping sampler leaks across UV face seams at minification");
            // The comparative proof: intra-level LINEAR must measure strictly
            // worse at minified stages — the test keeps exposing the seam
            // problem the shipping sampler avoids.
            if (linear[1] >= ship[1] || linear[2] >= ship[2])
                throw std::runtime_error("LinearMips reference should measure worse than NEAREST intra-level");
        }

        // Issue #160 (review P2): a REAL A/B of the temporal benefit. The same
        // deterministic camera pan is rendered twice per step — once with the
        // shipping mipmapped sampler, once with a NearestMip0 sampler that
        // reproduces the pre-#160 behavior — and the mean |frame-to-frame
        // luma delta| is measured over a fixed ROI covering the mobs only.
        // The mipmapped sampler must be materially more stable; the old
        // FXAA-only pan metric could never prove that.
        double abNearest = 0.0, abMipped = 0.0;
        {
            std::vector<entities::MobRenderState> ab;
            for (size_t i = 0; i < entities::kMobSpeciesCount; ++i)
                ab.push_back({entities::MobSpecies(i), {(float(i) - 1.5f) * 1.5f, 0, 60.f}, 0, 0, 0, 0, 0});
            for (size_t i = 0; i < entities::kMobSpeciesCount; ++i)
                ab.push_back({entities::MobSpecies(i), {(float(i) - 1.5f) * 1.5f, 0, 140.f}, 0, 0, 0, 0, 0});
            FrameUBO base = frame(2);
            base.projection = glm::perspective(glm::radians(45.f), 2.f, 0.1f, 400.f);
            base.fogParams = {400.f, 800.f, 0.f, 0.f}; // no distance fog on the fixture
            const glm::vec3 eye(0.f, 3.f, -10.f);
            constexpr int kSteps = 32;
            constexpr float kSweepDeg = 3.0f;

            int roi[4] = {0, 0, 0, 0}; // x0, y0, x1, y1 (exclusive)
            bool haveRoi = false;
            const auto nonBackground = [](uint8_t r, uint8_t g, uint8_t b) {
                return std::abs(int(r) - 31) + std::abs(int(g) - 43) + std::abs(int(b) - 59) > 24;
            };
            const auto uboAt = [&](int i) {
                const float a = glm::radians(kSweepDeg * (float(i) / float(kSteps - 1) - 0.5f));
                FrameUBO u = base;
                u.view = glm::lookAt(eye, eye + glm::vec3(std::sin(a), 0.f, std::cos(a)), glm::vec3(0, 1, 0));
                u.viewPos = glm::vec4(eye, 1);
                return u;
            };
            // One full pan with ONE sampler policy: mean |frame-to-frame luma
            // delta| over the fixed mob ROI.
            const auto runPan = [&](MobRenderer::SamplerPolicy policy) {
                std::vector<uint8_t> prev;
                double sum = 0.0;
                int pairs = 0;
                for (int i = 0; i < kSteps; ++i)
                {
                    auto px = f.render(ab, uboAt(i), policy == MobRenderer::SamplerPolicy::Mipmapped ? 0 : 1);
                    if (!haveRoi)
                    {
                        int x0 = int(f.width), y0 = int(f.height), x1 = -1, y1 = -1;
                        for (uint32_t y = 0; y < f.height; ++y)
                            for (uint32_t x = 0; x < f.width; ++x)
                            {
                                const size_t p = (size_t(y) * f.width + x) * 4;
                                if (nonBackground(px[p], px[p + 1], px[p + 2]))
                                {
                                    x0 = std::min(x0, int(x));
                                    y0 = std::min(y0, int(y));
                                    x1 = std::max(x1, int(x) + 1);
                                    y1 = std::max(y1, int(y) + 1);
                                }
                            }
                        if (x1 < 0)
                            throw std::runtime_error("A/B fixture rendered no mobs");
                        constexpr int kPad = 48; // > the 3 deg pan shift at any fixture distance
                        roi[0] = std::max(0, x0 - kPad);
                        roi[1] = std::max(0, y0 - kPad);
                        roi[2] = std::min(int(f.width), x1 + kPad);
                        roi[3] = std::min(int(f.height), y1 + kPad);
                        haveRoi = true;
                    }
                    else
                    {
                        // Every frame's mob pixels must stay inside the fixed
                        // ROI, or the metric would silently compare backdrop.
                        for (uint32_t y = 0; y < f.height; ++y)
                            for (uint32_t x = 0; x < f.width; ++x)
                            {
                                const size_t p = (size_t(y) * f.width + x) * 4;
                                if (nonBackground(px[p], px[p + 1], px[p + 2]) &&
                                    (int(x) < roi[0] || int(x) >= roi[2] || int(y) < roi[1] || int(y) >= roi[3]))
                                    throw std::runtime_error("A/B mobs left the fixed ROI");
                            }
                    }
                    if (!prev.empty())
                    {
                        for (int y = roi[1]; y < roi[3]; ++y)
                            for (int x = roi[0]; x < roi[2]; ++x)
                            {
                                const size_t p = (size_t(y) * f.width + x) * 4;
                                const int l0 = (prev[p] * 299 + prev[p + 1] * 587 + prev[p + 2] * 114) / 1000;
                                const int l1 = (px[p] * 299 + px[p + 1] * 587 + px[p + 2] * 114) / 1000;
                                sum += std::abs(l1 - l0);
                            }
                        ++pairs;
                    }
                    prev = std::move(px);
                }
                return sum / (double(pairs) * double((roi[2] - roi[0]) * (roi[3] - roi[1])));
            };
            abMipped = runPan(MobRenderer::SamplerPolicy::Mipmapped);
            // Swap in the pre-#160 sampling (same mip-chained images, sampler
            // clamped to mip 0 with NEAREST) and render the identical pan.
            f.renderer.commitTextures(f.renderer.prepareTextures(f.imm, "",
                                                                 MobRenderer::SamplerPolicy::NearestMip0));
            abNearest = runPan(MobRenderer::SamplerPolicy::NearestMip0);
            // Restore the shipping sampling for the rest of the suite.
            f.renderer.commitTextures(f.renderer.prepareTextures(f.imm, ""));
            if (abNearest <= 0.0)
                throw std::runtime_error("A/B nearest-mip0 delta is degenerate");
            const double improvement = 1.0 - abMipped / abNearest;
            std::cout << "Mob temporal A/B: mip0/nearest delta " << abNearest << "/255, mipmapped "
                      << abMipped << "/255, improvement " << improvement * 100.0 << "%\n";
            if (improvement < 0.10)
                throw std::runtime_error("mipmapped mob sampling is not temporally stable enough vs mip0/nearest");
        }
        { // keep the A/B evidence next to the profile report
            std::ofstream report(output / "ab-temporal.txt", std::ios::app);
            report << "Mob temporal A/B (fixed mob ROI, 32-step pan): mip0/nearest " << abNearest
                   << "/255, mipmapped " << abMipped << "/255, improvement "
                   << (abNearest > 0.0 ? 100.0 * (1.0 - abMipped / abNearest) : 0.0) << "%\n";
        }

        f.resize(800, 600);
        f.render(states, frame(800.f / 600), 0);
        // Camera culling cannot suppress shadow casters behind the eye.
        // Issue #130: every off-camera mob must feed zero instances to the
        // color pass while its cascade instances stay exact.
        const entities::MobModels bakedModels;
        auto hidden = states;
        const std::array<glm::vec3, 4> hiddenPos{glm::vec3(0, 0, -9), glm::vec3(0, 0, -11),
                                                 glm::vec3(0, 0, -13), glm::vec3(0, 0, -15)};
        for (size_t i = 0; i < hidden.size(); ++i)
            hidden[i].position = hiddenPos[i];
        const FrameUBO hiddenUbo = frame(800.f / 600);
        f.render(hidden, hiddenUbo, 1);
        if (f.renderer.visibleCount() != 0)
            throw std::runtime_error("camera culling failed");
        {
            std::array<uint8_t, 4> shadowCasters{};
            for (size_t i = 0; i < hidden.size(); ++i)
            {
                const uint8_t mask = visibilityMask(hiddenUbo, hiddenPos[i]);
                if (mask & 1)
                    throw std::runtime_error("hidden fixture mob unexpectedly camera-visible");
                shadowCasters[size_t(hidden[i].species)] |= mask;
            }
            if (!(shadowCasters[0] & 0xE) && !(shadowCasters[1] & 0xE) && !(shadowCasters[2] & 0xE) &&
                !(shadowCasters[3] & 0xE))
                throw std::runtime_error("hidden fixture has no off-camera shadow caster");
            const auto expect = expectedPasses(bakedModels, hidden, hiddenUbo);
            if (expect.draws[0] || expect.instances[0])
                throw std::runtime_error("hidden fixture must expect no color-pass instances");
            for (uint32_t pass = 0; pass < 4; ++pass)
            {
                const auto st = f.renderer.passStats(1, pass);
                if (st.draws != expect.draws[pass] || st.instances != expect.instances[pass])
                    throw std::runtime_error("off-camera mob per-pass instance accounting mismatch: pass " +
                                             std::to_string(pass) + " draws " + std::to_string(st.draws) +
                                             " vs " + std::to_string(expect.draws[pass]) + ", instances " +
                                             std::to_string(st.instances) + " vs " +
                                             std::to_string(expect.instances[pass]));
            }
        }
        f.resize(1200, 600);
        // ===== Issue #130: static-part instanced batch submission =====
        {
            // a. Batch table contract: exactly one batch per baked MobPart, in
            // species/model/part order, every range valid.
            std::array<uint32_t, entities::kMobSpeciesCount> speciesParts{};
            size_t totalParts = 0;
            for (size_t s = 0; s < entities::kMobSpeciesCount; ++s)
            {
                speciesParts[s] = uint32_t(bakedModels.models[s].parts.size());
                totalParts += speciesParts[s];
            }
            std::cout << "Mob batches: total=" << totalParts << " cow=" << speciesParts[0]
                      << " pig=" << speciesParts[1] << " sheep=" << speciesParts[2]
                      << " chicken=" << speciesParts[3] << "\n";
            if (f.renderer.batchCount() != totalParts)
                throw std::runtime_error("batch table must hold exactly one batch per MobPart");
            {
                std::array<uint32_t, entities::kMobSpeciesCount> seen{}, next{};
                for (uint32_t b = 0; b < f.renderer.batchCount(); ++b)
                {
                    const auto info = f.renderer.batchInfo(b);
                    if (info.firstVertex + info.vertexCount > bakedModels.vertices.size())
                        throw std::runtime_error("batch references vertices outside the shared buffer");
                    if (info.vertexCount == 0 || info.vertexCount % 6)
                        throw std::runtime_error("batch vertex range is not whole boxes");
                    if (info.texture >= kMobTextures.size())
                        throw std::runtime_error("batch texture index out of range");
                    const size_t s = size_t(info.species);
                    if (info.partIndex != next[s]++)
                        throw std::runtime_error("batch part indices must be contiguous per species");
                    ++seen[s];
                }
                for (size_t s = 0; s < entities::kMobSpeciesCount; ++s)
                    if (seen[s] != speciesParts[s])
                        throw std::runtime_error("batch table lost or duplicated a species range");
            }

            // b. 48-sheep worst case: <= 18 draws/pass, <= 72 total, exact
            // per-pass instance accounting, verified against real submission
            // via vkCmdDraw hooks.
            std::vector<entities::MobRenderState> sheep;
            for (int i = 0; i < 48; ++i)
                sheep.push_back({entities::MobSpecies::Sheep, {(i % 8 - 3.5f) * 2.2f, 0, (i / 8) * 2.2f},
                                 0, 1, 0, 0, 1});
            const FrameUBO sheepUbo = frame(2, true);
            for (const auto &s : sheep)
                if (visibilityMask(sheepUbo, s.position) != 0xF)
                    throw std::runtime_error("sheep fixture must be camera + all-cascade visible");
            const auto sheepExpect = expectedPasses(bakedModels, sheep, sheepUbo);
            cmdDrawCalls = setBindCalls = 0;
            cmdDrawMinInstances = ~0u;
            cmdDrawMaxInstances = 0;
            recordedDraws.clear();
            recordedDraws.reserve(256); // pre-reserved: keep allocation tests clean
            realCmdDraw = vkCmdDraw;
            realBindSets = vkCmdBindDescriptorSets;
            vkCmdDraw = countingCmdDraw;
            vkCmdBindDescriptorSets = countingBindSets;
            const auto sheepImage = f.render(sheep, sheepUbo, 0);
            vkCmdDraw = realCmdDraw;
            vkCmdBindDescriptorSets = realBindSets;
            uint32_t sheepDraws = 0;
            std::array<uint32_t, 4> sheepPassDraws{}, sheepPassInst{};
            for (uint32_t pass = 0; pass < 4; ++pass)
            {
                const auto st = f.renderer.passStats(0, pass);
                if (st.draws != sheepExpect.draws[pass] || st.instances != sheepExpect.instances[pass])
                    throw std::runtime_error("sheep fixture per-pass batch accounting mismatch");
                sheepDraws += st.draws;
                sheepPassDraws[pass] = st.draws;
                sheepPassInst[pass] = st.instances;
            }
            if (sheepDraws > 4 * speciesParts[size_t(entities::MobSpecies::Sheep)])
                throw std::runtime_error("48 sheep exceed 18 draws/pass x 4 passes");
            // Recorded-command proof (issue #130 review): the emitted draws are
            // the 18 sheep batches in table order, each carrying all 48
            // instances, with gap-free firstInstance ranges that end exactly at
            // the pass total.
            if (cmdDrawCalls != sheepDraws || recordedDraws.size() != sheepDraws)
                throw std::runtime_error("recorded vkCmdDraw count differs from reported batches");
            uint32_t sheepBatchBegin = ~0u;
            for (uint32_t b = 0; b < f.renderer.batchCount(); ++b)
                if (f.renderer.batchInfo(b).species == entities::MobSpecies::Sheep)
                {
                    sheepBatchBegin = b;
                    break;
                }
            {
                size_t cursor = 0;
                for (uint32_t pass : {1u, 2u, 3u, 0u}) // command order: cascades then color
                {
                    verifyRecordedPassDraws(f.renderer, recordedDraws.data() + cursor,
                                            sheepPassDraws[pass], sheepPassInst[pass], sheepBatchBegin,
                                            uint32_t(sheep.size()));
                    cursor += sheepPassDraws[pass];
                }
                if (cursor != recordedDraws.size())
                    throw std::runtime_error("unexpected extra recorded draws");
            }
            if (cmdDrawCalls != sheepDraws)
                throw std::runtime_error("recorded vkCmdDraw count differs from reported batches");
            if (cmdDrawMinInstances < 1 || cmdDrawMaxInstances > entities::kMaxMobCount)
                throw std::runtime_error("a batch draw was empty or over-populated");
            // Sheep sample 3 textures (base/wool/undercoat): at most one frame
            // set + one texture run per texture per pass.
            if (setBindCalls > 4 * 4)
                throw std::runtime_error("descriptor set binds did not collapse to texture runs");
            size_t sheepPixels = 0;
            for (size_t i = 0; i < sheepImage.size(); i += 4)
                if (sheepImage[i] != sheepImage[0] || sheepImage[i + 1] != sheepImage[1])
                    ++sheepPixels;
            if (sheepPixels < 10000)
                throw std::runtime_error("48-sheep fixture rendered no mobs");

            // c. Mixed pass visibility: one camera+all-cascade mob, one
            // camera-only mob (outside the cascade ortho boxes), one
            // shadow-only mob. A dedicated camera looking toward +x keeps the
            // camera-only mob inside the frustum but laterally past the
            // cascade boxes centered on the origin.
            std::vector<entities::MobRenderState> mixedVis;
            const std::array<glm::vec3, 3> visPos{glm::vec3(16, 1, 0), glm::vec3(30, 1, 0),
                                                  glm::vec3(0, 0, -10)};
            for (const auto &p : visPos)
                mixedVis.push_back({entities::MobSpecies::Sheep, p, 0, 1, 0, 0, 1});
            FrameUBO visUbo = frame(2, true);
            const glm::vec3 visEye(0.f, 2.f, -7.f);
            visUbo.view = glm::lookAt(visEye, glm::vec3(30, 1, 0), glm::vec3(0, 1, 0));
            visUbo.projection = glm::perspective(glm::radians(45.f), 2.f, 0.1f, 400.f);
            visUbo.viewPos = glm::vec4(visEye, 1);
            const std::array<uint8_t, 3> masks{visibilityMask(visUbo, visPos[0]),
                                               visibilityMask(visUbo, visPos[1]),
                                               visibilityMask(visUbo, visPos[2])};
            if (masks[0] != 0xF)
                throw std::runtime_error("origin mob must be camera + all-cascade visible");
            if (!(masks[1] & 1) || (masks[1] & 0xE))
                throw std::runtime_error("side mob must be camera-only");
            if ((masks[2] & 1) || !(masks[2] & 0xE))
                throw std::runtime_error("behind mob must be shadow-only");
            f.render(mixedVis, visUbo, 0);
            const auto visExpect = expectedPasses(bakedModels, mixedVis, visUbo);
            for (uint32_t pass = 0; pass < 4; ++pass)
            {
                const auto st = f.renderer.passStats(0, pass);
                if (st.draws != visExpect.draws[pass] || st.instances != visExpect.instances[pass])
                    throw std::runtime_error("mixed-visibility per-pass instance accounting mismatch");
            }
            if (f.renderer.visibleCount() != 2)
                throw std::runtime_error("visibleCount must count camera-visible mobs only");

            // d. Capacity policy: populations past kMaxMobCount are
            // contractually invalid and fail deterministically; the renderer
            // stays usable afterwards.
            {
                std::vector<entities::MobRenderState> tooMany(entities::kMaxMobCount + 1,
                                                              entities::MobRenderState{
                                                                  entities::MobSpecies::Cow, {0, 0, 0}});
                bool threw = false;
                try
                {
                    f.renderer.prepare(0, sheepUbo, tooMany);
                }
                catch (const std::exception &)
                {
                    threw = true;
                }
                if (!threw)
                    throw std::runtime_error("over-capacity population must fail deterministically");
                f.render(sheep, sheepUbo, 1);
                if (f.renderer.visibleCount() != 48)
                    throw std::runtime_error("renderer unusable after a rejected prepare");
            }

            // e. One-mob sparse population, frame-slot isolation without
            // re-preparing, and the empty population (no draws, no stale
            // batches).
            {
                const std::vector<entities::MobRenderState> oneCow{
                    entities::MobRenderState{entities::MobSpecies::Cow, {0, 0, 0}, 0, 1, 0, 0, 1}};
                const uint32_t cowParts = uint32_t(bakedModels.models[size_t(entities::MobSpecies::Cow)].parts.size());
                f.render(oneCow, sheepUbo, 1);
                for (uint32_t pass = 0; pass < 4; ++pass)
                {
                    const auto st = f.renderer.passStats(1, pass);
                    if (st.draws != cowParts || st.instances != cowParts)
                        throw std::runtime_error("single-cow per-pass accounting mismatch");
                }
                // Frame-slot isolation (issue #130 review): prepare slot 0,
                // then prepare the OTHER slot empty and record slot 0 with
                // renderPrepared() — no re-preparation — so a slot-1 prepare
                // touching slot 0 draws/pass bases/stats/instances is caught.
                f.renderer.prepare(0, sheepUbo, sheep);
                const auto isolatedFull = f.renderPrepared(0, sheepUbo);
                if (isolatedFull != sheepImage)
                    throw std::runtime_error("prepare + renderPrepared diverges from render()");
                f.renderer.prepare(1, sheepUbo, {});
                for (uint32_t pass = 0; pass < 4; ++pass)
                {
                    const auto st = f.renderer.passStats(0, pass);
                    if (st.draws != sheepExpect.draws[pass] || st.instances != sheepExpect.instances[pass])
                        throw std::runtime_error("prepare on frame slot 1 corrupted slot 0 stats");
                }
                const auto isolatedAgain = f.renderPrepared(0, sheepUbo);
                if (isolatedAgain != isolatedFull)
                    throw std::runtime_error("slot 1 prepare corrupted slot 0 draws or instance buffer");
                for (uint32_t pass = 0; pass < 4; ++pass)
                {
                    const auto st = f.renderer.passStats(0, pass);
                    if (st.draws != sheepExpect.draws[pass] || st.instances != sheepExpect.instances[pass])
                        throw std::runtime_error("slot 0 stats drifted after slot 1 prepare");
                }
                // Reverse direction: slot 0 empty, slot 1 full — slot 0 must
                // stay empty even while slot 1 is prepared and recorded.
                f.renderer.prepare(0, sheepUbo, {});
                f.renderer.prepare(1, sheepUbo, sheep);
                f.renderPrepared(1, sheepUbo);
                for (uint32_t pass = 0; pass < 4; ++pass)
                {
                    const auto st = f.renderer.passStats(0, pass);
                    if (st.draws || st.instances)
                        throw std::runtime_error("slot 1 prepare/record disturbed empty slot 0");
                }
                cmdDrawCalls = setBindCalls = 0;
                cmdDrawMinInstances = ~0u;
                cmdDrawMaxInstances = 0;
                recordedDraws.clear();
                recordedDraws.reserve(8);
                realCmdDraw = vkCmdDraw;
                realBindSets = vkCmdBindDescriptorSets;
                vkCmdDraw = countingCmdDraw;
                vkCmdBindDescriptorSets = countingBindSets;
                const auto emptyImage = f.renderPrepared(0, sheepUbo); // record slot 0 as-is
                vkCmdDraw = realCmdDraw;
                vkCmdBindDescriptorSets = realBindSets;
                if (cmdDrawCalls || !recordedDraws.empty())
                    throw std::runtime_error("empty population emitted draws");
                for (uint32_t pass = 0; pass < 4; ++pass)
                {
                    const auto st = f.renderer.passStats(0, pass);
                    if (st.draws || st.instances)
                        throw std::runtime_error("stale batches survived an empty prepare");
                }
                for (size_t i = 0; i < emptyImage.size(); i += 4)
                    if (emptyImage[i] != emptyImage[0] || emptyImage[i + 1] != emptyImage[1])
                        throw std::runtime_error("empty population rendered pixels");
            }

            // f. Steady-state prepare must be heap-allocation-free.
            {
                countingAllocations = true;
                heapAllocations = 0;
                for (int i = 0; i < 64; ++i)
                    f.renderer.prepare(uint32_t(i) % 2, sheepUbo, sheep);
                const size_t steady = heapAllocations;
                countingAllocations = false;
                if (steady)
                    throw std::runtime_error("MobRenderer::prepare allocates in steady state");
            }
        }
        states.clear();
        for (int i = 0; i < 48; ++i)
            states.push_back(
                {entities::MobSpecies(i % int(entities::kMobSpeciesCount)),
                 {(i % 8 - 3.5f) * 2.2f, 0, (i / 8) * 2.2f}, 0, 1, 0, 0, 1});
        std::vector<double> gpuTimes, cpuTimes, recordTimes, prepareTimes;
        size_t serial = 0;
        for (int i = 0; i < 160; ++i)
        {
            auto start = std::chrono::steady_clock::now();
            auto pixels = f.render(states, frame(2, true), i % 2, i == 159);
            double cpu =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const auto &sample = f.gpu.latest();
            if (i >= 40)
            {
                cpuTimes.push_back(cpu);
                recordTimes.push_back(f.lastRecordMs);
                prepareTimes.push_back(f.lastPrepareMs);
                if (sample.serial != serial && sample.present[size_t(GpuPass::Mobs)])
                {
                    double sum = sample.ms[size_t(GpuPass::Mobs)];
                    for (int c = 0; c < 3; ++c)
                        sum += sample.ms[size_t(GpuPass::MobShadow0) + c];
                    gpuTimes.push_back(sum);
                }
            }
            serial = sample.serial;
            if (i == 159)
                f.save(output / "48-mobs.ppm", pixels);
        }
        if (f.renderer.visibleCount() != 48)
            throw std::runtime_error("48-mob fixture not fully visible");
        // Issue #130: mixed-fixture submission accounting (last prepare ran on
        // slot 1 with the full 48-mob population).
        std::array<uint32_t, 4> mixedDraws{}, mixedInst{};
        {
            const auto mixedExpect = expectedPasses(bakedModels, states, frame(2, true));
            uint32_t totalDraws = 0;
            for (uint32_t pass = 0; pass < 4; ++pass)
            {
                const auto st = f.renderer.passStats(1, pass);
                if (st.draws != mixedExpect.draws[pass] || st.instances != mixedExpect.instances[pass])
                    throw std::runtime_error("mixed 48-mob per-pass accounting mismatch");
                totalDraws += st.draws;
                mixedDraws[pass] = st.draws;
                mixedInst[pass] = st.instances;
            }
            if (totalDraws > 4 * f.renderer.batchCount())
                throw std::runtime_error("mixed fixture exceeds one draw per static batch per pass");
            // Recorded-command proof (issue #130 review): with every batch
            // populated, draws map 1:1 onto batchInfo(0..41), each carrying
            // exactly 12 instances (48 mobs / 4 species), instance ranges
            // continuous and ending exactly at the pass total.
            if (mixedDraws[0] != f.renderer.batchCount() || mixedInst[0] != mixedDraws[0] * 12)
                throw std::runtime_error("mixed fixture is not fully populated; recorded proof n/a");
            cmdDrawCalls = setBindCalls = 0;
            cmdDrawMinInstances = ~0u;
            cmdDrawMaxInstances = 0;
            recordedDraws.clear();
            recordedDraws.reserve(256);
            realCmdDraw = vkCmdDraw;
            realBindSets = vkCmdBindDescriptorSets;
            vkCmdDraw = countingCmdDraw;
            vkCmdBindDescriptorSets = countingBindSets;
            f.render(states, frame(2, true), 0);
            vkCmdDraw = realCmdDraw;
            vkCmdBindDescriptorSets = realBindSets;
            {
                size_t cursor = 0;
                for (uint32_t pass : {1u, 2u, 3u, 0u}) // command order: cascades then color
                {
                    const auto st = f.renderer.passStats(0, pass);
                    verifyRecordedPassDraws(f.renderer, recordedDraws.data() + cursor, st.draws,
                                            st.instances, 0, 12);
                    cursor += st.draws;
                }
                if (cursor != recordedDraws.size() || cmdDrawCalls != cursor)
                    throw std::runtime_error("unexpected extra recorded draws in mixed fixture");
            }
        }
        // Issue #130 instrumentation: the all-sheep worst articulation case.
        std::array<uint32_t, 4> sheepBenchDraws{};
        std::vector<double> sheepGpuTimes, sheepRecordTimes, sheepPrepareTimes;
        {
            std::vector<entities::MobRenderState> sheep;
            for (int i = 0; i < 48; ++i)
                sheep.push_back({entities::MobSpecies::Sheep, {(i % 8 - 3.5f) * 2.2f, 0, (i / 8) * 2.2f},
                                 0, 1, 0, 0, 1});
            size_t sheepSerial = 0;
            for (int i = 0; i < 80; ++i)
            {
                auto pixels = f.render(sheep, frame(2, true), i % 2, i == 79);
                const auto &sample = f.gpu.latest();
                if (i >= 20)
                {
                    sheepRecordTimes.push_back(f.lastRecordMs);
                    sheepPrepareTimes.push_back(f.lastPrepareMs);
                    if (sample.serial != sheepSerial && sample.present[size_t(GpuPass::Mobs)])
                    {
                        double sum = sample.ms[size_t(GpuPass::Mobs)];
                        for (int c = 0; c < 3; ++c)
                            sum += sample.ms[size_t(GpuPass::MobShadow0) + c];
                        sheepGpuTimes.push_back(sum);
                    }
                }
                sheepSerial = sample.serial;
                if (i == 79)
                    f.save(output / "48-sheep.ppm", pixels);
            }
            for (uint32_t pass = 0; pass < 4; ++pass)
                sheepBenchDraws[pass] = f.renderer.passStats(1, pass).draws;
        }
        std::ofstream report(output / "gpu-profile.txt");
        const auto statsLine = [&](const char *label, const std::array<uint32_t, 4> &draws,
                                   const std::array<uint32_t, 4> &inst) {
            report << label << " draws/pass=" << draws[0] << "/" << draws[1] << "/" << draws[2] << "/"
                   << draws[3] << " instances/pass=" << inst[0] << "/" << inst[1] << "/" << inst[2]
                   << "/" << inst[3] << " (camera/shadow0/shadow1/shadow2)\n";
        };
        statsLine("48 mobs", mixedDraws, mixedInst);
        {
            std::array<uint32_t, 4> sheepInst{};
            for (uint32_t pass = 0; pass < 4; ++pass)
                sheepInst[pass] = f.renderer.passStats(1, pass).instances;
            statsLine("48 sheep", sheepBenchDraws, sheepInst);
        }
        const auto p95 = [](std::vector<double> v) {
            std::sort(v.begin(), v.end());
            return v.empty() ? 0.0 : v[v.size() * 95 / 100];
        };
        const auto mean = [](const std::vector<double> &v) {
            return v.empty() ? 0.0 : std::accumulate(v.begin(), v.end(), 0.0) / v.size();
        };
        report << "48 mobs MobPrepare CPU mean_ms=" << mean(prepareTimes) << " p95_ms=" << p95(prepareTimes)
               << "\n";
        report << "48 sheep MobPrepare CPU mean_ms=" << mean(sheepPrepareTimes)
               << " p95_ms=" << p95(sheepPrepareTimes) << "\n";
        report << "48 sheep command recording CPU mean_ms=" << mean(sheepRecordTimes)
               << " p95_ms=" << p95(sheepRecordTimes) << "\n";
        if (!sheepGpuTimes.empty())
            report << "48 sheep color + 3 shadows GPU mean_ms=" << mean(sheepGpuTimes)
                   << " p95_ms=" << p95(sheepGpuTimes) << " samples=" << sheepGpuTimes.size() << "\n";
        report << "Device: " << f.context.getDeviceProperties().deviceName << "\n";
        // Issue #160 memory/cost evidence: mip-0 vs full-chain payload and
        // the whole CPU-generate + multi-level upload reload cost.
        report << "Mob textures: mip0 texel bytes=" << f.renderer.textureMip0Bytes()
               << " full-chain texel bytes=" << f.renderer.textureTexelBytes()
               << " reload ms=" << texReloadMs << "\n";
        if (gpuTimes.empty())
            report << "GPU timestamps unavailable\n";
        else
        {
            std::sort(gpuTimes.begin(), gpuTimes.end());
            report << "48 mobs color + 3 shadows GPU mean_ms="
                   << std::accumulate(gpuTimes.begin(), gpuTimes.end(), 0.0) / gpuTimes.size()
                   << " p95_ms=" << gpuTimes[gpuTimes.size() * 95 / 100] << " samples=" << gpuTimes.size()
                   << "\n";
        }
        std::sort(recordTimes.begin(), recordTimes.end());
        report << "48 mobs command recording CPU mean_ms="
               << std::accumulate(recordTimes.begin(), recordTimes.end(), 0.0) / recordTimes.size()
               << " p95_ms=" << recordTimes[recordTimes.size() * 95 / 100] << "\n";
        report << "Host submit+GPU wait mean_ms="
               << std::accumulate(cpuTimes.begin(), cpuTimes.end(), 0.0) / cpuTimes.size()
               << " (not CPU simulation time)\n";
        report << "Validation enabled=" << f.context.isValidationEnabled()
               << " errors=" << f.context.validationErrorCount() << "\n";
        if (f.context.validationErrorCount())
            throw std::runtime_error("Vulkan validation errors");
        std::cout << "PASS: render, animation, reload rollback, resize and 48 mobs\n";
    }
    catch (const std::exception &e)
    {
        if (realAllocate)
            vkAllocateDescriptorSets = realAllocate;
        std::cerr << e.what() << "\n";
        result = 1;
    }
    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return result;
}
