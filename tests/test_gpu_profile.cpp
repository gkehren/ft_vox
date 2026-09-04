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
        require(gpuTimestampSupported(true, 64, 1.f), "64-bit support");
        require(!gpuTimestampSupported(false, 64, 1.f), "device capability gate");
        require(!gpuTimestampSupported(true, 0, 1.f), "queue capability gate");
        require(!gpuTimestampSupported(true, 65, 1.f), "invalid bit count");
        require(!gpuTimestampSupported(true, 64, NAN), "invalid period");
        require(!gpuTimestampSupported(true, 64, 0.f), "zero period");
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
        std::cout << "PASS: GPU conversion and benchmark capture isolation\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
