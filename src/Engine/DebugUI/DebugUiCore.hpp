#pragma once

// Developer-console state that is independent of ImGui rendering (issue
// #179): snapshot value types, bounded metric histories, unit formatting and
// sustained-condition health logic. Kept free of engine/ImGui includes so
// the aggregation helpers are unit-testable without Vulkan or a window.
//
// Data flow: Engine fills GameUIFrame once per frame (as before);
// updateDebugUiState() (DebugUiEngine.hpp, defined in DebugUiCore.cpp)
// samples it into the snapshots below at a throttled rate, and the domain
// panels (DebugPanels.hpp) render from UiState. Panels keep direct access to
// the settings structs (RenderSettings / ShaderParameters /
// PostProcessSettings) for mutable tuning controls — those are the engine's
// settings API, not debug state.

#include <Engine/WorkloadTelemetry.hpp>
#include <Engine/GameUIResourcePack.hpp>
#include <Engine/PlayerUi.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <glm/glm.hpp>

namespace debugui
{

// --- Formatting (human-readable KiB/MiB/GiB, etc.) ---

/// 0 -> "0 B", 1023 -> "1023 B", 1536 -> "1.5 KiB", 1048576 -> "1.0 MiB" ...
std::string formatBytes(uint64_t bytes);
/// Count with the largest whole unit that fits: "512", "1.2k", "3.4M".
std::string formatCount(uint64_t n);
/// Milliseconds with fixed precision ("1.42", "0.031" under 1 ms).
std::string formatMs(float ms);

// --- Bounded metric history (fixed storage after init, chronological read) ---

constexpr size_t kMetricHistorySize = 256;

class MetricHistory
{
public:
	void push(float v);
	void clear() { m_count = 0; }
	size_t count() const { return m_count; }
	/// Chronological (oldest first) view for plotting; empty when no sample.
	const float *data() const { return m_samples.data(); }
	float latest() const { return m_count ? m_samples[(m_write + kMetricHistorySize - 1) % kMetricHistorySize] : 0.f; }
	/// Value from roughly `back` samples ago (0 = latest), clamped.
	float back(size_t backIndex) const;
	/// Copy samples oldest-first into `dst` (at least kMetricHistorySize).
	void copyOrdered(float *dst) const;

private:
	std::array<float, kMetricHistorySize> m_samples{};
	size_t m_write{0};
	size_t m_count{0};
};

// --- Sustained-condition health monitors (no one-frame-transient alarms) ---

/// Flags a warning only after its condition held true for enterSeconds
/// continuously, and clears it only after the condition held false for
/// exitSeconds (hysteresis prevents flapping at a threshold boundary).
/// Recent-event warnings use enterSeconds = 0 with the caller passing
/// `condition = event within the last N seconds`.
class HealthMonitor
{
public:
	HealthMonitor(float enterSeconds, float exitSeconds);
	/// Feed the current condition with the elapsed time since the last call.
	/// Returns true while the warning is raised.
	bool update(bool condition, float dtSeconds);
	bool active() const { return m_active; }
	float conditionSeconds() const { return m_conditionSeconds; }
	void reset();

private:
	float m_enterSeconds;
	float m_exitSeconds;
	float m_conditionSeconds{0.f}; // continuously true
	float m_clearSeconds{0.f};	   // continuously false
	bool m_active{false};
};

// --- Debug snapshots (read-only value copies consumed by panels) ---

struct FrameDebugSnapshot
{
	float fps{0.f};
	/// Hierarchical-profiler frame time (events → present), NOT the paced
	/// simulation delta — they diverge whenever pacing clamps dt or a stall
	/// is absorbed by the frame clock (issue #179 review).
	float cpuFrameMs{0.f};
	/// Paced simulation delta fed to gameplay ticks, kept separate so the
	/// two can never be conflated again.
	float simulationDtMs{0.f};
	float avgMs{0.f};
	float onePercentLowMs{0.f};
	float gpuFrameMs{0.f};
	bool gpuValid{false};
	bool vsync{false};
	const char *presentMode{""};
};

struct StreamingDebugSnapshot
{
	size_t loadedChunks{0};
	size_t drawCount{0};
	size_t pendingLoad{0};
	size_t pendingGen{0};
	size_t pendingMesh{0};
	size_t pendingLight{0};
	size_t uploadBacklog{0};	// active chunks with a staged mesh awaiting GPU copy
	size_t deferredReleases{0}; // chunks waiting out the frames-in-flight delay
	int viewDistance{0};
	int nearRange{0};
	size_t poolCapacity{0};
	size_t poolFree{0};
	size_t poolAcquired{0};
	size_t poolRejects{0};
	size_t poolGrows{0};
	uint64_t meshJobsDispatched{0};
	uint64_t lightJobsDispatched{0};
};

/// Memory/workload groups, sourced from telemetry::Registry::sampleLive()
/// plus main-thread pool/arena getters. All byte values are live "current"
/// unless the field name says peak.
struct MemoryDebugSnapshot
{
	bool telemetryEnabled{false};
	telemetry::Registry::LiveSnapshot live{};
	double sampledAt{0.0};

	// Main-thread direct samples (not from the registry).
	size_t arenaPages{0};
	uint64_t arenaFreeBytes{0};
	uint64_t arenaHighWaterBytes{0};
	uint64_t arenaLiveBytes{0};
	uint64_t stagingUsedBytes{0};
	uint64_t stagingCapacityBytes{0};
};

struct ChunkDebugSnapshot
{
	bool valid{false};
	glm::ivec3 coord{0, 0, 0};
	float distance{0.f};
	bool loaded{false}; // present in the ChunkManager map
	// Chunk state (valid when loaded).
	int state{0}; // ChunkState ordinal
	uint64_t meshGeneration{0};
	uint64_t meshRevision{0};
	bool lodMesh{false};
	bool inTransit{false};
	bool uploadPending{false};
	bool hasPendingMeshResult{false};
	bool unuploadedFullMesh{false};
	uint16_t dirtySections{0};
	bool lightCacheWanted{false};
	bool lightCachePresent{false};
	bool visible{false};
	uint32_t opaqueDrawCount{0};
	uint32_t waterDrawCount{0};
	uint32_t opaqueIndexCount{0};
	uint32_t waterIndexCount{0};
	uint32_t liveGpuSections{0};
	uint32_t activeIndex{0};
};

/// Player/physics diagnostics (issue #184): the raw solver counters and
/// motion flags removed from the gameplay surfaces. Copied from the frame's
/// playerui::PlayerSnapshot while the Player Diagnostics window is open —
/// including the camera view state, so the panel stays snapshot-only (issue
/// #184 review) and never reads the Camera directly.
struct PlayerDebugSnapshot
{
	bool valid{false};
	bool flight{false};
	bool grounded{false};
	bool swimming{false};
	bool submergedWater{false};
	bool submergedLava{false};
	bool waitingForTerrain{false};
	float speed{0.f};
	glm::vec3 position{0.f};
	float yaw{0.f};
	float pitch{0.f};
	uint32_t physicsSteps{0};
	uint64_t queriedCells{0};
	uint64_t queryIterations{0};
	uint64_t droppedSteps{0};
	playerui::CameraViewMode cameraViewMode{playerui::CameraViewMode::Perspective};
	float cameraMovementSpeed{0.f};
	float mouseSensitivity{0.f};
	float isometricZoom{0.f};
};

// --- Console state owned by GameUI ---

struct PanelState
{
	/// Read-only Status Overlay density (issue #184): F1 cycles, the View
	/// menu offers the same three states. Replaces the old catch-all HUD.
	playerui::StatusOverlayDensity statusOverlay{playerui::StatusOverlayDensity::Detailed};
	/// Interactive Player / Gameplay panel (View menu).
	bool playerPanel{false};
	bool overview{false};
	bool rendering{false};	   // settings ("Graphics")
	bool renderDebug{false};   // diagnostic views
	bool streaming{false};
	bool performance{false};   // CPU/GPU profiler (was "Profiler")
	bool playerDiagnostics{false}; // raw player/physics solver counters
	bool chunkInspector{false};
	bool memory{false};
	bool benchmark{false};
	bool world{false};
	bool help{false};
	bool overlayHints{true};
};

/// Per-scope CPU statistics maintained while the Performance panel is open.
/// Keyed by the profiler's static scope name pointer, so the table stays
/// bounded by the number of distinct scopes, not by time.
struct ScopeStats
{
	const char *name{nullptr};
	float lastMs{0.f};
	float avgMs{0.f};
	float peakMs{0.f};
	uint64_t frames{0};
	uint32_t depth{0};
	/// 10 Hz-ish history (pushed per sampled frame) for click-to-plot.
	MetricHistory history;
};

static constexpr size_t kMaxScopeStats = 96;

/// Heavy-sampled health conditions. Each monitor only raises after the
/// underlying condition has been sustained (see HealthMonitor); thresholds
/// are rooted in engine constants (see .cpp comments).
struct DebugHealth
{
	HealthMonitor poolRejects{2.0f, 6.0f};
	HealthMonitor stagingFailures{0.0f, 4.0f}; // any event within a 2 s window keeps it raised
	HealthMonitor meshBacklog{3.0f, 6.0f};
	HealthMonitor lightBacklog{3.0f, 6.0f};
	HealthMonitor uploadBacklog{5.0f, 5.0f};
	HealthMonitor retiredBacklog{3.0f, 6.0f};
	HealthMonitor gpuMemoryGrowth{5.0f, 10.0f};
	HealthMonitor slowFrames{2.0f, 8.0f};

	/// Advance only the monitors fed by the streaming-domain snapshot (pool
	/// rejects + queue backlogs; issue #186). Called from the throttled
	/// sampler when the primary Streaming panel is open without Overview, so
	/// it can show sustained warnings without sampling the memory domain.
	/// Memory-derived monitors (staging failures, retired backlog, GPU
	/// growth) are deliberately NOT touched here: their timestamps/histories
	/// come from the memory sample and would advance on stale data.
	void updateStreaming(const class UiState &state, float dt);

	/// Full refresh: streaming monitors + memory/frame monitors (Overview
	/// path). The streaming monitors are advanced exactly once here too, so
	/// callers must not run both entries on the same sample.
	void update(const class UiState &state, float dt);
};

/// Per-domain snapshot container + bounded histories. Owned by GameUI;
/// refreshed by updateDebugUiState().
class UiState
{
public:
	PanelState panels;

	FrameDebugSnapshot frame;
	StreamingDebugSnapshot streaming;
	MemoryDebugSnapshot memory;
	PlayerDebugSnapshot player;

	// 10 Hz bounded histories (sampled only while a consumer panel is open).
	MetricHistory cpuMs;
	MetricHistory gpuMs;
	MetricHistory pendingMesh;
	MetricHistory pendingLoad;
	MetricHistory pendingGen;
	MetricHistory pendingLight;
	MetricHistory uploadBacklog;
	MetricHistory activeChunks;
	MetricHistory gpuLiveBytes;
	MetricHistory retiredBytes;
	MetricHistory stagingUsed;
	MetricHistory arenaUtilization;

	DebugHealth health;

	// Scope table for the Performance panel (bounded by kMaxScopeStats).
	std::array<ScopeStats, kMaxScopeStats> scopeStats{};
	size_t scopeStatCount{0};
	int selectedScopeGraph{-1}; // row highlighted/plotted, -1 = none
	/// Profiler capture epoch at the last scope-stat sample. A change
	/// (Clear history button, world reload, any Profiler::clearHistory)
	/// resets the accumulated table so avg/peak/history never blend two
	/// capture windows.
	uint64_t lastProfilerEpoch{0};

	// Chunk inspector selection (chunk coordinates; Y is always 0 today).
	glm::ivec3 inspectCoord{0, 0, 0};
	bool inspectHasTarget{false};
	bool eventTraceEnabled{false}; // mirrors ChunkManager's opt-in trace

	// Previous telemetry sample, for "since last refresh" event deltas.
	bool hasPrevEvents{false};
	std::array<uint64_t, telemetry::EventCount> prevEvents{};
	std::array<uint64_t, telemetry::EventCount> eventDelta{};
	double lastTelemetrySample{-1.0};
	double lastHistorySample{-1.0};
	float dtSinceLastSample{0.f};

	// Recent-event health bookkeeping (cumulative counters -> last-seen time).
	size_t prevPoolRejects{0};
	double lastPoolRejectIncrease{-100.0};
	double lastStagingFailureAt{-100.0};

	// Graphics panel navigation (issue #185): selected category; persists for
	// the session.
	int graphicsCategory{0};

	// Resource-pack browser state (Graphics panel section + file dialog).
	ResourcePackUiState resourcePackUi{};
};

/// Drop the accumulated CPU scope table (call on a profiler capture-epoch
/// change: Clear history, world reload). Also clears the plot selection.
inline void resetScopeStats(UiState &state)
{
	state.scopeStats = {};
	state.scopeStatCount = 0;
	state.selectedScopeGraph = -1;
}

} // namespace debugui
