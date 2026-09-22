#include <Engine/Benchmark.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char *message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    try
    {
        // The device-wide timestampComputeAndGraphics flag is deliberately not
        // an input: a selected queue with valid timestamps is sufficient.
        for (uint32_t bits : {36u, 40u, 48u, 56u, 64u})
            require(gpuTimestampSupported(bits, 1.f), "valid graphics timestamp width");
        require(!gpuTimestampSupported(0, 1.f), "queue without timestamps");
        require(!gpuTimestampSupported(65, 1.f), "invalid bit count");
        require(!gpuTimestampSupported(64, NAN), "invalid timestamp period");
        require(!gpuTimestampSupported(64, INFINITY), "infinite timestamp period");
        require(!gpuTimestampSupported(64, -1.f), "negative timestamp period");
        require(!gpuTimestampSupported(64, 0.f), "zero timestamp period");
        require(std::abs(gpuTimestampMilliseconds(250, 10, 8, 1000.f) - .016) < 1e-9, "8-bit wrap");
        require(std::abs(gpuTimestampMilliseconds(UINT64_MAX - 9, 10, 64, 1000.f) - .020) < 1e-9, "64-bit wrap");
        require(gpuTimestampMilliseconds(0, 2000000, 64, .5f) == 1., "nanoseconds to milliseconds");

        Camera camera;
        Benchmark benchmark;
        benchmark.config().warmupSec = 0.f;
        benchmark.config().durationSec = 5.f;
        benchmark.requestStart();
        benchmark.onWorldReady(glm::vec3(0.f));
        benchmark.tick(.001, camera);
        const auto tag = benchmark.gpuCaptureTag();
        require(tag != 0, "running capture tag");
        GpuFrameSample sample;
        sample.present[0] = sample.present[2] = true;
        sample.benchmarkTag = tag;
        for (uint64_t i = 1; i <= 100; ++i)
        {
            sample.serial = i;
            sample.ms[0] = static_cast<float>(i);
            sample.ms[2] = 2.f;
            benchmark.sampleGpu(sample);
            benchmark.sampleGpu(sample); // A delayed result must never be counted twice.
        }
        sample.serial = 101;
        sample.benchmarkTag = 0; // Warmup result arriving during measurement.
        benchmark.sampleGpu(sample);
        benchmark.tick(6., camera);
        const auto &report = benchmark.report();
        require(report.gpuSamples == 100, "deduplication and warmup exclusion");
        require(report.gpuAvgMs == 50.5f, "GPU average");
        require(report.gpuPercentilesAvailable && report.gpuP95Ms >= 94.f && report.gpuP99Ms >= 98.f, "GPU percentiles");
        require(report.gpuPasses[2].count == 100 && report.gpuPasses[2].totalMs == 200., "per-pass aggregation");
        require(report.gpuPasses[3].count == 0, "absent pass stays absent");

        benchmark.requestStart();
        benchmark.onWorldReady(glm::vec3(0.f));
        benchmark.tick(.001, camera);
        sample.benchmarkTag = tag; // Previous run's in-flight result.
        benchmark.sampleGpu(sample);
        benchmark.tick(6., camera);
        require(!benchmark.report().gpuAvailable && benchmark.report().gpuSamples == 0, "new run excludes old GPU results");
        require(benchmark.formatReportText().find("unavailable") != std::string::npos, "unavailable report");
        benchmark.requestStart();
        benchmark.onWorldReady(glm::vec3(0.f));
        benchmark.tick(.001, camera);
        sample.benchmarkTag = benchmark.gpuCaptureTag();
        benchmark.sampleGpu(sample);
        benchmark.tick(6., camera);
        require(benchmark.report().gpuAvailable && !benchmark.report().gpuPercentilesAvailable,
                "short capture keeps average but suppresses percentiles");
        benchmark.config().warmupSec = 2.f;
        benchmark.requestStart();
        benchmark.onWorldReady(glm::vec3(0.f));
        benchmark.tick(1., camera);
        require(benchmark.gpuCaptureTag() == 0, "warmup has no measurement tag");
        sample.benchmarkTag = 0;
        benchmark.sampleGpu(sample);
        benchmark.tick(1.1, camera);
        require(benchmark.gpuCaptureTag() != 0, "measurement tag after warmup");
        benchmark.sampleGpu(sample); // Late result from warmup.
        ++sample.serial;
        sample.benchmarkTag = benchmark.gpuCaptureTag();
        sample.ms[0] = 3.f;
        benchmark.sampleGpu(sample);
        benchmark.tick(6., camera);
        require(benchmark.report().gpuSamples == 1 && benchmark.report().gpuAvgMs == 3.f,
                "warmup samples excluded before and after measurement starts");

        // Queue-peak reset between runs (issue #173 review follow-up):
        // requestStart() is the authoritative reset boundary - a second run
        // must never inherit the first run's pending peaks, whichever way
        // the first run ended.
        auto sampleQueues = [&benchmark](size_t load, size_t gen, size_t mesh, size_t light) {
            benchmark.sampleFrame(16.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f,
                                  100, 10, load, gen, mesh, light,
                                  1, 1.f, 1, 1.f, 0, 0.f, 0, 0.f);
        };
        auto runToCompletion = [&benchmark, &camera]() {
            benchmark.config().warmupSec = 0.f; // earlier sections raise it; warmup 0 = straight to Running
            benchmark.requestStart();
            benchmark.onWorldReady(glm::vec3(0.f));
            benchmark.tick(.001, camera);
        };

        // Run #1: high queue peaks, completes normally.
        runToCompletion();
        sampleQueues(90, 8, 7, 8);
        benchmark.tick(6., camera);
        require(benchmark.report().peakPendingLoad == 90 && benchmark.report().peakPendingGen == 8 &&
                    benchmark.report().peakPendingMesh == 7 && benchmark.report().peakPendingLight == 8,
                "run #1 reports its own queue peaks");

        // Run #2: far lower peaks - none of run #1's values may survive.
        runToCompletion();
        sampleQueues(11, 1, 2, 2);
        benchmark.tick(6., camera);
        require(benchmark.report().peakPendingLoad == 11 && benchmark.report().peakPendingGen == 1 &&
                    benchmark.report().peakPendingMesh == 2 && benchmark.report().peakPendingLight == 2,
                "run #2 resets every queue peak");

        // cancel() -> requestStart(): an abandoned run must not leak either.
        runToCompletion();
        sampleQueues(90, 8, 7, 8);
        benchmark.cancel();
        runToCompletion();
        sampleQueues(11, 1, 2, 2);
        benchmark.tick(6., camera);
        require(benchmark.report().peakPendingLoad == 11 && benchmark.report().peakPendingGen == 1 &&
                    benchmark.report().peakPendingMesh == 2 && benchmark.report().peakPendingLight == 2,
                "cancel -> restart keeps no previous peak");

        // PoolGrow telemetry (review round 3): frames without growth are not
        // samples, active-step stats describe one growth operation, and the
        // frame contribution averages over EVERY measured frame.
        {
            auto sampleGrow = [&benchmark](float poolGrow) {
                benchmark.sampleFrame(16.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, poolGrow,
                                      1, 1, 0, 0, 0, 0, 0, 0.f, 0, 0.f, 0, 0.f, 0, 0.f);
            };
            auto runBenchmark = [&benchmark, &camera]() {
                benchmark.config().warmupSec = 0.f;
                benchmark.config().viewDistanceSwitch = 0;
                benchmark.requestStart();
                benchmark.onWorldReady(glm::vec3(0.f));
                benchmark.tick(.001, camera);
            };

            runBenchmark();
            sampleGrow(0.f); // a no-growth frame must not become a sample
            for (float grow : {0.4f, 0.6f, 0.8f, 1.2f})
                sampleGrow(grow);
            benchmark.tick(6., camera);
            const BenchmarkReport &r = benchmark.report();
            require(r.poolGrowSteps == 4, "zero-cost frames excluded from PoolGrow samples");
            require(std::abs(r.avgPoolGrow - 0.75f) < 1e-5f, "PoolGrow active average (0.4+0.6+0.8+1.2)/4");
            // percentileOfSorted nearest-index rule: sorted[ (size_t)(0.95*3) ] = 0.8.
            require(std::abs(r.p95PoolGrow - 0.8f) < 1e-5f, "PoolGrow active p95");
            require(std::abs(r.maxPoolGrow - 1.2f) < 1e-5f, "PoolGrow active max");
            require(std::abs(r.avgPoolGrowFrameMs - 3.0f / 5.f) < 1e-5f,
                    "PoolGrow frame contribution averages over all 5 frames");
            require(benchmark.formatReportText().find("PoolGrow") != std::string::npos,
                    "report surfaces the PoolGrow line when growth happened");

            // A run with zero growth reports no steps and no PoolGrow line —
            // all-zero averages must not masquerade as measurements.
            runBenchmark();
            sampleGrow(0.f);
            benchmark.tick(6., camera);
            require(benchmark.report().poolGrowSteps == 0 && benchmark.report().avgPoolGrow == 0.f,
                    "growth-free run reports no PoolGrow steps");
            require(benchmark.formatReportText().find("PoolGrow") == std::string::npos,
                    "growth-free report omits the PoolGrow line");

            // View-distance switch, applied downward: the Engine mirrors
            // tickBenchmark() — run with a configured switch, drive past the
            // 25% switch point, consume, then finish. The report must record
            // the APPLIED switch and explain the absence of growth, never
            // fabricate PoolGrow metrics.
            {
                benchmark.config().warmupSec = 0.f;
                benchmark.config().durationSec = 5.f;
                benchmark.config().viewDistanceSwitch = 256;
                benchmark.requestStart();
                benchmark.onWorldReady(glm::vec3(0.f));
                benchmark.tick(1.3, camera); // 26% of measurement: past the switch point
                int target = 0;
                require(benchmark.consumeViewDistanceSwitch(target),
                        "view-distance switch fires after 25%");
                require(target == 256, "view-distance switch returns configured target");
                int secondTarget = 0;
                require(!benchmark.consumeViewDistanceSwitch(secondTarget),
                        "view-distance switch fires only once");
                sampleGrow(0.f);
                benchmark.tick(4.f, camera);
                const BenchmarkReport &r = benchmark.report();
                require(r.viewDistanceSwitch == 256, "report records actually applied switch");
                require(r.poolGrowSteps == 0, "downward switch causes no pool growth");
                const std::string text = benchmark.formatReportText();
                require(text.find("View switch -> 256") != std::string::npos,
                        "report shows applied switch");
                require(text.find("No ChunkPool growth observed") != std::string::npos,
                        "report explains absence of growth");
                require(text.find("PoolGrow avg") == std::string::npos,
                        "no fabricated PoolGrow metrics");
            }

            // Applied switch WITH growth samples: switch line + PoolGrow
            // stats, and no "no growth" caveat.
            {
                benchmark.config().warmupSec = 0.f;
                benchmark.config().durationSec = 5.f;
                benchmark.config().viewDistanceSwitch = 1024;
                benchmark.requestStart();
                benchmark.onWorldReady(glm::vec3(0.f));
                benchmark.tick(1.3, camera);
                int target = 0;
                require(benchmark.consumeViewDistanceSwitch(target) && target == 1024,
                        "upward switch fires at the switch point");
                sampleGrow(0.5f);
                benchmark.tick(4.f, camera);
                const BenchmarkReport &g = benchmark.report();
                require(g.viewDistanceSwitch == 1024 && g.poolGrowSteps == 1,
                        "applied switch + growth samples are both reported");
                const std::string text = benchmark.formatReportText();
                require(text.find("View switch -> 1024") != std::string::npos,
                        "upward switch reported");
                require(text.find("PoolGrow avg") != std::string::npos,
                        "growth samples reported alongside the applied switch");
                require(text.find("No ChunkPool growth observed") == std::string::npos,
                        "no no-growth caveat when growth happened");
            }

            // Configured but NEVER fired (run ends before the switch point,
            // or cancel): the report must not claim a switch at all — this is
            // the exact regression the applied-vs-configured split fixes.
            {
                benchmark.config().warmupSec = 0.f;
                benchmark.config().durationSec = 5.f;
                benchmark.config().viewDistanceSwitch = 1024;
                benchmark.requestStart();
                benchmark.onWorldReady(glm::vec3(0.f));
                sampleGrow(0.f);
                benchmark.tick(6., camera); // finalize without ever consuming the switch
                require(benchmark.report().viewDistanceSwitch == 0,
                        "configured but unapplied switch is not reported");
                require(benchmark.formatReportText().find("View switch ->") == std::string::npos,
                        "report does not claim an unapplied switch");
            }
        }

        std::cout << "PASS: GPU conversion and benchmark capture isolation\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
