#include <Engine/WorkloadTelemetry.hpp>
#include <utils.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

static void require(bool b, const char* message) { if (!b) throw std::runtime_error(message); }
int main() {
    try {
        using namespace telemetry;
        Registry r;
        if (!r.enabled) {
            r.replace(VoxelBytes, 0, 1024); r.add(AllocCreated);
            auto s = r.snapshot();
            require(!s.enabled && !s.current[VoxelBytes] && !s.events[AllocCreated], "disabled instrumentation");
            return 0;
        }
        r.replace(VoxelBytes, 0, 1024);
        r.replace(VoxelBytes, 1024, 512);
        r.add(AllocCreated, 4);
        auto oldEpoch = r.captureEpoch();
        r.beginCapture();
        Snapshot work; work.stageCalls[Skylight] = 1; work.stageNs[Skylight] = 100;
        r.worker(oldEpoch, work);
        auto s = r.snapshot();
        require(s.current[VoxelBytes] == 512 && s.peak[VoxelBytes] == 512, "reset preserves ownership and rebases peak");
        require(!s.events[AllocCreated] && !s.stageCalls[Skylight], "reset excludes prior events and delayed workers");
        std::vector<std::thread> threads;
        const auto epoch = r.captureEpoch();
        for (int i=0; i<4; ++i) threads.emplace_back([&] {
            for (int j=0; j<1000; ++j) {
                r.replace(ShellCapacity, 0, 16);
                r.worker(epoch, work);
                r.add(AllocCreated);
                r.replace(ShellCapacity, 16, 0);
            }
        });
        for (auto& t : threads) t.join();
        s = r.snapshot();
        require(s.current[ShellCapacity] == 0 && s.peak[ShellCapacity] <= 64, "concurrent ownership balance");
        require(s.stageCalls[Skylight] == 4000 && s.stageNs[Skylight] == 400000, "worker aggregation");
        require(s.events[AllocCreated] == 4000, "concurrent counters");
        r.beginCapture();
        s = r.snapshot();
        require(!s.events[AllocCreated] && !s.stageCalls[Skylight], "second capture starts empty");

        // Capture-local vs persistent gauges (issue #111/#112 reviews):
        // warmup staging high-water, end-of-frame chunk-manager samples and
        // the VoxelPool active/free split must not leak into a new
        // measurement window, while ownership gauges (including the pool's
        // retained capacity) survive the boundary with their peak rebased.
        Registry w;
        w.replace(VoxelBytes, 0, 65536);
        w.set(StagingUsed, 1024 * 1024);
        w.set(ActiveChunks, 4200);
        w.set(DeferredChunks, 20);
        w.set(VoxelPoolCapacity, 4600);
        w.set(VoxelPoolCapacityBytes, 4600ull * CHUNK_VOLUME);
        w.set(VoxelPoolActive, 4200);
        w.set(VoxelPoolFree, 400);
        // mesh.pool.* (issue #104/#114): retained capacity is persistent,
        // active/free splits are capture-local.
        w.set(MeshPoolCapacity, 20);
        w.set(MeshPoolCapacityBytes, 20ull * 1024 * 1024);
        w.set(MeshPoolActive, 15);
        w.set(MeshPoolFree, 5);
        w.add(AllocCreated);
        w.add(UploadChunks);
        w.add(StagingFailures);
        w.add(OpaqueDraws);
        const auto warm = w.snapshot();
        require(warm.peak[StagingUsed] == 1024 * 1024, "warmup staging peak recorded");
        w.beginCapture();
        const auto reset = w.snapshot();
        require(reset.current[VoxelBytes] == 65536 && reset.peak[VoxelBytes] == 65536,
                "persistent ownership survives capture, peak rebased");
        require(reset.current[VoxelPoolCapacity] == 4600 && reset.peak[VoxelPoolCapacity] == 4600,
                "voxel pool retained capacity survives capture");
        require(reset.current[VoxelPoolCapacityBytes] == 4600ull * CHUNK_VOLUME &&
                reset.peak[VoxelPoolCapacityBytes] == 4600ull * CHUNK_VOLUME,
                "voxel pool retained capacity bytes survive capture");
        require(reset.current[StagingUsed] == 0 && reset.peak[StagingUsed] == 0,
                "capture resets transient staging current and peak");
        require(reset.current[ActiveChunks] == 0 && reset.peak[ActiveChunks] == 0 &&
                reset.current[DeferredChunks] == 0 && reset.peak[DeferredChunks] == 0,
                "capture resets sampled chunk-manager gauges");
        require(reset.current[VoxelPoolActive] == 0 && reset.peak[VoxelPoolActive] == 0,
                "voxel pool active resets at capture boundary");
        require(reset.current[VoxelPoolFree] == 0 && reset.peak[VoxelPoolFree] == 0,
                "voxel pool free resets at capture boundary");
        require(reset.current[MeshPoolCapacity] == 20 && reset.peak[MeshPoolCapacity] == 20 &&
                reset.current[MeshPoolCapacityBytes] == 20ull * 1024 * 1024,
                "mesh pool retained capacity survives capture");
        require(reset.current[MeshPoolActive] == 0 && reset.peak[MeshPoolActive] == 0 &&
                reset.current[MeshPoolFree] == 0 && reset.peak[MeshPoolFree] == 0,
                "mesh pool active/free reset at capture boundary");
        require(!reset.events[AllocCreated] && !reset.events[UploadChunks] &&
                !reset.events[StagingFailures] && !reset.events[OpaqueDraws],
                "capture clears event categories");
        w.set(StagingUsed, 64 * 1024);
        w.set(ActiveChunks, 4300);
        w.set(DeferredChunks, 7);
        w.set(VoxelPoolActive, 3500);
        w.set(VoxelPoolFree, 1100);
        w.set(MeshPoolActive, 3);
        w.set(MeshPoolFree, 17);
        const auto measured = w.snapshot();
        require(measured.current[StagingUsed] == 64 * 1024 &&
                measured.peak[StagingUsed] == 64 * 1024,
                "measured staging high-water starts from zero");
        require(measured.peak[ActiveChunks] == 4300 && measured.peak[DeferredChunks] == 7,
                "measured chunk peaks reflect measured frames only");
        require(measured.current[VoxelPoolActive] == 3500 && measured.peak[VoxelPoolActive] == 3500,
                "active peak belongs to measured capture");
        require(measured.current[VoxelPoolFree] == 1100 && measured.peak[VoxelPoolFree] == 1100,
                "free peak belongs to measured capture");
        require(measured.current[MeshPoolActive] == 3 && measured.peak[MeshPoolActive] == 3 &&
                measured.current[MeshPoolFree] == 17 && measured.peak[MeshPoolFree] == 17,
                "measured mesh pool peaks are capture-local");

        // Two successive captures (benchmark -> reload -> new benchmark in
        // the same process): the previous run's peaks must not contribute to
        // the next window.
        w.beginCapture();
        w.set(VoxelPoolActive, 3900);
        w.set(VoxelPoolFree, 700);
        const auto second = w.snapshot();
        require(second.peak[VoxelPoolActive] == 3900,
                "second capture active peak is its own, not the previous run's");
        require(second.peak[VoxelPoolFree] == 700,
                "second capture free peak is its own, not the previous run's");

        // Repeated captures (the warmup=0 path fires beginCapture several
        // times in a row) must keep ownership intact and sampled gauges at
        // zero.
        w.beginCapture();
        w.beginCapture();
        w.beginCapture();
        const auto repeated = w.snapshot();
        require(repeated.current[VoxelBytes] == 65536 && repeated.peak[VoxelBytes] == 65536,
                "repeated captures keep ownership");
        require(repeated.current[StagingUsed] == 0 && repeated.peak[StagingUsed] == 0 &&
                repeated.current[ActiveChunks] == 0 && repeated.current[DeferredChunks] == 0,
                "repeated captures keep sampled gauges at zero");
        require(repeated.current[VoxelPoolCapacity] == 4600 &&
                repeated.current[VoxelPoolCapacityBytes] == 4600ull * CHUNK_VOLUME,
                "repeated captures keep retained voxel pool capacity");

        std::cout << "PASS: telemetry concurrency, ownership, capture epochs, reset, "
                     "capture-local gauges\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
