#include <Engine/WorkloadTelemetry.hpp>
#include <utils.hpp>
#include <cmath>
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
        Snapshot work; work.stageCalls[MeshFamily][Skylight] = 1; work.stageNs[MeshFamily][Skylight] = 100;
        r.worker(oldEpoch, work);
        auto s = r.snapshot();
        require(s.current[VoxelBytes] == 512 && s.peak[VoxelBytes] == 512, "reset preserves ownership and rebases peak");
        require(!s.events[AllocCreated] && !s.stageCalls[MeshFamily][Skylight], "reset excludes prior events and delayed workers");
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
        require(s.stageCalls[MeshFamily][Skylight] == 4000 && s.stageNs[MeshFamily][Skylight] == 400000, "worker aggregation");
        require(s.events[AllocCreated] == 4000, "concurrent counters");
        r.beginCapture();
        s = r.snapshot();
        require(!s.events[AllocCreated] && !s.stageCalls[MeshFamily][Skylight], "second capture starts empty");

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
        w.set(LightPoolCapacity, 10);
        w.set(LightPoolCapacityBytes, 10ull * 128 * 1024);
        w.set(LightPoolActive, 7);
        w.set(LightPoolFree, 3);
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
        require(reset.current[LightPoolCapacity] == 10 && reset.peak[LightPoolCapacity] == 10 &&
                reset.current[LightPoolCapacityBytes] == 10ull * 128 * 1024,
                "light pool retained capacity survives capture");
        require(reset.current[LightPoolActive] == 0 && reset.peak[LightPoolActive] == 0 &&
                reset.current[LightPoolFree] == 0 && reset.peak[LightPoolFree] == 0,
                "light pool active/free reset at capture boundary");
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
        w.set(LightPoolActive, 2);
        w.set(LightPoolFree, 8);
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
        require(measured.current[LightPoolActive] == 2 && measured.peak[LightPoolActive] == 2 &&
                measured.current[LightPoolFree] == 8 && measured.peak[LightPoolFree] == 8,
                "measured light pool peaks are capture-local");

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

        // Per-call sample retention (benchmark avg/p95): MeshSample keeps one
        // duration per completed stage segment plus one chain-sum sample per
        // completed mesh, StageSample keeps its single stage, and a capture
        // boundary clears the samples together with the totals.
        {
            Registry& g = registry();
            g.beginCapture();
            {
                MeshSample mesh(Skylight);
                mesh.next(Blocklight);
                mesh.next(FacesGreedyAO);
            }
            { StageSample occupancy(Occupancy); }
            const auto sampled = g.snapshot();
            require(sampled.stageCalls[MeshFamily][Skylight] == 1 && sampled.stageCalls[MeshFamily][Blocklight] == 1 &&
                    sampled.stageCalls[MeshFamily][FacesGreedyAO] == 1 && sampled.stageCalls[MeshFamily][Occupancy] == 1,
                    "mesh chain and standalone stage calls recorded");
            require(sampled.stageSamplesMs[MeshFamily][Skylight].size() == 1 &&
                    sampled.stageSamplesMs[MeshFamily][Blocklight].size() == 1 &&
                    sampled.stageSamplesMs[MeshFamily][FacesGreedyAO].size() == 1 &&
                    sampled.stageSamplesMs[MeshFamily][Occupancy].size() == 1,
                    "one duration sample per completed stage segment");
            require(sampled.totalSamplesMs[MeshFamily].size() == 1,
                    "one chain-sum sample per completed mesh");
            const double chainMs = double(sampled.stageNs[MeshFamily][Skylight] + sampled.stageNs[MeshFamily][Blocklight] +
                                          sampled.stageNs[MeshFamily][FacesGreedyAO]) / 1e6;
            require(std::abs(sampled.totalSamplesMs[MeshFamily][0] - chainMs) < 1e-4,
                    "mesh total sample is the sum of the stage chain");
            g.beginCapture();
            const auto cleared = g.snapshot();
            require(cleared.stageSamplesMs[MeshFamily][Skylight].empty() && cleared.stageSamplesMs[MeshFamily][Occupancy].empty() &&
                    cleared.totalSamplesMs[MeshFamily].empty(),
                    "capture boundary clears retained samples");
        }

        // Family isolation (issue #173 review): a LightCache-family build
        // sample must publish only under lightCache.* and never touch the
        // mesh.* gauges or the mesh per-build totals.
        {
            Registry& g = registry();
            g.beginCapture();
            {
                MeshSample light(Skylight, LightCacheFamily);
                light.next(Blocklight);
            }
            { StageSample halo(HaloFill, LightCacheFamily); }
            const auto sampled = g.snapshot();
            require(sampled.stageCalls[LightCacheFamily][Skylight] == 1 &&
                    sampled.stageCalls[LightCacheFamily][Blocklight] == 1 &&
                    sampled.stageCalls[LightCacheFamily][HaloFill] == 1,
                    "light-cache chain calls recorded under lightCache.*");
            require(sampled.totalSamplesMs[LightCacheFamily].size() == 1,
                    "one chain-sum sample per completed light-cache build");
            for (size_t i = 0; i < StageCount; ++i)
                require(!sampled.stageCalls[MeshFamily][i] && sampled.totalSamplesMs[MeshFamily].empty(),
                        "light-cache sample never contaminates mesh.* telemetry");
            const double chainMs = double(sampled.stageNs[LightCacheFamily][Skylight] +
                                          sampled.stageNs[LightCacheFamily][Blocklight]) / 1e6;
            require(std::abs(sampled.totalSamplesMs[LightCacheFamily][0] - chainMs) < 1e-4,
                    "light-cache total sample is the sum of its stage chain");
        }

        // Live read semantics (issue #179): sampleLive() backs the developer
        // console. It must not reset capture epochs, must not consume
        // benchmark capture data, must be repeatable, and must stay coherent
        // while workers publish into the same registry.
        {
            Registry& g = registry();
            g.beginCapture();
            const uint64_t epoch = g.captureEpoch();
            g.replace(VoxelBytes, 0, 4096);
            g.set(ActiveChunks, 77);
            g.add(UploadChunks, 3);
            auto live1 = g.sampleLive();
            require(live1.enabled && live1.current[VoxelBytes] == 4096 &&
                        live1.current[ActiveChunks] == 77 && live1.events[UploadChunks] == 3,
                    "live read sees current gauges and events");
            auto live2 = g.sampleLive();
            require(live2.current[VoxelBytes] == 4096 && live2.events[UploadChunks] == 3,
                    "live reads are repeatable (non-destructive)");
            require(g.captureEpoch() == epoch, "live read does not touch the capture epoch");
            auto afterLive = g.snapshot();
            require(afterLive.current[VoxelBytes] == 4096 &&
                        afterLive.current[ActiveChunks] == 77 && afterLive.events[UploadChunks] == 3,
                    "benchmark snapshot after live reads still holds the full capture");

            g.beginCapture();
            const auto e2 = g.captureEpoch();
            Snapshot work;
            work.stageCalls[MeshFamily][Skylight] = 1;
            work.stageNs[MeshFamily][Skylight] = 125;
            std::thread liveReader([&] {
                for (int i = 0; i < 2000; ++i) (void)g.sampleLive();
            });
            for (int i = 0; i < 1000; ++i) g.worker(e2, work);
            liveReader.join();
            auto live3 = g.sampleLive();
            require(live3.stageCalls[MeshFamily][Skylight] == 1000 &&
                        live3.stageNs[MeshFamily][Skylight] == 125000,
                    "live view coherent with concurrent worker publication");
            auto post = g.snapshot();
            require(post.stageCalls[MeshFamily][Skylight] == 1000,
                    "benchmark capture intact after concurrent live reads");
        }

        std::cout << "PASS: telemetry concurrency, ownership, capture epochs, reset, "
                     "capture-local gauges, live reads\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
