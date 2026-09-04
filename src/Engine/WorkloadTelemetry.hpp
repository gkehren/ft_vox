#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

// Independent of the CPU/GPU profilers. Gauges describe ownership; events
// describe a capture interval. No worker-owned vector is read by the sampler.
namespace telemetry {
enum Gauge : size_t {
    VoxelBytes, ShellBytes, ShellCapacity, OpaqueVertexBytes, OpaqueVertexCapacity,
    OpaqueIndexBytes, OpaqueIndexCapacity, WaterVertexBytes, WaterVertexCapacity,
    WaterIndexBytes, WaterIndexCapacity, ColumnBytes, OccupancyBytes,
    GpuOpaqueVertex, GpuOpaqueIndex, GpuWaterVertex, GpuWaterIndex,
    RetiredBytes, RetiredBuffers, PoolCapacity, PoolAcquired, PoolFree,
    ActiveChunks, DeferredChunks, StagingUsed, CpuMeshCapacity, GpuLiveBytes,
    VoxelPoolCapacity, VoxelPoolActive, VoxelPoolFree, VoxelPoolCapacityBytes,
    GaugeCount
};
inline constexpr const char* gaugeNames[] = {
    "voxel.bytes", "shell.sizeBytes", "shell.capacityBytes",
    "cpu.opaque.vertex.sizeBytes", "cpu.opaque.vertex.capacityBytes",
    "cpu.opaque.index.sizeBytes", "cpu.opaque.index.capacityBytes",
    "cpu.water.vertex.sizeBytes", "cpu.water.vertex.capacityBytes",
    "cpu.water.index.sizeBytes", "cpu.water.index.capacityBytes",
    "column.bytes", "occupancy.bytes", "gpu.opaque.vertex.bytes",
    "gpu.opaque.index.bytes", "gpu.water.vertex.bytes", "gpu.water.index.bytes",
    "gpu.retired.bytes", "gpu.retired.buffers", "pool.capacity", "pool.acquired",
    "pool.free", "chunks.active", "chunks.deferred", "staging.slice.bytes",
    "cpu.mesh.capacityBytes", "gpu.live.bytes",
    "voxel.pool.capacity", "voxel.pool.active", "voxel.pool.free",
    "voxel.pool.capacityBytes"
};
enum Event : size_t {
    AllocCreated, AllocDestroyed, PoolRejected, UploadChunks, UploadVertexBytes,
    UploadIndexBytes, UploadDeferred, StagingFailures, OpaqueDraws, WaterDraws,
    Shadow0, Shadow1, Shadow2, VoxelPoolGrow, EventCount
};
inline constexpr const char* eventNames[] = {
    "mesh.allocations.created", "mesh.allocations.destroyed", "pool.rejected",
    "upload.chunks", "upload.vertexBytes", "upload.indexBytes", "upload.deferred",
    "staging.failures", "draws.opaque", "draws.water", "draws.shadow.0",
    "draws.shadow.1", "draws.shadow.2", "voxel.pool.growEvents"
};
enum Stage : size_t { Skylight, Blocklight, Occupancy, FacesGreedyAO, Lod, StageCount };
inline constexpr const char* stageNames[] = {"skylight", "blocklight", "occupancy", "facesGreedyAO", "LOD"};

// Capture-local gauges describe transient state sampled during a capture
// window (staging slice usage; end-of-frame chunk-manager samples; the
// VoxelPool active/free split). Unlike ownership gauges they are NOT
// carried across a beginCapture() boundary: a warmup high-water mark must
// not become a measurement peak. Gauges default to persistent ownership;
// classify one here only if its value is meaningless outside the currently
// running capture. Note VoxelPoolCapacity/VoxelPoolCapacityBytes stay
// persistent: retained backing memory really exists at the boundary.
inline constexpr bool isCaptureLocalGauge(Gauge g) {
    switch (g) {
    case StagingUsed:
    case ActiveChunks:
    case DeferredChunks:
    case VoxelPoolActive:
    case VoxelPoolFree:
        return true;
    default:
        return false;
    }
}
struct Snapshot {
    bool enabled{false};
    std::array<uint64_t, GaugeCount> current{}, peak{};
    std::array<uint64_t, EventCount> events{};
    std::array<uint64_t, StageCount> stageNs{}, stageCalls{};
    uint64_t maskCells{}, aoVertices{}, opaqueVertices{}, opaqueIndices{}, waterVertices{}, waterIndices{};
};
class Registry {
public:
    const bool enabled = [] { const char* e = std::getenv("FT_VOX_TELEMETRY"); return !e || std::strcmp(e, "0") != 0; }();
    void replace(Gauge g, uint64_t before, uint64_t after) {
        if (!enabled) return;
        std::lock_guard lock(mutex);
        data.current[g] = data.current[g] - before + after;
        data.peak[g] = std::max(data.peak[g], data.current[g]);
        if (g >= GpuOpaqueVertex && g <= GpuWaterIndex) {
            data.current[GpuLiveBytes] = data.current[GpuOpaqueVertex] + data.current[GpuOpaqueIndex]
                + data.current[GpuWaterVertex] + data.current[GpuWaterIndex];
            data.peak[GpuLiveBytes] = std::max(data.peak[GpuLiveBytes], data.current[GpuLiveBytes]);
        }
    }
    void set(Gauge g, uint64_t value) {
        if (!enabled) return;
        std::lock_guard lock(mutex);
        data.current[g] = value;
        data.peak[g] = std::max(data.peak[g], value);
    }
    template<size_t N> void replaceCpu(const std::array<uint64_t, N>& before, const std::array<uint64_t, N>& after) {
        if (!enabled) return;
        std::lock_guard lock(mutex);
        for (size_t i=0; i<N; ++i) {
            data.current[i] = data.current[i] - before[i] + after[i];
            data.peak[i] = std::max(data.peak[i], data.current[i]);
        }
        data.current[CpuMeshCapacity] = data.current[OpaqueVertexCapacity] + data.current[OpaqueIndexCapacity]
            + data.current[WaterVertexCapacity] + data.current[WaterIndexCapacity];
        data.peak[CpuMeshCapacity] = std::max(data.peak[CpuMeshCapacity], data.current[CpuMeshCapacity]);
    }
    void add(Event e, uint64_t n = 1) {
        if (enabled) events[e].fetch_add(n, std::memory_order_relaxed);
    }
    // Only the main thread resets events. Worker batches carry an epoch.
    void beginCapture() {
        std::lock_guard lock(mutex);
        ++epoch;
        const auto previousCurrent = data.current;
        data = {};
        // Ownership gauges describe state that still exists at the capture
        // boundary: carry the value over and rebase the peak to it, so the
        // world built during warmup is not under-reported. Capture-local
        // gauges start at zero so warmup high-water marks (staging slice
        // usage, chunk-manager frame samples) cannot leak into measurement.
        for (size_t i = 0; i < GaugeCount; ++i) {
            if (!isCaptureLocalGauge(static_cast<Gauge>(i)))
                data.current[i] = data.peak[i] = previousCurrent[i];
        }
        for (auto& e : events) e.store(0, std::memory_order_relaxed);
    }
    uint64_t captureEpoch() {
        if (!enabled) return 0;
        std::lock_guard lock(mutex);
        return epoch;
    }
    void worker(uint64_t tag, const Snapshot& s) {
        if (!enabled) return;
        std::lock_guard lock(mutex);
        if (tag != epoch) return;
        for (size_t i=0; i<StageCount; ++i) {
            data.stageNs[i] += s.stageNs[i]; data.stageCalls[i] += s.stageCalls[i];
        }
        data.maskCells += s.maskCells; data.aoVertices += s.aoVertices;
        data.opaqueVertices += s.opaqueVertices; data.opaqueIndices += s.opaqueIndices;
        data.waterVertices += s.waterVertices; data.waterIndices += s.waterIndices;
    }
    Snapshot snapshot() {
        std::lock_guard lock(mutex);
        auto s = data; s.enabled = enabled;
        for (size_t i=0; i<EventCount; ++i) s.events[i] = events[i].load(std::memory_order_relaxed);
        return s;
    }
private:
    std::mutex mutex;
    Snapshot data{};
    std::array<std::atomic<uint64_t>, EventCount> events{};
    uint64_t epoch{1};
};
inline Registry& registry() { static Registry r; return r; }

// A few clock reads per mesh, one mutex acquisition on completion. Face/AO
// counts are local; no clock or atomic operation in the inner voxel loops.
class MeshSample {
    using Clock = std::chrono::steady_clock;
    uint64_t tag = registry().captureEpoch();
    Stage stage;
    Clock::time_point start{};
public:
    Snapshot data{};
    explicit MeshSample(Stage s) : stage(s) { if (tag) start = Clock::now(); }
    void next(Stage s) {
        if (!tag) return;
        auto now = Clock::now();
        data.stageNs[stage] += std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count();
        ++data.stageCalls[stage]; start = now; stage = s;
    }
    ~MeshSample() { if (tag) { next(stage); registry().worker(tag, data); } }
};
} // namespace telemetry
