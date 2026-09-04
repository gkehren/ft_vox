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
        std::cout << "PASS: telemetry concurrency, ownership, capture epochs and reset\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
