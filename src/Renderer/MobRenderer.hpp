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
    /// path (NEAREST magnification, LINEAR minification over the full chain);
    /// NearestMip0 reproduces the pre-#160 behavior and exists for the A/B
    /// temporal regression test.
    enum class SamplerPolicy { Mipmapped, NearestMip0 };
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
    /// Mip levels of the first committed mob image (0 when none); asserts the
    /// full-chain GPU wiring for issue #160.
    uint32_t textureMipLevels() const { return (m_textures && m_textures->images[0].image) ? m_textures->images[0].mipLevels : 0; }
    /// Logical RGBA8 texel payload across all committed mob albedo images,
    /// mip chains included. This is the texture content size, not the VMA
    /// allocation size (which adds alignment/padding).
    size_t textureTexelBytes() const;
    /// Mip 0 payload of the same images — the pre-#160 single-level footprint.
    size_t textureMip0Bytes() const;
    void prepare(uint32_t frame, const FrameUBO &, const std::vector<entities::MobRenderState> &);
    void record(VkCommandBuffer, uint32_t frame, VkDescriptorSet frameSet, int cascade = -1);
    size_t visibleCount() const { return m_visible; }

  private:
    struct Instance
    {
        glm::mat4 model;
        glm::vec4 uvScale;
        glm::vec4 localLight;
    };
    struct Draw
    {
        uint32_t first, count, instance, texture;
        uint8_t visibility;
    };
    static constexpr size_t kMaxParts = 32 * entities::kMaxMobCount;
    VkContext *m_context{};
    VkDescriptorSetLayout m_textureLayout{}, m_frameLayout{};
    VkPipelineLayout m_layout{};
    VkPipeline m_color{}, m_shadow{};
    VkImageView m_shadowView{};
    VkSampler m_shadowSampler{};
    AllocatedBuffer m_vertices{};
    std::array<AllocatedBuffer, 2> m_instances{};
    std::array<std::vector<Draw>, 2> m_draws;
    std::array<std::array<glm::mat4, 4>, 2> m_matrices{};
    std::unique_ptr<Textures> m_textures;
    entities::MobModels m_models;
    size_t m_visible{};
};
