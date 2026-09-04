#include "Engine/Benchmark.hpp"
#include "Engine/BuildInfo.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <cstring>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

static constexpr const char *kBackgroundNames[] = {"TerrainQueue", "MeshQueue", "BiomeMap"};

void Benchmark::requestStart()
{
	if (isActive())
		return;
	m_phase = BenchmarkPhase::Reloading;
	m_elapsed = 0.f;
	m_showReport = false;
	m_report = {};
	m_backgroundWork = {};
	m_frameMs.clear();
	m_sumStreaming = m_sumAcquire = m_sumRecord = m_sumImGui = m_sumPresent = 0;
	m_sumVisibility = m_sumMeshUpload = 0;
	m_terrainJobs = m_meshJobs = m_lodJobs = 0;
	m_terrainMs = m_meshMs = m_lodMs = 0;
	m_peakChunks = m_peakDraw = m_peakLoad = m_peakGen = m_peakMesh = 0;
	m_over16 = m_over33 = 0;
	const float dur = std::clamp(m_config.durationSec, 5.f, 300.f);
	m_config.durationSec = dur;
	m_config.warmupSec = std::clamp(m_config.warmupSec, 0.f, dur);
	const size_t est = static_cast<size_t>(std::ceil((dur + 5.f) * 120.f));
	m_frameMs.reserve(est);
}

void Benchmark::cancel()
{
	if (m_phase == BenchmarkPhase::Idle)
		return;
	m_phase = BenchmarkPhase::Idle;
	m_elapsed = 0.f;
	// Keep vsync restore flag if we forced it
}

void Benchmark::onWorldReady(const glm::vec3 &surfaceCenter)
{
	m_center = surfaceCenter;
	m_surfaceY = surfaceCenter.y;
	m_elapsed = 0.f;
	m_phase = BenchmarkPhase::Warmup;
	if (m_config.warmupSec <= 0.f)
		m_phase = BenchmarkPhase::Running;
}

void Benchmark::setSettingsSnapshot(int viewDist, int w, int h, bool vsync,
									const char *presentMode,
									const char *device)
{
	m_viewDistance = viewDist;
	m_windowW = w;
	m_windowH = h;
	m_vsync = vsync;
	m_presentMode = presentMode ? presentMode : "";
	m_deviceName = device ? device : "";
}

void Benchmark::applyCamera(Camera &camera, float t01) const
{
	const float t = std::clamp(t01, 0.f, 1.f);
	const float orbits = static_cast<float>(std::max(1, m_config.pathOrbits));
	const float angle = t * orbits * 6.28318530718f;
	const float R = m_config.pathRadius;
	const float y = m_surfaceY + m_config.pathHeight;

	const glm::vec3 pos(m_center.x + R * std::cos(angle), y, m_center.z + R * std::sin(angle));
	camera.setPosition(pos);
	camera.setMode(CameraMode::PERSPECTIVE);

	const glm::vec3 lookAt(m_center.x, m_surfaceY + 8.f, m_center.z);
	const glm::vec3 dir = glm::normalize(lookAt - pos);
	const float yaw = glm::degrees(std::atan2(dir.z, dir.x));
	const float pitch = glm::degrees(std::asin(std::clamp(dir.y, -1.f, 1.f)));
	camera.setYawPitch(yaw, pitch);
}

void Benchmark::tick(double dt, Camera &camera)
{
	if (m_phase != BenchmarkPhase::Warmup && m_phase != BenchmarkPhase::Running)
		return;

	m_elapsed += static_cast<float>(dt);

	const float totalPath = m_config.warmupSec + m_config.durationSec;
	const float t01 = totalPath > 1e-4f ? std::clamp(m_elapsed / totalPath, 0.f, 1.f) : 1.f;
	applyCamera(camera, t01);

	if (m_phase == BenchmarkPhase::Warmup && m_elapsed >= m_config.warmupSec)
		m_phase = BenchmarkPhase::Running;

	if (m_phase == BenchmarkPhase::Running &&
		m_elapsed >= m_config.warmupSec + m_config.durationSec)
	{
		finalize();
		m_phase = BenchmarkPhase::Done;
		m_showReport = true;
	}
}

void Benchmark::sampleFrame(float frameMs, float scopeStreaming, float scopeAcquire, float scopeRecord,
							float scopeImGui, float scopePresent, float scopeVisibility,
							float scopeMeshUpload, size_t chunks, size_t drawCount, size_t pendingLoad,
							size_t pendingGen, size_t pendingMesh, uint64_t terrainJobs, float terrainMs,
							uint64_t meshJobs, float meshMs, uint64_t lodJobs, float lodMs)
{
	if (m_phase != BenchmarkPhase::Running)
		return;

	m_frameMs.push_back(frameMs);
	m_sumStreaming += scopeStreaming;
	m_sumAcquire += scopeAcquire;
	m_sumRecord += scopeRecord;
	m_sumImGui += scopeImGui;
	m_sumPresent += scopePresent;
	m_sumVisibility += scopeVisibility;
	m_sumMeshUpload += scopeMeshUpload;

	m_terrainJobs += terrainJobs;
	m_terrainMs += terrainMs;
	m_meshJobs += meshJobs;
	m_meshMs += meshMs;
	m_lodJobs += lodJobs;
	m_lodMs += lodMs;

	m_peakChunks = std::max(m_peakChunks, chunks);
	m_peakDraw = std::max(m_peakDraw, drawCount);
	m_peakLoad = std::max(m_peakLoad, pendingLoad);
	m_peakGen = std::max(m_peakGen, pendingGen);
	m_peakMesh = std::max(m_peakMesh, pendingMesh);

	if (frameMs > 16.7f)
		++m_over16;
	if (frameMs > 33.3f)
		++m_over33;
}

void Benchmark::sampleBackgroundWork(const char *name, uint64_t count, double totalMs)
{
	if (m_phase != BenchmarkPhase::Running)
		return;
	for (size_t i = 0; i < m_backgroundWork.size(); ++i)
		if (std::strcmp(name, kBackgroundNames[i]) == 0)
		{
			m_backgroundWork[i].count += count;
			m_backgroundWork[i].totalMs += totalMs;
		}
}

float Benchmark::totalProgress() const
{
	const float total = m_config.warmupSec + m_config.durationSec;
	if (total <= 1e-4f)
		return 1.f;
	return std::clamp(m_elapsed / total, 0.f, 1.f);
}

float Benchmark::measureProgress() const
{
	if (m_phase == BenchmarkPhase::Warmup || m_elapsed < m_config.warmupSec)
		return 0.f;
	if (m_config.durationSec <= 1e-4f)
		return 1.f;
	return std::clamp((m_elapsed - m_config.warmupSec) / m_config.durationSec, 0.f, 1.f);
}

float Benchmark::remainingMeasureSec() const
{
	const float end = m_config.warmupSec + m_config.durationSec;
	return std::max(0.f, end - m_elapsed);
}

float Benchmark::percentileSorted(std::vector<float> &sorted, float p01)
{
	if (sorted.empty())
		return 0.f;
	const float p = std::clamp(p01, 0.f, 1.f);
	const size_t n = sorted.size();
	const size_t idx = std::min(n - 1, static_cast<size_t>(std::ceil(p * static_cast<float>(n)) - 1.f));
	// For p=0 use first; for p=1 use last
	if (p <= 0.f)
		return sorted.front();
	if (p >= 1.f)
		return sorted.back();
	const size_t i = static_cast<size_t>(p * static_cast<float>(n - 1));
	return sorted[i];
}

int Benchmark::computeScore(const BenchmarkReport &r)
{
	if (r.frames <= 0 || r.avgMs <= 1e-6f)
		return 0;

	const float avgFps = r.avgFps;
	const float p1Fps = r.onePercentLowFps;

	const float nAvg = std::clamp(avgFps / 60.f, 0.f, 2.f);
	const float nP1 = std::clamp(p1Fps / 60.f, 0.f, 2.f);
	const float spread = std::max(0.f, r.p99Ms - r.p50Ms);
	const float nStab = std::clamp(1.f - spread / 33.3f, 0.f, 1.f);

	float score = 10000.f * (0.45f * std::min(nAvg, 1.f) + 0.15f * std::max(0.f, nAvg - 1.f) +
							 0.30f * std::min(nP1, 1.f) + 0.10f * nStab);

	const float dropFrac =
		static_cast<float>(r.framesOver33ms) / static_cast<float>(std::max(1, r.frames));
	score *= (1.f - 0.15f * std::clamp(dropFrac, 0.f, 1.f));

	return std::clamp(static_cast<int>(std::lround(score)), 0, 10000);
}

char Benchmark::gradeForScore(int score)
{
	if (score >= 9000)
		return 'S';
	if (score >= 7500)
		return 'A';
	if (score >= 6000)
		return 'B';
	if (score >= 4500)
		return 'C';
	if (score >= 3000)
		return 'D';
	return 'F';
}

void Benchmark::finalize()
{
	BenchmarkReport r{};
	r.valid = true;
	r.seed = m_config.seed;
	r.durationSec = m_config.durationSec;
	r.warmupSec = m_config.warmupSec;
	r.measuredSec = std::max(0.f, m_elapsed - m_config.warmupSec);
	r.frames = static_cast<int>(m_frameMs.size());

	if (!m_frameMs.empty())
	{
		double sum = 0;
		r.minMs = m_frameMs[0];
		r.maxMs = m_frameMs[0];
		for (float v : m_frameMs)
		{
			sum += v;
			r.minMs = std::min(r.minMs, v);
			r.maxMs = std::max(r.maxMs, v);
		}
		r.avgMs = static_cast<float>(sum / static_cast<double>(m_frameMs.size()));
		r.avgFps = r.avgMs > 1e-4f ? 1000.f / r.avgMs : 0.f;

		std::vector<float> sorted = m_frameMs;
		std::sort(sorted.begin(), sorted.end());
		r.p50Ms = percentileSorted(sorted, 0.50f);
		r.p95Ms = percentileSorted(sorted, 0.95f);
		r.p99Ms = percentileSorted(sorted, 0.99f);
		r.p1LowMs = r.p99Ms; // 1% low FPS ≈ high frame-time percentile
		r.onePercentLowFps = r.p1LowMs > 1e-4f ? 1000.f / r.p1LowMs : 0.f;
	}

	const float invN = r.frames > 0 ? 1.f / static_cast<float>(r.frames) : 0.f;
	r.avgStreaming = static_cast<float>(m_sumStreaming) * invN;
	r.avgAcquire = static_cast<float>(m_sumAcquire) * invN;
	r.avgRecord = static_cast<float>(m_sumRecord) * invN;
	r.avgImGui = static_cast<float>(m_sumImGui) * invN;
	r.avgPresent = static_cast<float>(m_sumPresent) * invN;
	r.avgVisibility = static_cast<float>(m_sumVisibility) * invN;
	r.avgMeshUpload = static_cast<float>(m_sumMeshUpload) * invN;

	r.backgroundWork = m_backgroundWork;
	r.biomeMapZoom = m_config.biomeMapZoom;
	r.biomeMapSequential = m_config.biomeMapSequential;
	r.terrainGenJobs = m_terrainJobs;
	r.terrainGenTotalMs = static_cast<float>(m_terrainMs);
	r.terrainGenAvgMs = m_terrainJobs > 0 ? static_cast<float>(m_terrainMs / m_terrainJobs) : 0.f;
	r.meshBuildJobs = m_meshJobs;
	r.meshBuildTotalMs = static_cast<float>(m_meshMs);
	r.meshBuildAvgMs = m_meshJobs > 0 ? static_cast<float>(m_meshMs / m_meshJobs) : 0.f;
	r.meshLodJobs = m_lodJobs;
	r.meshLodTotalMs = static_cast<float>(m_lodMs);
	r.meshLodAvgMs = m_lodJobs > 0 ? static_cast<float>(m_lodMs / m_lodJobs) : 0.f;

	r.peakChunks = m_peakChunks;
	r.peakDraw = m_peakDraw;
	r.peakPendingLoad = m_peakLoad;
	r.peakPendingGen = m_peakGen;
	r.peakPendingMesh = m_peakMesh;
	r.framesOver16ms = m_over16;
	r.framesOver33ms = m_over33;

	r.viewDistance = m_viewDistance;
	r.windowW = m_windowW;
	r.windowH = m_windowH;
	r.vsync = m_vsync;
	r.presentMode = m_presentMode;
	r.deviceName = m_deviceName;

	r.gitHash = BuildInfo::gitHash();
	r.gitBranch = BuildInfo::gitBranch();
	r.gitDescribe = BuildInfo::gitDescribe();
	r.revisionLabel = BuildInfo::revisionLabel();
	r.buildUtc = BuildInfo::buildUtc();
	r.gitDirty = BuildInfo::gitDirty();

	r.score = computeScore(r);
	r.grade = gradeForScore(r.score);
	m_report = r;
}

std::string Benchmark::formatReportText() const
{
	const BenchmarkReport &r = m_report;
	if (!r.valid)
		return "No benchmark report.";

	std::ostringstream o;
	o << "ft_vox Benchmark Report\n";
	o << "=======================\n";
	o << "Score: " << r.score << " / 10000  Grade: " << r.grade << "\n";
	o << "Revision: " << r.revisionLabel;
	if (r.gitDirty)
		o << " (dirty tree at build)";
	o << "\n";
	o << "  git " << r.gitHash << "  branch " << r.gitBranch << "  describe " << r.gitDescribe
	  << "\n";
	o << "  build UTC " << r.buildUtc << "\n";
	o << "Seed: " << r.seed << "  Duration: " << r.durationSec << "s  Warmup: " << r.warmupSec
	  << "s  Measured: " << r.measuredSec << "s  Frames: " << r.frames << "\n";
	o << "Device: " << r.deviceName << "\n";
	o << "Viewport: " << r.windowW << "x" << r.windowH << "  ViewDist: " << r.viewDistance
	  << "  VSync: " << (r.vsync ? "on" : "off")
	  << "  PresentMode: " << r.presentMode << "\n\n";
	o << "Frame times (ms)\n";
	o << "  avg " << r.avgMs << "  min " << r.minMs << "  max " << r.maxMs << "\n";
	o << "  p50 " << r.p50Ms << "  p95 " << r.p95Ms << "  p99 " << r.p99Ms << "\n";
	o << "  avg FPS " << r.avgFps << "  1% low FPS " << r.onePercentLowFps << "\n";
	o << "  frames >16.7ms: " << r.framesOver16ms << "  >33.3ms: " << r.framesOver33ms << "\n\n";
	o << "CPU scopes (avg ms)\n";
	o << "  Streaming " << r.avgStreaming << "  Visibility " << r.avgVisibility << "\n";
	o << "  Acquire " << r.avgAcquire << "  Record " << r.avgRecord << "  MeshUpload "
	  << r.avgMeshUpload << "\n";
	o << "  ImGui " << r.avgImGui << "  Present " << r.avgPresent << "\n\n";
	o << "Worker jobs\n";
	o << "  TerrainGen  n=" << r.terrainGenJobs << "  avgMs=" << r.terrainGenAvgMs
	  << "  totalMs=" << r.terrainGenTotalMs << "\n";
	o << "  MeshBuild   n=" << r.meshBuildJobs << "  avgMs=" << r.meshBuildAvgMs
	  << "  totalMs=" << r.meshBuildTotalMs << "\n";
	o << "  MeshLOD     n=" << r.meshLodJobs << "  avgMs=" << r.meshLodAvgMs
	  << "  totalMs=" << r.meshLodTotalMs << "\n\n";
	o << "Biome map: zoom=" << r.biomeMapZoom
	  << " mode=" << (r.biomeMapSequential ? "sequential" : "scheduled")
	  << " (fixed center when enabled)\n";
	for (size_t i = 0; i < r.backgroundWork.size(); ++i)
	{
		const auto &work = r.backgroundWork[i];
		o << "  " << kBackgroundNames[i] << " n=" << work.count
		  << " avgMs=" << (work.count ? work.totalMs / work.count : 0.0)
		  << " totalMs=" << work.totalMs << "\n";
	}
	o << "Peaks: chunks=" << r.peakChunks << " draw=" << r.peakDraw
	  << " qLoad/Gen/Mesh=" << r.peakPendingLoad << "/" << r.peakPendingGen << "/"
	  << r.peakPendingMesh << "\n\n";
	o << "Score: 45% avgFPS@60 + 15% headroom + 30% 1%low@60 + 10% stability;\n";
	o << "       -15% max penalty for fraction of frames >33ms.\n";
	return o.str();
}

namespace
{
bool ensureDir(const std::string &path)
{
	if (path.empty())
		return true;
#if defined(_WIN32)
	return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
	return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

std::string sanitizeFilenameToken(std::string s)
{
	for (char &c : s)
	{
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
			c == '_')
			continue;
		c = '_';
	}
	return s;
}
} // namespace

std::string Benchmark::saveReportToFile(const std::string &directory) const
{
	m_lastSavedPath.clear();
	if (!m_report.valid)
		return {};

	std::string dir = directory;
	if (dir.empty())
		dir = "benchmarks";

	if (!ensureDir(dir))
	{
		// Still try writing; some platforms already have the dir
	}

	// Filename: bench_YYYYMMDD_HHMMSS_<hash>_s<seed>_sc<score>.txt
	std::time_t now = std::time(nullptr);
	std::tm tmUtc{};
#if defined(_WIN32)
	gmtime_s(&tmUtc, &now);
#else
	gmtime_r(&now, &tmUtc);
#endif
	char timeBuf[32];
	std::snprintf(timeBuf, sizeof(timeBuf), "%04d%02d%02d_%02d%02d%02d", tmUtc.tm_year + 1900,
				  tmUtc.tm_mon + 1, tmUtc.tm_mday, tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);

	const std::string rev = sanitizeFilenameToken(m_report.revisionLabel);
	char name[256];
	std::snprintf(name, sizeof(name), "bench_%s_%s_s%d_sc%d.txt", timeBuf, rev.c_str(),
				  m_report.seed, m_report.score);

	const std::string path = dir + "/" + name;
	std::ofstream out(path, std::ios::out | std::ios::trunc);
	if (!out)
		return {};

	out << formatReportText();
	if (!out.good())
		return {};

	m_lastSavedPath = path;
	return path;
}
