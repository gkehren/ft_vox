#pragma once

#include <Engine/GpuProfile.hpp>
#include <Vulkan/VkContext.hpp>
#include <vector>

/// Query storage belongs to the frame-slot owner. onSlotReady() must only be
/// called after that slot's existing fence, before resetting/re-recording it.
class VkGpuProfiler
{
public:
    VkGpuProfiler() = default;
    VkGpuProfiler(const VkGpuProfiler &) = delete;
    VkGpuProfiler &operator=(const VkGpuProfiler &) = delete;
    void init(VkContext &context, uint32_t slots);
    void shutdown(); // owner has waited for the device
    void onSlotReady(uint32_t slot);
    void beginRecording(VkCommandBuffer cmd, uint32_t slot, uint64_t benchmarkTag);
    void beginPass(VkCommandBuffer cmd, GpuPass pass);
    void endPass(VkCommandBuffer cmd, GpuPass pass);
    void endRecording(VkCommandBuffer cmd);
    void markSubmitted(uint32_t slot);
    void syncCapture(uint64_t epoch);
    void setEnabled(bool enabled);
    bool enabled() const { return m_enabled; }
    bool supported() const { return m_supported; }
    const char *status() const;
    const GpuFrameSample &latest() const { return m_latest; }
    const float *history() const { return m_history.data(); }
    int historyCount() const { return m_historyCount; }
    int historyWrite() const { return m_historyWrite; }
    static constexpr int kHistorySize = 240;
private:
    static constexpr uint32_t kQueries = static_cast<uint32_t>(kGpuPassCount * 2);
    struct Slot
    {
        VkQueryPool pool{VK_NULL_HANDLE};
        uint64_t generation{0}, benchmarkTag{0};
        uint32_t started{0}, ended{0};
        bool recorded{false}, pending{false};
    };
    void clear();
    VkDevice m_device{VK_NULL_HANDLE};
    std::vector<Slot> m_slots;
    Slot *m_recording{nullptr};
    bool m_supported{false}, m_enabled{true};
    float m_period{0.f};
    uint32_t m_validBits{0};
    uint64_t m_epoch{0}, m_generation{0}, m_serial{0};
    GpuFrameSample m_latest{};
    std::array<float, kHistorySize> m_history{};
    int m_historyCount{0}, m_historyWrite{0};
};
