// ItemEntityRenderer — derived dropped-item presentation (issue #229, Epic
// #223 Phase F). See ItemEntityRenderer.hpp for the boundary contract. The
// buffer/pipeline/culling discipline mirrors MobRenderer (simplified: one
// shared cube, one pass, no shadow casting).

#include <Renderer/ItemEntityRenderer.hpp>

#include <Items/ItemEntitySystem.hpp>
#include <Vulkan/GraphicsPipelineBuilder.hpp>
#include <Vulkan/VkShader.hpp>
#include <Vulkan/VkUpload.hpp>
#include <Engine/Profiler.hpp>

#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

static_assert(ItemEntityRenderer::kMaxItems == items::ItemEntitySettings::capacity,
              "item renderer capacity must match the sim population cap");

namespace
{
// Unit cube centered at the origin (scaled to the item body size by the
// instance model matrix). Per-face normals + full-tile UVs; V = 0 is the
// texture TOP so side faces read upright under the atlas upload convention.
constexpr std::array<ItemEntityRenderer::Vertex, 24> kCubeVertices = [] {
    std::array<ItemEntityRenderer::Vertex, 24> v{};
    const glm::vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const glm::vec3 p[6][4] = {
        {{0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}},
        {{-0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, 0.5f}},
        {{-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},
        {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}},
        {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},
        {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}},
    };
    const glm::vec2 uv[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (int f = 0; f < 6; ++f)
        for (int i = 0; i < 4; ++i)
            v[size_t(f * 4 + i)] = {p[f][i], n[f], uv[i]};
    return v;
}();

constexpr std::array<uint16_t, 36> kCubeIndices = [] {
    std::array<uint16_t, 36> idx{};
    for (uint16_t f = 0; f < 6; ++f)
    {
        const uint16_t b = uint16_t(f * 4);
        const size_t o = size_t(f * 6);
        idx[o + 0] = b;
        idx[o + 1] = uint16_t(b + 1);
        idx[o + 2] = uint16_t(b + 2);
        idx[o + 3] = b;
        idx[o + 4] = uint16_t(b + 2);
        idx[o + 5] = uint16_t(b + 3);
    }
    return idx;
}();

// Frustum sphere test (the MobRenderer plane-extraction pattern): six
// Gribb-Hartmann planes from the view-projection, conservative radius.
bool visible(const glm::mat4 &matrix, glm::vec3 center, float radius)
{
    const glm::vec4 c(center, 1.0f);
    const auto r3 = glm::row(matrix, 3);
    const std::array<glm::vec4, 6> planes{r3 + glm::row(matrix, 0), r3 - glm::row(matrix, 0),
                                          r3 + glm::row(matrix, 1), r3 - glm::row(matrix, 1),
                                          glm::row(matrix, 2),      r3 - glm::row(matrix, 2)};
    for (const auto &p : planes)
        if (glm::dot(p, c) < -radius * glm::length(glm::vec3(p)))
            return false;
    return true;
}
} // namespace

void ItemEntityRenderer::init(VkContext &context, ImmediateCommands &imm,
                              VkDescriptorSetLayout frameLayout, VkDescriptorSetLayout atlasLayout,
                              VkFormat color, VkFormat depth)
{
    m_context = &context;
    const auto device = context.getDevice();

    const std::array<VkDescriptorSetLayout, 2> layouts{frameLayout, atlasLayout};
    VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2;
    pl.pSetLayouts = layouts.data();
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(device, &pl, nullptr, &m_layout) != VK_SUCCESS)
        throw std::runtime_error("Item pipeline layout failed");

    const auto vertexBytes = kCubeVertices.size() * sizeof(Vertex);
    m_vertices = createBuffer(context.getAllocator(), vertexBytes,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    uploadBuffer(context.getAllocator(), imm, m_vertices, kCubeVertices.data(), vertexBytes);
    const auto indexBytes = kCubeIndices.size() * sizeof(uint16_t);
    m_indices = createBuffer(context.getAllocator(), indexBytes,
                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    uploadBuffer(context.getAllocator(), imm, m_indices, kCubeIndices.data(), indexBytes);
    for (auto &slot : m_instances)
        slot = createBuffer(context.getAllocator(), kMaxItems * sizeof(Instance),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                VMA_ALLOCATION_CREATE_MAPPED_BIT);

    recreatePipelines(color, depth);
}

void ItemEntityRenderer::recreatePipelines(VkFormat color, VkFormat depth)
{
    // Extracted from init (review M17, the MobRenderer pattern): the
    // swapchain-recreate path rebuilds the item pipeline when the HDR/depth
    // formats change; the caller waits the device first.
    const auto device = m_context->getDevice();
    if (m_pipeline)
        vkDestroyPipeline(device, m_pipeline, nullptr);
    m_pipeline = VK_NULL_HANDLE;
    const std::array<VkVertexInputBindingDescription, 2> vb{
        {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
         {1, sizeof(Instance), VK_VERTEX_INPUT_RATE_INSTANCE}}};
    const std::array<VkVertexInputAttributeDescription, 9> va{
        {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
         {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
         {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
         {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
         {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
         {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
         {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48},
         {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 64},
         {8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 80}}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 2;
    vi.pVertexBindingDescriptions = vb.data();
    vi.vertexAttributeDescriptionCount = 9;
    vi.pVertexAttributeDescriptions = va.data();

    auto vert = loadShaderModule(device, resolveSpvPath("item.vert.spv"));
    try
    {
        auto frag = loadShaderModule(device, resolveSpvPath("item.frag.spv"));
        try
        {
            const VkPipelineShaderStageCreateInfo stages[2] = {
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr}};
            VkPipelineRasterizationStateCreateInfo rs{
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE; // closed mini-cube; the mob convention
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1;
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
                .setDepth(ds)
                .setColorBlendAttachments(&blend, 1)
                .setRenderingFormats(&color, 1, depth);
            m_pipeline = builder.build(device);
        }
        catch (...)
        {
            destroyShaderModule(device, frag);
            throw;
        }
        destroyShaderModule(device, frag);
    }
    catch (...)
    {
        destroyShaderModule(device, vert);
        throw;
    }
    destroyShaderModule(device, vert);
}

void ItemEntityRenderer::shutdown()
{
    if (!m_context)
        return;
    const auto device = m_context->getDevice();
    m_context->waitIdle();
    if (m_pipeline)
        vkDestroyPipeline(device, m_pipeline, nullptr);
    if (m_layout)
        vkDestroyPipelineLayout(device, m_layout, nullptr);
    if (m_vertices.buffer)
        destroyBuffer(m_context->getAllocator(), m_vertices);
    if (m_indices.buffer)
        destroyBuffer(m_context->getAllocator(), m_indices);
    for (auto &slot : m_instances)
        if (slot.buffer)
            destroyBuffer(m_context->getAllocator(), slot);
    m_pipeline = VK_NULL_HANDLE;
    m_layout = VK_NULL_HANDLE;
    m_context = nullptr;
}

void ItemEntityRenderer::prepare(uint32_t frame, const FrameUBO &ubo,
                                 const std::vector<ItemRenderState> &items)
{
    PROFILE_SCOPE("ItemPrepare");
    // Clamp + one-shot log instead of a frame-aborting throw (review §4.6,
    // the MobRenderer pattern): the sim's population cap makes this
    // unreachable; presentation degrades gracefully instead.
    if (items.size() > kMaxItems)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            std::cerr << "[items] input count " << items.size() << " exceeds renderer capacity "
                      << kMaxItems << " — clamped\n";
        }
    }
    m_viewProjection[frame] = ubo.projection * ubo.view;
    auto *instances = static_cast<Instance *>(m_instances[frame].info.pMappedData);
    uint32_t count = 0;
    for (const ItemRenderState &item : items)
    {
        if (count >= kMaxItems)
            break; // instance buffer bound (the clamp above)
        // Conservative sphere around the mini-cube center (feet + half body).
        const glm::vec3 center = item.position + glm::vec3(0.0f, 0.125f + item.bob, 0.0f);
        if (!visible(m_viewProjection[frame], center, 0.3f))
            continue;
        const float sky =
            (item.localSkylight < 0.0f) ? 1.0f : std::clamp(item.localSkylight, 0.0f, 1.0f);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), center);
        model = glm::rotate(model, item.rotation, glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(0.25f));
        instances[count++] = {model,
                              {static_cast<float>(item.atlasLayer), sky, 0.0f, 0.0f},
                              {item.localBlockRgb.r, item.localBlockRgb.g, item.localBlockRgb.b, 0.0f}};
    }
    m_visibleCount[frame] = count;
    m_visible = count;
    if (count)
        vmaFlushAllocation(m_context->getAllocator(), m_instances[frame].allocation, 0,
                           count * sizeof(Instance));
}

void ItemEntityRenderer::record(VkCommandBuffer cmd, uint32_t frame, VkDescriptorSet frameSet,
                                VkDescriptorSet atlasSet)
{
    const uint32_t count = m_visibleCount[frame];
    if (!count)
        return;
    PROFILE_SCOPE("ItemDraw");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    const std::array<VkDescriptorSet, 2> sets{frameSet, atlasSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 2, sets.data(), 0, nullptr);
    vkCmdPushConstants(cmd, m_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4),
                       &m_viewProjection[frame]);
    const std::array<VkBuffer, 2> buffers{m_vertices.buffer, m_instances[frame].buffer};
    const VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets);
    vkCmdBindIndexBuffer(cmd, m_indices.buffer, 0, VK_INDEX_TYPE_UINT16);
    // ONE instanced indexed draw for the whole visible population: the layer
    // travels per instance, so no texture-run splitting is needed.
    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(kCubeIndices.size()), count, 0, 0, 0);
}
