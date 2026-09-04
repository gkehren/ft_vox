#pragma once

#include <Camera/Camera.hpp>
#include <Engine/GpuProfile.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <array>

enum class BenchmarkPhase
{
	Idle,
	Reloading,
	Warmup,
	Running,
	Done
};

struct BenchmarkConfig
{
	int seed{42};
	float durationSec{60.f};
	float warmupSec{3.f};
	int pathOrbits{2};
	float pathRadius{160.f};
	float pathHeight{48.f}; // above surface Y at center
	bool forceVsyncOff{false};
	float biomeMapZoom{0.f}; // >0 opens a fixed-center map during the benchmark
	bool biomeMapSequential{false}; // control measurement of the previous path
};

struct BenchmarkWorkTiming
{
	uint64_t count{0};
	double totalMs{0};
};

struct BenchmarkReport
{
	bool valid{false};
	bool gpuAvailable{false}, gpuPercentilesAvailable{false};
	uint64_t gpuSamples{0};
	float gpuAvgMs{0.f}, gpuP95Ms{0.f}, gpuP99Ms{0.f};
	std::array<BenchmarkWorkTiming, kGpuPassCount> gpuPasses{};
	std::array<BenchmarkWorkTiming, 3> backgroundWork{};
	float biomeMapZoom{0.f};
	bool biomeMapSequential{false};

	int seed{0};
	float durationSec{0.f};
	float warmupSec{0.f};
	float measuredSec{0.f};
	int frames{0};

	float avgMs{0.f};
	float minMs{0.f};
	float maxMs{0.f};
	float p50Ms{0.f};
	float p95Ms{0.f};
	float p99Ms{0.f};
	float p1LowMs{0.f}; // ~99th percentile frame time (slow frames)
	float avgFps{0.f};
	float onePercentLowFps{0.f};

	float avgStreaming{0.f};
	float avgAcquire{0.f};
	float avgRecord{0.f};
	float avgImGui{0.f};
	float avgPresent{0.f};
	float avgVisibility{0.f};
	float avgMeshUpload{0.f};

	uint64_t terrainGenJobs{0};
	float terrainGenTotalMs{0.f};
	float terrainGenAvgMs{0.f};
	uint64_t meshBuildJobs{0};
	float meshBuildTotalMs{0.f};
	float meshBuildAvgMs{0.f};
	uint64_t meshLodJobs{0};
	float meshLodTotalMs{0.f};
	float meshLodAvgMs{0.f};

	size_t peakChunks{0};
	size_t peakDraw{0};
	size_t peakPendingLoad{0};
	size_t peakPendingGen{0};
	size_t peakPendingMesh{0};

	int framesOver16ms{0};
	int framesOver33ms{0};

	int viewDistance{0};
	int windowW{0};
	int windowH{0};
	bool vsync{false};
	std::string presentMode;
	std::string deviceName;

	/// Build-time code identity (git hash, dirty flag, branch, build UTC).
	std::string gitHash;
	std::string gitBranch;
	std::string gitDescribe;
	std::string revisionLabel; // short hash + optional '*'
	std::string buildUtc;
	bool gitDirty{false};

	int score{0}; // 0–10000
	char grade{'F'};
};

/// Scripted orbit benchmark: reload world, fly path, aggregate metrics, score.
class Benchmark
{
public:
	BenchmarkConfig &config() { return m_config; }
	const BenchmarkConfig &config() const { return m_config; }

	BenchmarkPhase phase() const { return m_phase; }
	bool isActive() const
	{
		return m_phase == BenchmarkPhase::Reloading || m_phase == BenchmarkPhase::Warmup ||
			   m_phase == BenchmarkPhase::Running;
	}
	bool locksInput() const
	{
		return m_phase == BenchmarkPhase::Warmup || m_phase == BenchmarkPhase::Running;
	}
	bool showReport() const { return m_showReport; }
	void setShowReport(bool v) { m_showReport = v; }

	const BenchmarkReport &report() const { return m_report; }

	/// Request start (Engine will reload on Reloading phase).
	void requestStart();
	void cancel();

	/// Called by Engine after reloadWorld succeeds.
	void onWorldReady(const glm::vec3 &surfaceCenter);

	/// Drive camera along path; advance timers. Call every frame while active (after reload).
	void tick(double dt, Camera &camera);

	/// After profiler endFrame: record sample if in Running (post-warmup).
	void sampleFrame(float frameMs, float scopeStreaming, float scopeAcquire, float scopeRecord,
					 float scopeImGui, float scopePresent, float scopeVisibility, float scopeMeshUpload,
					 size_t chunks, size_t drawCount, size_t pendingLoad, size_t pendingGen,
					 size_t pendingMesh, uint64_t terrainJobs, float terrainMs, uint64_t meshJobs,
					 float meshMs, uint64_t lodJobs, float lodMs);

	/// Queue waits and completed map latency from the frame's profiler snapshot.
	void sampleBackgroundWork(const char *name, uint64_t count, double totalMs);
	/// Zero outside measurement; tags exclude delayed warmup/previous-run results.
	uint64_t gpuCaptureTag() const { return m_phase == BenchmarkPhase::Running ? m_gpuTag : 0; }
	void sampleGpu(const GpuFrameSample &sample);
	/// Progress 0–1 over warmup+duration total wall time for UI bar (full run).
	float totalProgress() const;
	/// Progress 0–1 of measurement only (warmup excluded).
	float measureProgress() const;
	float elapsedSec() const { return m_elapsed; }
	float remainingMeasureSec() const;

	/// Build multi-line text for clipboard / file.
	std::string formatReportText() const;

	/// Write summary next to the executable (or cwd). Returns path on success, empty on failure.
	std::string saveReportToFile(const std::string &directory = {}) const;

	/// Path of the last successful save (empty if never saved).
	const std::string &lastSavedPath() const { return m_lastSavedPath; }

private:
	void finalize();
	void applyCamera(Camera &camera, float t01) const;
	static float percentileSorted(std::vector<float> &sorted, float p01);
	static int computeScore(const BenchmarkReport &r);
	static char gradeForScore(int score);

	BenchmarkConfig m_config{};
	BenchmarkPhase m_phase{BenchmarkPhase::Idle};
	bool m_showReport{false};

	float m_elapsed{0.f}; // wall time since world ready
	glm::vec3 m_center{0.f};
	float m_surfaceY{80.f};

	// Saved VSync to restore after run
	bool m_hadForceVsync{false};
	bool m_prevVsync{true};
	bool m_vsyncRestorePending{false};

public:
	// Engine applies VSync toggle via these
	bool consumeForceVsyncOff(bool &outPrev) 
	{
		if (!m_hadForceVsync)
			return false;
		m_hadForceVsync = false;
		outPrev = m_prevVsync;
		return true;
	}
	void setPrevVsync(bool v) { m_prevVsync = v; }
	bool needsVsyncRestore() const { return m_vsyncRestorePending; }
	bool prevVsync() const { return m_prevVsync; }
	void clearVsyncRestore() { m_vsyncRestorePending = false; }

private:
	std::array<BenchmarkWorkTiming, 3> m_backgroundWork{};
	uint64_t m_gpuTag{0}, m_lastGpuSerial{0};
	std::vector<float> m_gpuFrames;
	std::array<BenchmarkWorkTiming, kGpuPassCount> m_gpuPasses{};
	std::vector<float> m_frameMs;
	double m_sumStreaming{0}, m_sumAcquire{0}, m_sumRecord{0}, m_sumImGui{0}, m_sumPresent{0};
	double m_sumVisibility{0}, m_sumMeshUpload{0};

	uint64_t m_terrainJobs{0};
	double m_terrainMs{0};
	uint64_t m_meshJobs{0};
	double m_meshMs{0};
	uint64_t m_lodJobs{0};
	double m_lodMs{0};

	size_t m_peakChunks{0}, m_peakDraw{0}, m_peakLoad{0}, m_peakGen{0}, m_peakMesh{0};
	int m_over16{0}, m_over33{0};

	// Settings snapshotted at start of measurement
	int m_viewDistance{0};
	int m_windowW{0}, m_windowH{0};
	bool m_vsync{false};
	std::string m_presentMode;
	std::string m_deviceName;

public:
	void setSettingsSnapshot(int viewDist, int w, int h, bool vsync,
							 const char *presentMode, const char *device);
	void markForceVsync(bool prevVsync)
	{
		m_hadForceVsync = true;
		m_prevVsync = prevVsync;
		m_vsyncRestorePending = true;
	}

private:
	BenchmarkReport m_report{};
	mutable std::string m_lastSavedPath;
};
