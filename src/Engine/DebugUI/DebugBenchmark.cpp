// Benchmark panel + report (issue #179): the scored benchmark controls
// moved out of the old Profiler window into their own domain panel; the
// report window is unchanged in content and behavior.

#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/GameUI.hpp>
#include <Engine/DebugUI/DebugPanelUtil.hpp>
#include <Engine/Benchmark.hpp>
#include <Engine/GpuProfile.hpp>

#include <imgui/imgui.h>

namespace debugui
{

void drawBenchmarkPanel(UiState &s, GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(360, 380), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Benchmark", &s.panels.benchmark))
	{
		ImGui::End();
		return;
	}

	if (!frame.benchmark)
	{
		ImGui::TextDisabled("Benchmark unavailable.");
		ImGui::End();
		return;
	}

	Benchmark &bench = *frame.benchmark;
	BenchmarkConfig &cfg = bench.config();
	ImGui::TextDisabled("Reload seed, orbit path, scored report.");

	const bool active = bench.isActive();
	ImGui::BeginDisabled(active);
	ImGui::InputInt("Seed", &cfg.seed);
	if (cfg.seed <= 0)
		cfg.seed = 42;
	ImGui::SliderFloat("Duration (s)", &cfg.durationSec, 10.f, 180.f, "%.0f");
	ImGui::SliderFloat("Warmup (s)", &cfg.warmupSec, 0.f, 15.f, "%.1f");
	ImGui::SliderFloat("Orbit radius", &cfg.pathRadius, 64.f, 320.f, "%.0f");
	ImGui::SliderInt("Orbits", &cfg.pathOrbits, 1, 6);
	ImGui::Checkbox("Force VSync off", &cfg.forceVsyncOff);
	ImGui::TextUnformatted("Path: Orbit (look at spawn)");
	ImGui::EndDisabled();

	if (!active)
	{
		if (ImGui::Button("Start benchmark", ImVec2(-1.f, 0.f)))
			bench.requestStart();
	}
	else
	{
		const char *phaseStr = "…";
		switch (bench.phase())
		{
		case BenchmarkPhase::Reloading:
			phaseStr = "Reloading world…";
			break;
		case BenchmarkPhase::Warmup:
			phaseStr = "Warmup (streaming fill)";
			break;
		case BenchmarkPhase::Running:
			phaseStr = "Measuring";
			break;
		default:
			break;
		}
		ImGui::Text("%s", phaseStr);
		ImGui::ProgressBar(bench.totalProgress(), ImVec2(-1.f, 0.f));
		ImGui::Text("Elapsed %.1fs  |  remain measure %.1fs  |  FPS ~%.0f",
					bench.elapsedSec(), bench.remainingMeasureSec(), frame.fps);
		if (ImGui::Button("Cancel", ImVec2(-1.f, 0.f)))
			bench.cancel();
	}

	if (bench.report().valid && !active)
	{
		if (ImGui::Button("Show last report"))
			bench.setShowReport(true);
		ImGui::SameLine();
		const BenchmarkReport &r = bench.report();
		ImGui::Text("Last score: %d (%c)", r.score, r.grade);
	}

	ImGui::End();
}

void drawBenchmarkReport(UiState &s, GameUIFrame &frame)
{
	if (!frame.benchmark)
		return;
	Benchmark &bench = *frame.benchmark;
	const BenchmarkReport &r = bench.report();
	bool open = true;
	ImGui::SetNextWindowSize(ImVec2(440, 520), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Benchmark Report", &open))
	{
		ImGui::End();
		if (!open)
			bench.setShowReport(false);
		return;
	}
	if (!open)
	{
		bench.setShowReport(false);
		ImGui::End();
		return;
	}

	ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.3f, 1.f), "SCORE  %d  /  10000", r.score);
	ImGui::SameLine();
	ImGui::Text("  Grade %c", r.grade);
	ImGui::Text("Seed %d  |  %.0fs measure (+%.0fs warmup)  |  %d frames", r.seed, r.durationSec,
				r.warmupSec, r.frames);

	ImGui::SeparatorText("Frame times");
	if (ImGui::BeginTable("bm_ft", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		auto row = [](const char *k, const char *fmt, auto... args) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(k);
			ImGui::TableNextColumn();
			ImGui::Text(fmt, args...);
		};
		row("Avg FPS", "%.1f", r.avgFps);
		row("1%% low FPS", "%.1f", r.onePercentLowFps);
		row("Avg / min / max ms", "%.2f / %.2f / %.2f", r.avgMs, r.minMs, r.maxMs);
		row("p50 / p95 / p99 ms", "%.2f / %.2f / %.2f", r.p50Ms, r.p95Ms, r.p99Ms);
		row("Frames >16.7 ms", "%d", r.framesOver16ms);
		row("Frames >33.3 ms", "%d", r.framesOver33ms);
		ImGui::EndTable();
	}

	ImGui::SeparatorText("GPU timestamps");
	if (!r.gpuAvailable)
		ImGui::TextDisabled("Unavailable: no completed GPU samples");
	else
	{
		ImGui::Text("%llu samples | average %.3f ms", static_cast<unsigned long long>(r.gpuSamples), r.gpuAvgMs);
		if (r.gpuPercentilesAvailable)
			ImGui::Text("p95 %.3f ms | p99 %.3f ms", r.gpuP95Ms, r.gpuP99Ms);
		else
			ImGui::TextDisabled("p95/p99 unavailable (fewer than 100 samples)");
		for (size_t i = 1; i < kGpuPassCount; ++i)
			if (r.gpuPasses[i].count)
				ImGui::Text("%s: %.3f ms", kGpuPassNames[i], r.gpuPasses[i].totalMs / r.gpuPasses[i].count);
	}
	ImGui::SeparatorText("CPU scopes (avg ms)");
	if (ImGui::BeginTable("bm_sc", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		auto row = [](const char *k, float v) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(k);
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", v);
		};
		row("Streaming", r.avgStreaming);
		row("Visibility", r.avgVisibility);
		row("Acquire", r.avgAcquire);
		row("Record", r.avgRecord);
		row("MeshUpload", r.avgMeshUpload);
		row("ImGui", r.avgImGui);
		row("Present", r.avgPresent);
		ImGui::EndTable();
	}

	ImGui::SeparatorText("Worker jobs");
	if (ImGui::BeginTable("bm_wk", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Job");
		ImGui::TableSetupColumn("count");
		ImGui::TableSetupColumn("avg ms");
		ImGui::TableSetupColumn("total ms");
		ImGui::TableHeadersRow();
		auto wrow = [](const char *n, uint64_t c, float a, float t) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(n);
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(c));
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", a);
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", t);
		};
		wrow("TerrainGen", r.terrainGenJobs, r.terrainGenAvgMs, r.terrainGenTotalMs);
		wrow("MeshBuild", r.meshBuildJobs, r.meshBuildAvgMs, r.meshBuildTotalMs);
		wrow("MeshLOD", r.meshLodJobs, r.meshLodAvgMs, r.meshLodTotalMs);
		ImGui::EndTable();
	}

	ImGui::SeparatorText("Build / revision");
	ImGui::Text("Revision: %s%s", r.revisionLabel.c_str(), r.gitDirty ? "  (dirty)" : "");
	ImGui::Text("Branch: %s", r.gitBranch.c_str());
	ImGui::TextWrapped("Describe: %s", r.gitDescribe.c_str());
	ImGui::Text("Built (UTC): %s", r.buildUtc.c_str());

	ImGui::SeparatorText("Peaks & settings");
	ImGui::Text("Chunks %zu  |  draw %zu  |  queues %zu / %zu / %zu / %zu", r.peakChunks, r.peakDraw,
				r.peakPendingLoad, r.peakPendingGen, r.peakPendingMesh, r.peakPendingLight);
	ImGui::Text("View %d  |  %dx%d  |  VSync %s  |  %s",
				r.viewDistance, r.windowW, r.windowH,
				r.vsync ? "on" : "off", r.presentMode.c_str());
	if (!r.deviceName.empty())
		ImGui::TextWrapped("%s", r.deviceName.c_str());

	ImGui::Separator();
	ImGui::TextWrapped(
		"Score: 45%% avgFPS@60 + 15%% headroom + 30%% 1%%low@60 + 10%% stability; "
		"up to -15%% for frames >33ms. Higher is better.");

	if (ImGui::Button("Copy summary"))
	{
		const std::string text = bench.formatReportText();
		ImGui::SetClipboardText(text.c_str());
	}
	ImGui::SameLine();
	if (ImGui::Button("Save summary"))
	{
		const std::string path = bench.saveReportToFile("benchmark-results");
		if (path.empty())
			ImGui::OpenPopup("bench_save_fail");
		else
			ImGui::OpenPopup("bench_save_ok");
	}
	ImGui::SameLine();
	if (ImGui::Button("Close"))
		bench.setShowReport(false);

	if (ImGui::BeginPopup("bench_save_ok"))
	{
		ImGui::Text("Saved:");
		ImGui::TextWrapped("%s", bench.lastSavedPath().c_str());
		if (ImGui::Button("OK"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	if (ImGui::BeginPopup("bench_save_fail"))
	{
		ImGui::TextWrapped("Failed to write benchmark-results/… (check cwd permissions).");
		if (ImGui::Button("OK"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	if (!bench.lastSavedPath().empty())
		ImGui::TextDisabled("Last save: %s", bench.lastSavedPath().c_str());

	ImGui::End();
}

} // namespace debugui
