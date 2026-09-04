#include <Engine/WorkloadTelemetry.hpp>
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

        // Capture-local vs persistent gauges (issue #111 review): warmup
        // staging high-water and end-of-frame chunk-manager samples must not
        // leak into a new measurement window, while ownership gauges survive
        // the boundary with their peak rebased to the carried-over value.
        Registry w;
        w.replace(VoxelBytes, 0, 65536);
        w.set(StagingUsed, 1024 * 1024);
        w.set(ActiveChunks, 4200);
        w.set(DeferredChunks, 20);
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
        require(reset.current[StagingUsed] == 0 && reset.peak[StagingUsed] == 0,
                "capture resets transient staging current and peak");
        require(reset.current[ActiveChunks] == 0 && reset.peak[ActiveChunks] == 0 &&
                reset.current[DeferredChunks] == 0 && reset.peak[DeferredChunks] == 0,
                "capture resets sampled chunk-manager gauges");
        require(!reset.events[AllocCreated] && !reset.events[UploadChunks] &&
                !reset.events[StagingFailures] && !reset.events[OpaqueDraws],
                "capture clears event categories");
        w.set(StagingUsed, 64 * 1024);
        w.set(ActiveChunks, 4300);
        w.set(DeferredChunks, 7);
        const auto measured = w.snapshot();
        require(measured.current[StagingUsed] == 64 * 1024 &&
                measured.peak[StagingUsed] == 64 * 1024,
                "measured staging high-water starts from zero");
        require(measured.peak[ActiveChunks] == 4300 && measured.peak[DeferredChunks] == 7,
                "measured chunk peaks reflect measured frames only");

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

        std::cout << "PASS: telemetry concurrency, ownership, capture epochs, reset, "
                     "capture-local gauges\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
