#include "MobRenderer.hpp"
#include "ColorSpace.hpp"
#include "TextureMips.hpp"
#include <Vulkan/GraphicsPipelineBuilder.hpp>
#include <Vulkan/VkShader.hpp>
#include <Vulkan/VkUpload.hpp>
#include <Engine/Profiler.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <utils.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>

MobRenderer::Textures::~Textures()
{
    if (!context)
        return;
    if (pool)
        vkDestroyDescriptorPool(context->getDevice(), pool, nullptr);
    if (sampler)
        vkDestroySampler(context->getDevice(), sampler, nullptr);
    for (auto &i : images)
        if (i.image)
            destroyImage(context->getAllocator(), context->getDevice(), i);
}
namespace
{
// Per-texture UV face rectangles of the baked box models, in texel units
// (issue #160 review P1): a mob skin is an atlas of independent box faces, so
// mip generation must never blend across their boundaries. Every face's 4 UV
// corners are integer "model pixel" coordinates (MobModel divides them by
// 64x32 when baking), and all supported skin shapes map model pixels to texels
// with the single scale imgW/64 (the sampler's uvScale = {1, w/(2h)} folding
// proves texel = modelPixel * imgW/64 on both axes).
std::vector<texture_mips::MipRect> collectTextureRects(const entities::MobModels &models, uint32_t texture,
                                                       uint32_t imgW, uint32_t imgH)
{
    std::vector<texture_mips::MipRect> rects;
    const uint32_t scale = std::max(imgW / 64u, 1u);
    for (const auto &species : models.models)
    {
        for (const auto &part : species.parts)
        {
            if (part.texture != texture)
                continue;
            // 6 faces x 6 vertices, baked contiguously per part.
            for (uint32_t face = 0; face < 6; ++face)
            {
                const size_t base = part.firstVertex + face * 6;
                float u0 = 1.0f, v0 = 1.0f, u1 = 0.0f, v1 = 0.0f;
                for (size_t v = base; v < base + 6 && v < models.vertices.size(); ++v)
                {
                    const glm::vec2 &uv = models.vertices[v].uv;
                    u0 = std::min(u0, uv.x);
                    v0 = std::min(v0, uv.y);
                    u1 = std::max(u1, uv.x);
                    v1 = std::max(v1, uv.y);
                }
                // uv corners are exact multiples of 1/64, 1/32 (powers of two).
                const uint32_t rx = uint32_t(std::lround(u0 * 64.0f)) * scale;
                const uint32_t ry = uint32_t(std::lround(v0 * 32.0f)) * scale;
                const uint32_t rw = (uint32_t(std::lround(u1 * 64.0f)) - uint32_t(std::lround(u0 * 64.0f))) * scale;
                const uint32_t rh = (uint32_t(std::lround(v1 * 32.0f)) - uint32_t(std::lround(v0 * 32.0f))) * scale;
                if (rx + rw > imgW || ry + rh > imgH || rw == 0 || rh == 0)
                    continue; // defensive: never emit a rect outside the image
                rects.push_back({rx, ry, rw, rh});
            }
        }
    }
    // Distinct faces of different parts never overlap in these layouts, but
    // different parts can share identical rects (e.g. the two cow horns).
    std::sort(rects.begin(), rects.end(), [](const texture_mips::MipRect &a, const texture_mips::MipRect &b) {
        return a.y != b.y ? a.y < b.y : a.x != b.x ? a.x < b.x : a.h != b.h ? a.h < b.h : a.w < b.w;
    });
    rects.erase(std::unique(rects.begin(), rects.end(), [](const texture_mips::MipRect &a, const texture_mips::MipRect &b) {
                    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
                }),
                rects.end());
    return rects;
}
} // namespace

std::unique_ptr<MobRenderer::Textures> MobRenderer::prepareTextures(ImmediateCommands &imm,
                                                                    const std::string &pack, SamplerPolicy policy)
{
    const auto cpu = loadMobTextures(pack, std::string(RES_PATH) + "default-resource-pack.zip");
    auto result = std::make_unique<Textures>();
    result->context = m_context;
    result->report = cpu.report;
    VkDevice device = m_context->getDevice();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    // Pixel-art contract (issue #160): NEAREST magnification keeps close-range
    // texels crisp; LINEAR minification over the full CPU-generated mip chain
    // stabilizes distant skins. Anisotropy stays off: mostly upright
    // box-model surfaces see no measurable gain to justify the sampler cost.
    // NearestMip0 reproduces the pre-#160 sampling for the A/B regression test.
    si.magFilter = VK_FILTER_NEAREST;
    if (policy == SamplerPolicy::NearestMip0)
    {
        si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.maxLod = 0.0f; // clamp to mip 0 only
    }
    else
    {
        si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.maxLod = VK_LOD_CLAMP_NONE;
    }
    si.minLod = 0.0f;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &si, nullptr, &result->sampler) != VK_SUCCESS)
        throw std::runtime_error("Mob sampler failed");
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 6;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(device, &pi, nullptr, &result->pool) != VK_SUCCESS)
        throw std::runtime_error("Mob descriptor pool failed");
    for (size_t i = 0; i < 6; ++i)
    {
        const auto &pixels = cpu.images[i];
        auto &img = result->images[i];
        const uint32_t width = uint32_t(pixels.width), height = uint32_t(pixels.height);
        // Full CPU-generated mip chain (issue #160): the same linear-light,
        // alpha-coverage-preserving filter as the terrain atlas, run per UV
        // face rectangle so neighboring box faces never blend into each other
        // and each face keeps its own alpha coverage (review P1).
        const uint32_t mips = texture_mips::mipLevelCount(width, height);
        img = createImage2D(m_context->getAllocator(), device, width, height,
                            colorspace::kAlbedoTextureFormat,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, mips);
        std::vector<uint8_t> chain(texture_mips::chainBytes(width, height));
        const std::vector<texture_mips::MipRect> rects = collectTextureRects(m_models, uint32_t(i), width, height);
        texture_mips::generateLayerChainRects(width, height, pixels.rgba.data(), rects.data(), rects.size(),
                                              chain.data());
        // Every level is resident before the descriptor set is written, and a
        // failure here unwinds the whole staging Textures bundle — the live
        // set committed by commitTextures() is never touched.
        uploadRgba8Image2DMipChain(m_context->getAllocator(), imm, img, chain.data(), VkDeviceSize(chain.size()));
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = result->pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &m_textureLayout;
        if (vkAllocateDescriptorSets(device, &ai, &result->sets[i]) != VK_SUCCESS)
            throw std::runtime_error("Mob descriptor allocation failed");
        std::array<VkDescriptorImageInfo, 2> images{
            {{result->sampler, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
             {m_shadowSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}}};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (uint32_t j = 0; j < 2; ++j)
        {
            writes[j] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[j].dstSet = result->sets[i];
            writes[j].dstBinding = j;
            writes[j].descriptorCount = 1;
            writes[j].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[j].pImageInfo = &images[j];
        }
        vkUpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);
    }
    return result;
}

size_t MobRenderer::textureTexelBytes() const
{
    size_t bytes = 0;
    if (!m_textures)
        return bytes;
    for (const auto &img : m_textures->images)
    {
        if (!img.image)
            continue;
        for (uint32_t level = 0; level < img.mipLevels; ++level)
            bytes += size_t(std::max(img.width >> level, 1u)) * std::max(img.height >> level, 1u) * 4;
    }
    return bytes;
}

size_t MobRenderer::textureMip0Bytes() const
{
    size_t bytes = 0;
    if (!m_textures)
        return bytes;
    for (const auto &img : m_textures->images)
        if (img.image)
            bytes += size_t(img.width) * img.height * 4;
    return bytes;
}

void MobRenderer::refreshShadowBinding(VkImageView shadowView, VkSampler shadowSampler)
{
    if (!m_textures || shadowView == VK_NULL_HANDLE || shadowSampler == VK_NULL_HANDLE)
        return;
    m_shadowView = shadowView;
    m_shadowSampler = shadowSampler;
    const auto device = m_context->getDevice();
    for (size_t i = 0; i < m_textures->sets.size(); ++i)
    {
        VkDescriptorImageInfo shadowInfo{m_shadowSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = m_textures->sets[i];
        write.dstBinding = 1;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &shadowInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }
}

void MobRenderer::init(VkContext &context, ImmediateCommands &imm, VkDescriptorSetLayout frameLayout,
                       VkImageView shadow, VkSampler shadowSampler, VkFormat color, VkFormat depth,
                       const std::string &pack)
{
    m_context = &context;
    m_frameLayout = frameLayout;
    m_shadowView = shadow;
    m_shadowSampler = shadowSampler;
    const auto device = context.getDevice();
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{
        {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &m_textureLayout) != VK_SUCCESS)
        throw std::runtime_error("Mob texture layout failed");
    std::array<VkDescriptorSetLayout, 2> layouts{frameLayout, m_textureLayout};
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2;
    pl.pSetLayouts = layouts.data();
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(device, &pl, nullptr, &m_layout) != VK_SUCCESS)
        throw std::runtime_error("Mob pipeline layout failed");
    m_textures = prepareTextures(imm, pack);
    auto bytes = m_models.vertices.size() * sizeof(entities::MobVertex);
    m_vertices = createBuffer(context.getAllocator(), bytes,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    uploadBuffer(context.getAllocator(), imm, m_vertices, m_models.vertices.data(), bytes);
    for (size_t i = 0; i < 2; ++i)
    {
        m_instances[i] = createBuffer(context.getAllocator(), kMaxParts * sizeof(Instance),
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                          VMA_ALLOCATION_CREATE_MAPPED_BIT);
        m_draws[i].reserve(kMaxParts);
    }
    std::array<VkVertexInputBindingDescription, 2> vb{
        {{0, sizeof(entities::MobVertex), VK_VERTEX_INPUT_RATE_VERTEX},
         {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE}}};
    std::array<VkVertexInputAttributeDescription, 9> va{
        {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(entities::MobVertex, position)},
         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(entities::MobVertex, normal)},
         {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(entities::MobVertex, uv)}}};
    for (uint32_t i = 0; i < 4; ++i)
        va[3 + i] = {3 + i, 1, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
    va[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 64};
    va[8] = {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 80};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vb.data();
    vi.vertexAttributeDescriptionCount = 9;
    vi.pVertexAttributeDescriptions = va.data();
    auto vert = loadShaderModule(device, resolveSpvPath("mob.vert.spv"));
    try
    {
        for (int shadowPass = 0; shadowPass < 2; ++shadowPass)
        {
            auto frag =
                loadShaderModule(device, resolveSpvPath(shadowPass ? "mob_shadow.frag.spv" : "mob.frag.spv"));
            try
            {
                VkPipelineShaderStageCreateInfo stages[2] = {
                    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
                    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                     VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr}};
                VkPipelineRasterizationStateCreateInfo rs{
                    VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                rs.polygonMode = VK_POLYGON_MODE_FILL;
                rs.cullMode = VK_CULL_MODE_NONE;
                rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                rs.lineWidth = 1;
                rs.depthBiasEnable = shadowPass;
                rs.depthBiasConstantFactor = 1.75f;
                rs.depthBiasSlopeFactor = 2.5f;
                VkPipelineDepthStencilStateCreateInfo ds{
                    VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
                ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
                ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                VkPipelineColorBlendAttachmentState blend{};
                blend.colorWriteMask = 0xf;
                GraphicsPipelineBuilder builder;
                builder.setLayout(m_layout)
                    .setShaderStages(stages, 2)
                    .setVertexInput(&vi)
                    .setRaster(rs)
                    .setDepth(ds);
                if (shadowPass)
                    builder.setNoColorAttachments().setRenderingFormats(nullptr, 0, depth);
                else
                    builder.setColorBlendAttachments(&blend, 1).setRenderingFormats(&color, 1, depth);
                (shadowPass ? m_shadow : m_color) = builder.build(device);
            }
            catch (...)
            {
                destroyShaderModule(device, frag);
                throw;
            }
            destroyShaderModule(device, frag);
        }
    }
    catch (...)
    {
        destroyShaderModule(device, vert);
        throw;
    }
    destroyShaderModule(device, vert);
}
void MobRenderer::shutdown()
{
    if (!m_context)
        return;
    auto device = m_context->getDevice();
    m_context->waitIdle();
    m_textures.reset();
    if (m_color)
        vkDestroyPipeline(device, m_color, nullptr);
    if (m_shadow)
        vkDestroyPipeline(device, m_shadow, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(device, m_layout, nullptr);
    if (m_textureLayout)
        vkDestroyDescriptorSetLayout(device, m_textureLayout, nullptr);
    if (m_vertices.buffer)
        destroyBuffer(m_context->getAllocator(), m_vertices);
    for (auto &b : m_instances)
        if (b.buffer)
            destroyBuffer(m_context->getAllocator(), b);
    m_color = m_shadow = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_textureLayout = VK_NULL_HANDLE;
    m_context = nullptr;
}
namespace
{
bool visible(const glm::mat4 &matrix, glm::vec3 position)
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
} // namespace
void MobRenderer::prepare(uint32_t frame, const FrameUBO &ubo,
                          const std::vector<entities::MobRenderState> &mobs)
{
    PROFILE_SCOPE("MobPrepare");
    m_visible = 0;
    auto &draws = m_draws[frame];
    draws.clear();
    auto &matrices = m_matrices[frame];
    matrices = {ubo.projection * ubo.view, ubo.cascadeMatrix0, ubo.cascadeMatrix1, ubo.cascadeMatrix2};
    auto *instances = static_cast<Instance *>(m_instances[frame].info.pMappedData);
    for (auto &mob : mobs)
    {
        uint8_t mask = 0;
        for (int i = 0; i < 4; ++i)
            if (visible(matrices[i], mob.position))
                mask |= uint8_t(1 << i);
        if (mask & 1)
            ++m_visible;
        if (!mask)
            continue;
        for (auto &part : m_models.models[size_t(mob.species)].parts)
        {
            if (draws.size() == kMaxParts)
                throw std::runtime_error("Mob part capacity exceeded");
            uint32_t index = uint32_t(draws.size());
            const auto &image = m_textures->images[part.texture];
            const float sky = (mob.localSkylight < 0.0f) ? 1.0f : std::clamp(mob.localSkylight, 0.0f, 1.0f);
            instances[index] = {entities::mobPartTransform(mob, part),
                                {1, float(image.width) / (2 * image.height), 0, 0},
                                {sky, mob.localBlockRgb.r, mob.localBlockRgb.g, mob.localBlockRgb.b}};
            draws.push_back({part.firstVertex, part.vertexCount, index, part.texture, mask});
        }
    }
    if (!draws.empty())
        vmaFlushAllocation(m_context->getAllocator(), m_instances[frame].allocation, 0,
                           draws.size() * sizeof(Instance));
}
void MobRenderer::record(VkCommandBuffer cmd, uint32_t frame, VkDescriptorSet frameSet, int cascade)
{
    if (m_draws[frame].empty())
        return;
    PROFILE_SCOPE("MobDraw");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cascade < 0 ? m_color : m_shadow);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &frameSet, 0, nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4),
                       &m_matrices[frame][cascade + 1]);
    const std::array<VkBuffer, 2> buffers{m_vertices.buffer, m_instances[frame].buffer};
    const VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets);
    uint32_t texture = ~0u;
    for (auto &draw : m_draws[frame])
    {
        if (!(draw.visibility & (1 << (cascade + 1))))
            continue;
        if (texture != draw.texture)
        {
            texture = draw.texture;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1,
                                    &m_textures->sets[texture], 0, nullptr);
        }
        vkCmdDraw(cmd, draw.count, 1, draw.first, draw.instance);
    }
}
