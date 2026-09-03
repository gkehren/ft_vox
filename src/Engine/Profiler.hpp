#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

/// Lightweight hierarchical CPU profiler for the main thread + aggregate worker samples.
/// Zero external deps; designed for an ImGui panel (F7).
///
/// Usage:
///   GetProfiler().beginFrame();
///   { PROFILE_SCOPE("Streaming"); ... }
///   GetProfiler().endFrame();
///
/// UI should read lastFrame() (previous completed frame). Capture runs even when the
/// panel is closed so opening F7 has history; use setEnabled(false) / Pause in UI.

#define FT_VOX_CONCAT_INNER(a, b) a##b
#define FT_VOX_CONCAT(a, b) FT_VOX_CONCAT_INNER(a, b)
#define PROFILE_SCOPE(name) ::ProfileScope FT_VOX_CONCAT(_ftVoxProf_, __LINE__)(name)
#define PROFILE_FUNCTION() PROFILE_SCOPE(__func__)

#include <mutex>

struct ProfileEntry
{
	const char *name{nullptr};
	float startMs{0.f};
	float durationMs{0.f};
	uint8_t depth{0};
	int16_t parent{-1};
};

struct WorkerBucket
{
	const char *name{nullptr};
	mutable std::mutex mutex;
	uint64_t count{0};
	uint64_t totalUs{0}; // microseconds for sub-millisecond precision

	WorkerBucket() = default;
	WorkerBucket(const WorkerBucket &o)
	{
		std::lock_guard<std::mutex> lock(o.mutex);
		name = o.name;
		count = o.count;
		totalUs = o.totalUs;
	}
	WorkerBucket &operator=(const WorkerBucket &o)
	{
		if (this != &o)
		{
			std::scoped_lock lock(mutex, o.mutex);
			name = o.name;
			count = o.count;
			totalUs = o.totalUs;
		}
		return *this;
	}
};

struct WorkerSnapshot
{
	const char *name{nullptr};
	uint64_t count{0};
	uint64_t totalUs{0};
	float totalMs{0.f};
	float avgMs{0.f};
};

struct SpikeRecord
{
	float frameMs{0.f};
	const char *topScope{nullptr};
	float topMs{0.f};
};

class Profiler
{
public:
	static constexpr int kMaxEntries = 96;
	static constexpr int kMaxDepth = 8;
	static constexpr int kHistorySize = 240;
	static constexpr int kMaxWorkerBuckets = 8;
	static constexpr int kSpikeLogSize = 8;
	static constexpr float kSpikeThresholdMs = 20.f;

	Profiler();
	Profiler(const Profiler &) = delete;
	Profiler &operator=(const Profiler &) = delete;
	Profiler(Profiler &&) = delete;
	Profiler &operator=(Profiler &&) = delete;

	void setEnabled(bool v) { m_enabled.store(v, std::memory_order_relaxed); }
	bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }

	void beginFrame();
	void endFrame();

	/// Main-thread only. String literals recommended (name pointer identity).
	void push(const char *name);
	void pop();

	/// Thread-safe aggregate samples from worker threads.
	void addWorkerSample(const char *name, float ms);

	/// Ensure a worker name slot exists before workers sample it.
	void registerWorkerName(const char *name);

	/// Frame-consistent snapshot of worker thread aggregates.
	void snapshotWorkers();

	void clearHistory();

	// --- UI accessors (read last completed frame) ---

	float lastFrameMs() const { return m_lastFrameMs; }
	float avgFrameMs() const { return m_avgFrameMs; }
	float fpsEstimate() const { return m_avgFrameMs > 1e-4f ? 1000.f / m_avgFrameMs : 0.f; }
	float onePercentLowMs() const { return m_onePercentLowMs; }

	const ProfileEntry *lastEntries() const { return m_lastEntries.data(); }
	int lastEntryCount() const { return m_lastEntryCount; }

	const float *frameHistory() const { return m_history.data(); }
	int historyCount() const { return m_historyCount; }
	int historyWriteIndex() const { return m_historyWrite; } // next slot (oldest if full)

	int workerSnapshotCount() const { return m_workerSnapCount; }
	const WorkerSnapshot *workerSnapshots() const { return m_workerSnaps.data(); }

	int spikeCount() const { return m_spikeCount; }
	const SpikeRecord *spikes() const { return m_spikes.data(); }

	/// Find duration of a named scope in the last frame (first match). 0 if missing.
	float lastScopeMs(const char *name) const;

private:
	using Clock = std::chrono::steady_clock;

	float nowMs() const
	{
		return std::chrono::duration<float, std::milli>(Clock::now() - m_frameStart).count();
	}

	void openScope(const char *name);
	void closeScope();
	void finalizeFrame(float frameMs);
	void updateAverages(float frameMs);
	void recordSpike(float frameMs);

	std::atomic<bool> m_enabled{true};
	bool m_inFrame{false};

	Clock::time_point m_frameStart{};

	// Building current frame
	std::array<ProfileEntry, kMaxEntries> m_entries{};
	int m_entryCount{0};
	std::array<int16_t, kMaxDepth> m_stack{};
	int m_stackDepth{0};

	// Last completed frame (UI)
	std::array<ProfileEntry, kMaxEntries> m_lastEntries{};
	int m_lastEntryCount{0};
	float m_lastFrameMs{0.f};

	// History ring
	std::array<float, kHistorySize> m_history{};
	int m_historyWrite{0};
	int m_historyCount{0};

	// Rolling average (~1s EMA-ish via simple sum window)
	float m_avgFrameMs{0.f};
	float m_onePercentLowMs{0.f};
	double m_avgAccumMs{0.0};
	int m_avgSamples{0};

	// Worker aggregates
	std::array<WorkerBucket, kMaxWorkerBuckets> m_workers{};
	std::atomic<int> m_workerBucketCount{0};
	std::array<WorkerSnapshot, kMaxWorkerBuckets> m_workerSnaps{};
	int m_workerSnapCount{0};
	std::mutex m_workerRegisterMutex;

	// Spikes (newest at end)
	std::array<SpikeRecord, kSpikeLogSize> m_spikes{};
	int m_spikeCount{0};
};

Profiler &GetProfiler();

/// RAII scope. No-op when profiler disabled or not in a frame.
class ProfileScope
{
public:
	explicit ProfileScope(const char *name) : m_active(GetProfiler().enabled())
	{
		if (m_active)
			GetProfiler().push(name);
	}
	~ProfileScope()
	{
		if (m_active)
			GetProfiler().pop();
	}
	ProfileScope(const ProfileScope &) = delete;
	ProfileScope &operator=(const ProfileScope &) = delete;

private:
	bool m_active{false};
};
