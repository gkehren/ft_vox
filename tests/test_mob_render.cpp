#include <Renderer/MobRenderer.hpp>
#include <Renderer/TextureManager.hpp>
#include <Renderer/ColorSpace.hpp>
#include <Vulkan/VkLoadLibrary.hpp>
#include <Vulkan/VkGpuProfiler.hpp>
#include <Vulkan/ImageBarrier.hpp>
#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>
#include <numeric>

namespace fs = std::filesystem;
static PFN_vkAllocateDescriptorSets realAllocate{};
static int allocationCalls{};
static VKAPI_ATTR VkResult VKAPI_CALL failAllocate(VkDevice d, const VkDescriptorSetAllocateInfo *i,
                                                   VkDescriptorSet *s)
{
    if (++allocationCalls == 2)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    return realAllocate(d, i, s);
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
    std::vector<uint8_t> render(const std::vector<entities::MobRenderState> &states, FrameUBO ubo,
                                uint32_t slot, bool copy = true)
    {
        renderer.prepare(slot, ubo, states);
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
    u.lightParams = {0.5, 0.7, 16, 1};
    u.visualParams = {1.0f, 1.0f, 1.0f, 0.0f};
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
        f.renderer.commitTextures(f.renderer.prepareTextures(f.imm, ""));
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

        f.resize(800, 600);
        f.render(states, frame(800.f / 600), 0);
        // Camera culling cannot suppress shadow casters behind the eye.
        auto hidden = states;
        for (auto &s : hidden)
            s.position = {0, 0, -15};
        f.render(hidden, frame(800.f / 600), 1);
        if (f.renderer.visibleCount() != 0)
            throw std::runtime_error("camera culling failed");
        f.resize(1200, 600);
        states.clear();
        for (int i = 0; i < 48; ++i)
            states.push_back(
                {entities::MobSpecies(i % int(entities::kMobSpeciesCount)),
                 {(i % 8 - 3.5f) * 2.2f, 0, (i / 8) * 2.2f}, 0, 1, 0, 0, 1});
        std::vector<double> gpuTimes, cpuTimes, recordTimes;
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
        std::ofstream report(output / "gpu-profile.txt");
        report << "Device: " << f.context.getDeviceProperties().deviceName << "\n";
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
