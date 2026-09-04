#include <Vulkan/VkGpuProfiler.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>

void VkGpuProfiler::init(VkContext &context, uint32_t slots)
{
    if (slots == 0)
    {
        m_supported = false;
        m_unavailableReason = UnavailableReason::NoSlots;
        return;
    }
    m_device = context.getDevice();
    const auto &limits = context.getDeviceProperties().limits;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(context.getPhysicalDevice(), &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(context.getPhysicalDevice(), &count, families.data());
    m_period = limits.timestampPeriod;
    const uint32_t family = context.getGraphicsQueueFamily();
    if (family >= count || family >= families.size())
    {
        m_supported = false;
        m_unavailableReason = UnavailableReason::InvalidQueueFamily;
        return;
    }
    m_validBits = families[family].timestampValidBits;
    m_supported = gpuTimestampSupported(m_validBits, m_period);
    if (const char *env = std::getenv("FT_VOX_GPU_PROFILING"))
        m_enabled = std::strcmp(env, "0") != 0;
    if (!m_supported)
    {
        m_unavailableReason = m_validBits == 0 ? UnavailableReason::NoTimestampBits :
            m_validBits > 64 ? UnavailableReason::InvalidTimestampBits :
            UnavailableReason::InvalidTimestampPeriod;
        return;
    }
    m_slots.resize(slots);
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = kQueries;
    for (auto &slot : m_slots)
        if (vkCreateQueryPool(m_device, &info, nullptr, &slot.pool) != VK_SUCCESS)
        {
            // Optional instrumentation must not prevent rendering.
            shutdown();
            m_unavailableReason = UnavailableReason::QueryPoolAllocationFailed;
            return;
        }
}

void VkGpuProfiler::shutdown()
{
    for (auto &slot : m_slots)
        if (slot.pool != VK_NULL_HANDLE)
            vkDestroyQueryPool(m_device, slot.pool, nullptr);
    m_slots.clear();
    m_recording = nullptr;
    m_supported = false;
    m_unavailableReason = UnavailableReason::NotInitialized;
    m_device = VK_NULL_HANDLE;
    clear();
}

void VkGpuProfiler::clear()
{
    ++m_generation;
    m_latest = {};
    m_history.fill(0.f);
    m_historyCount = m_historyWrite = 0;
}

void VkGpuProfiler::syncCapture(uint64_t epoch)
{
    if (epoch != m_epoch)
    {
        m_epoch = epoch;
        clear();
    }
}

void VkGpuProfiler::setEnabled(bool enabled)
{
    if (m_enabled != enabled)
    {
        m_enabled = enabled;
        clear();
    }
}

const char *VkGpuProfiler::status() const
{
    if (!m_supported)
    {
        switch (m_unavailableReason)
        {
        case UnavailableReason::NoSlots: return "GPU timestamps unavailable: no frame slots";
        case UnavailableReason::InvalidQueueFamily: return "GPU timestamps unavailable: invalid graphics queue family";
        case UnavailableReason::NoTimestampBits: return "GPU timestamps unavailable: graphics queue has timestampValidBits=0";
        case UnavailableReason::InvalidTimestampBits: return "GPU timestamps unavailable: invalid timestampValidBits";
        case UnavailableReason::InvalidTimestampPeriod: return "GPU timestamps unavailable: invalid timestampPeriod";
        case UnavailableReason::QueryPoolAllocationFailed: return "GPU timestamps unavailable: query pool allocation failed";
        default: return "GPU timestamps unavailable: not initialized";
        }
    }
    if (!m_enabled) return "GPU timestamps disabled";
    if (!m_latest.serial) return "Waiting for a completed GPU frame";
    return "GPU timestamp queries (delayed, graphics queue)";
}

void VkGpuProfiler::onSlotReady(uint32_t index)
{
    if (!m_supported) return;
    auto &slot = m_slots.at(index);
    if (!slot.pending) return;
    slot.pending = false;
    if (!m_enabled || slot.generation != m_generation) return;
    struct Query { uint64_t value, available; };
    std::array<Query, kQueries> queries{};
    // The existing frame fence has signalled. Never use WAIT_BIT or add a wait.
    const VkResult result = vkGetQueryPoolResults(m_device, slot.pool, 0, kQueries,
        sizeof(queries), queries.data(), sizeof(Query),
        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (result != VK_SUCCESS && result != VK_NOT_READY) return;
    GpuFrameSample sample;
    sample.benchmarkTag = slot.benchmarkTag;
    for (uint32_t i = 0; i < kGpuPassCount; ++i)
    {
        const uint32_t bit = uint32_t{1} << i;
        if (!(slot.ended & bit)) continue;
        if (!queries[i * 2].available || !queries[i * 2 + 1].available) return;
        sample.ms[i] = static_cast<float>(gpuTimestampMilliseconds(
            queries[i * 2].value, queries[i * 2 + 1].value, m_validBits, m_period));
        sample.present[i] = true;
    }
    if (!sample.present[static_cast<size_t>(GpuPass::Frame)]) return;
    sample.serial = ++m_serial;
    m_latest = sample;
    m_history[m_historyWrite] = sample.ms[0];
    m_historyWrite = (m_historyWrite + 1) % kHistorySize;
    m_historyCount = std::min(m_historyCount + 1, kHistorySize);
}

void VkGpuProfiler::beginRecording(VkCommandBuffer cmd, uint32_t index, uint64_t benchmarkTag)
{
    m_recording = nullptr;
    if (!m_supported) return;
    auto &slot = m_slots.at(index);
    slot.recorded = false;
    if (!m_enabled) return;
    slot.started = slot.ended = 0;
    slot.generation = m_generation;
    slot.benchmarkTag = benchmarkTag;
    m_recording = &slot;
    vkCmdResetQueryPool(cmd, slot.pool, 0, kQueries);
    beginPass(cmd, GpuPass::Frame);
}

void VkGpuProfiler::beginPass(VkCommandBuffer cmd, GpuPass pass)
{
    if (!m_recording) return;
    const auto i = static_cast<uint32_t>(pass);
    const uint32_t bit = uint32_t{1} << i;
    if (m_recording->started & bit) return;
    m_recording->started |= bit;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_recording->pool, i * 2);
}

void VkGpuProfiler::endPass(VkCommandBuffer cmd, GpuPass pass)
{
    if (!m_recording) return;
    const auto i = static_cast<uint32_t>(pass);
    const uint32_t bit = uint32_t{1} << i;
    if (!(m_recording->started & bit) || (m_recording->ended & bit)) return;
    m_recording->ended |= bit;
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_recording->pool, i * 2 + 1);
}

void VkGpuProfiler::endRecording(VkCommandBuffer cmd)
{
    if (!m_recording) return;
    endPass(cmd, GpuPass::Frame);
    m_recording->recorded = true;
    m_recording = nullptr;
}

void VkGpuProfiler::markSubmitted(uint32_t index)
{
    if (m_supported)
    {
        auto &slot = m_slots.at(index);
        slot.pending = slot.recorded;
        slot.recorded = false;
    }
}
