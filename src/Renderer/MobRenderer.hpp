#pragma once
#include <Entities/MobTypes.hpp>
#include <Entities/MobModel.hpp>
#include <Renderer/MobTextures.hpp>
#include <Renderer/FrameUBO.hpp>
#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkImage.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class MobRenderer
{
  public:
    /// Sampler policy for the mob albedo textures. Mipmapped is the shipping
    /// path (NEAREST magnification and minification over the full UV-rect-aware
    /// chain, LINEAR mip SELECTION — faces are adjacent in the atlas, so
    /// intra-level LINEAR would blend them right at their shared border).
    /// LinearMips keeps the intra-level bilinear and exists as the seam-test
    /// reference; NearestMip0 reproduces the pre-#160 behavior for the A/B
    /// temporal regression test.
    enum class SamplerPolicy { Mipmapped, LinearMips, NearestMip0 };
    struct Textures
    {
        VkContext *context{};
        std::array<AllocatedImage, 6> images{};
        std::array<VkDescriptorSet, 6> sets{};
        VkSampler sampler{};
        VkDescriptorPool pool{};
        MobTextureReport report{};
        ~Textures();
    };
    ~MobRenderer() { shutdown(); }
    void init(VkContext &, ImmediateCommands &, VkDescriptorSetLayout frameLayout, VkImageView shadow,
              VkSampler shadowSampler, VkFormat color, VkFormat depth, const std::string &pack);
    /// Rebind the shadow map view + sampler in every committed per-model
    /// descriptor set (issue #137: shadow map resolution changes recreate
    /// both the image and the sampler). Caller must device-idle first.
    void refreshShadowBinding(VkImageView shadowView, VkSampler shadowSampler);
    void shutdown();
    std::unique_ptr<Textures> prepareTextures(ImmediateCommands &, const std::string &,
                                              SamplerPolicy policy = SamplerPolicy::Mipmapped);
    void commitTextures(std::unique_ptr<Textures> textures) { m_textures.swap(textures); }
    MobTextureReport textureReport() const { return m_textures ? m_textures->report : MobTextureReport{}; }
    VkFormat textureFormat() const { return (m_textures && m_textures->images[0].image) ? m_textures->images[0].format : VK_FORMAT_UNDEFINED; }
    /// Mip levels of the committed mob image `index` (0 when unset) — the
    /// per-texture form lets tests assert every image's chain, not just [0].
    uint32_t textureMipLevels(size_t index) const
    {
        return (m_textures && index < m_textures->images.size() && m_textures->images[index].image)
                   ? m_textures->images[index].mipLevels
                   : 0;
    }
    /// Logical RGBA8 texel payload across all committed mob albedo images,
    /// mip chains included. This is the texture content size, not the VMA
    /// allocation size (which adds alignment/padding).
    size_t textureTexelBytes() const;
    /// Mip 0 payload of the same images — the pre-#160 single-level footprint.
    size_t textureMip0Bytes() const;
    void prepare(uint32_t frame, const FrameUBO &, const std::vector<entities::MobRenderState> &);
    void record(VkCommandBuffer, uint32_t frame, VkDescriptorSet frameSet, int cascade = -1);
    size_t visibleCount() const { return m_visible; }

    /// Render passes that consume mob instances: bit 0 = camera/color pass,
    /// bits 1-3 = shadow cascades 0-2 (issue #130).
    static constexpr uint32_t kMobPassCount = 4;
    /// Part-budget contract per baked model. init() validates every species
    /// model against this limit; exceeding it is a build-time-visible renderer
    /// capacity error, not a silent overflow (issue #130 review).
    static constexpr size_t kMaxPartsPerMob = 32;
    /// Static instancing batch: one baked MobPart's shared geometry range.
    /// Every visible mob instance of this part in one render pass is drawn by
    /// a single vkCmdDraw.
    struct BatchInfo
    {
        uint32_t firstVertex{}, vertexCount{}, texture{};
        entities::MobSpecies species{};
        uint32_t partIndex{}; ///< index of the MobPart inside its species model
    };
    /// Submission accounting of the last prepare() on `frame`: draws = vkCmdDraw
    /// calls record() issues for `pass`, instances = mob-part instances fed.
    struct PassStats
    {
        uint32_t draws{}, instances{};
    };
    uint32_t batchCount() const { return uint32_t(m_batches.size()); }
    BatchInfo batchInfo(uint32_t batch) const;
    PassStats passStats(uint32_t frame, uint32_t pass) const { return m_stats[frame][pass]; }
    /// Byte stride of one instance record in the mapped instance buffer.
    /// Exposed so tests can verify the pass-slice offsets actually bound by
    /// vkCmdBindVertexBuffers without duplicating the private Instance layout
    /// (issue #130 review).
    static constexpr size_t instanceStride() { return sizeof(Instance); }

  private:
    struct Instance
    {
        glm::mat4 model;
        glm::vec4 uvScale;
        glm::vec4 localLight;
    };
    /// One draw command: all instances of one static batch visible in one
    /// pass. firstInstance is relative to the pass instance slice bound by
    /// record().
    struct BatchDraw
    {
        uint32_t firstVertex, vertexCount, firstInstance, instanceCount, texture;
    };
    struct Batch
    {
        uint32_t firstVertex, vertexCount, texture;
    };
    // Budgets derived from the entity contract: at most kMaxPartsPerMob parts
    // per model (validated in init()) and kMaxMobCount mobs. Each of the
    // kMobPassCount instance slices holds up to kMaxParts instances, so the
    // per-frame mapped buffer is
    // kMaxParts * kMobPassCount * sizeof(Instance) (~576 KiB per frame slot).
    // A pass needing more instances than kMaxParts throws in prepare().
    static constexpr size_t kMaxParts = kMaxPartsPerMob * entities::kMaxMobCount;
    static constexpr size_t kMaxBatches = kMaxPartsPerMob * size_t(entities::kMobSpeciesCount);
    VkContext *m_context{};
    VkDescriptorSetLayout m_textureLayout{}, m_frameLayout{};
    VkPipelineLayout m_layout{};
    VkPipeline m_color{}, m_shadow{};
    VkImageView m_shadowView{};
    VkSampler m_shadowSampler{};
    AllocatedBuffer m_vertices{};
    std::array<AllocatedBuffer, 2> m_instances{};
    /// Per frame slot, per pass: non-empty batches in static batch order
    /// (species/model/part order, so texture descriptor runs stay bounded).
    std::array<std::array<std::vector<BatchDraw>, kMobPassCount>, 2> m_draws;
    /// Per frame slot, per pass: instance index where the pass slice starts
    /// inside the mapped instance buffer ([camera][shadow0][shadow1][shadow2]).
    std::array<std::array<uint32_t, kMobPassCount>, 2> m_passBase{};
    std::array<std::array<PassStats, kMobPassCount>, 2> m_stats{};
    /// Immutable static batch table built in init(); indexed by batchId.
    std::vector<Batch> m_batches;
    std::array<uint32_t, size_t(entities::kMobSpeciesCount)> m_batchBegin{}, m_batchPartCount{};
    // prepare() scratch, allocated once — steady-state prepare is allocation-free.
    std::array<std::array<uint32_t, kMaxBatches>, kMobPassCount> m_counts{};
    std::array<std::array<uint32_t, kMaxBatches>, kMobPassCount> m_cursor{};
    std::array<glm::vec4, kMaxBatches> m_uvScales{};
    std::array<uint8_t, entities::kMaxMobCount> m_masks{};
    std::array<std::array<glm::mat4, 4>, 2> m_matrices{};
    std::unique_ptr<Textures> m_textures;
    entities::MobModels m_models;
    size_t m_visible{};
};
