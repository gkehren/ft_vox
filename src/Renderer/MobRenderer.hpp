#pragma once
#include <Entities/MobTypes.hpp>
#include <Entities/MobSystem.hpp>
#include <Entities/MobModel.hpp>
#include <Renderer/MobTextures.hpp>
#include <Renderer/FrameUBO.hpp>
#include <Vulkan/VkContext.hpp>
#include <Vulkan/VkImage.hpp>
#include <memory>

class MobRenderer
{
  public:
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
    void shutdown();
    std::unique_ptr<Textures> prepareTextures(ImmediateCommands &, const std::string &);
    void commitTextures(std::unique_ptr<Textures> textures) { m_textures.swap(textures); }
    MobTextureReport textureReport() const { return m_textures ? m_textures->report : MobTextureReport{}; }
    void prepare(uint32_t frame, const FrameUBO &, const std::vector<entities::MobRenderState> &);
    void record(VkCommandBuffer, uint32_t frame, VkDescriptorSet frameSet, int cascade = -1);
    size_t visibleCount() const { return m_visible; }

  private:
    struct Instance
    {
        glm::mat4 model;
        glm::vec4 uvScale;
    };
    struct Draw
    {
        uint32_t first, count, instance, texture;
        uint8_t visibility;
    };
    static constexpr size_t kMaxParts = 32 * entities::MobSettings::capacity;
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
