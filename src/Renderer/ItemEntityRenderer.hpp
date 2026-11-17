#pragma once

// =============================================================================
// ItemEntityRenderer — DERIVED dropped-item presentation (issue #229, Epic
// #223 Phase F). Fed by the host from the simulation's id-sorted bounded
// ItemEntitySnapshots AFTER the sim advance (the mob snapshot pattern):
// one atlas-textured mini-cube per visible entity, previous/current
// interpolation done by the host, frustum-culled, ONE instanced indexed
// draw, bounded by the population cap. No shadow casting in v1 (items
// receive directional shadows like mobs; there is no shadow-pass pipeline).
//
// Strict boundary: the renderer holds NO authoritative state — it survives
// reload by re-reading snapshots; the Engine's epoch barrier drops the
// presentation states on world transitions (the mobStates discipline).
// =============================================================================

#include <Renderer/FrameUBO.hpp>
#include <Vulkan/VkBuffer.hpp>
#include <Vulkan/VkCommands.hpp>
#include <Vulkan/VkContext.hpp>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/// Derived per-item presentation state (the MobRenderState pattern): the
/// host interpolates the sim snapshot's previous/current positions, resolves
/// the atlas layer through the item registry (ItemId -> placeableBlock),
/// derives the presentation spin/bob from the authoritative age, and samples
/// the bridge's local light. Nothing here is persisted or authoritative.
struct ItemRenderState
{
    glm::vec3 position{0.0f};   // interpolated item feet position (world)
    uint32_t atlasLayer{0};     // block atlas layer (registry placeableBlock ordinal)
    float rotation{0.0f};       // Y spin, radians (presentation-only)
    float bob{0.0f};            // vertical bob offset (presentation-only)
    float localSkylight{-1.0f}; // < 0 = unassigned: host samples the bridge; renderer defaults 1
    glm::vec3 localBlockRgb{0.0f};
};

class ItemEntityRenderer
{
  public:
    /// Population-cap pin: mirrors items::ItemEntitySettings::capacity
    /// (static_assert in the .cpp keeps them aligned).
    static constexpr uint32_t kMaxItems = 1024;

    ItemEntityRenderer() = default;
    ~ItemEntityRenderer() { shutdown(); }
    ItemEntityRenderer(const ItemEntityRenderer &) = delete;
    ItemEntityRenderer &operator=(const ItemEntityRenderer &) = delete;

    /// `atlasLayout` is the WorldRenderer set-1 layout (binding 0 = block
    /// atlas array, binding 1 = shadow array) — the item pipeline samples
    /// the SAME descriptors the terrain/mobs use, so resource-pack reloads
    /// and shadow resolution changes rebind items automatically.
    void init(VkContext &context, ImmediateCommands &imm, VkDescriptorSetLayout frameLayout,
              VkDescriptorSetLayout atlasLayout, VkFormat color, VkFormat depth);
    /// Rebuild the pipeline for new attachment formats (review M17:
    /// swapchain recreate). Caller must device-idle first.
    void recreatePipelines(VkFormat color, VkFormat depth);
    void shutdown();

    /// Cull + build the frame slot's instance slice (allocation-free steady
    /// state, the MobRenderer::prepare discipline). Throws when the input
    /// exceeds kMaxItems (a sim/renderer capacity drift, never silent).
    void prepare(uint32_t frame, const FrameUBO &ubo, const std::vector<ItemRenderState> &items);

    /// Record into the opaque pass AFTER mobs: one instanced indexed draw
    /// for every visible item (the shared unit cube, per-instance layer).
    void record(VkCommandBuffer cmd, uint32_t frame, VkDescriptorSet frameSet, VkDescriptorSet atlasSet);

    size_t visibleCount() const { return m_visible; }

    /// Vertex/instance layouts are public contracts (the MobRenderer
    /// instanceStride pattern): tests and the static geometry table share them.
    struct Vertex
    {
        glm::vec3 position;
        glm::vec3 normal;
        glm::vec2 uv;
    };
    struct Instance
    {
        glm::mat4 model;
        glm::vec4 atlasAndSky; // x = atlas layer, y = skylight
        glm::vec4 blockLight;  // rgb
    };

  private:
    VkContext *m_context{nullptr};
    VkPipelineLayout m_layout{VK_NULL_HANDLE};
    VkPipeline m_pipeline{VK_NULL_HANDLE};
    AllocatedBuffer m_vertices{};
    AllocatedBuffer m_indices{};
    std::array<AllocatedBuffer, 2> m_instances{};
    std::array<glm::mat4, 2> m_viewProjection{};
    std::array<uint32_t, 2> m_visibleCount{};
    size_t m_visible{0};
};
