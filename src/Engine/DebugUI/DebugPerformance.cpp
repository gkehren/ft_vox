// Performance panel (issue #179): CPU/GPU profiling without running a
// benchmark. Preserves the previous Profiler functionality (hierarchical
// scopes, frame history, averages/1% low, GPU per-pass timestamps, worker
// jobs, spikes) and adds a flat, sortable scope table with
// current/average/peak columns plus per-scope history graphs. The benchmark
// controls moved to the dedicated Benchmark panel.

#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/GameUI.hpp>
#include <Engine/DebugUI/DebugPanelUtil.hpp>
#include <Engine/Profiler.hpp>
#include <Engine/GpuProfile.hpp>
#include <Vulkan/VkGpuProfiler.hpp>

#include <imgui/imgui.h>

#include <algorithm>
#include <vector>

namespace debugui
{
namespace
{
float historyMax(const MetricHistory &h, float floorMax)
{
	float m = floorMax;
	for (size_t i = 0; i < h.count(); ++i)
		m = std::max(m, h.back(h.count() - 1 - i) * 1.1f);
	return m;
}
} // namespace

void drawPerformance(UiState &s, GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(520, 620), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Performance", &s.panels.performance))
	{
		ImGui::End();
		return;
	}

	Profiler &prof = GetProfiler();

	bool capturing = prof.enabled();
	if (ImGui::Checkbox("Capture", &capturing))
		prof.setEnabled(capturing);
	ImGui::SameLine();
	if (ImGui::Button("Clear history"))
	{
		prof.clearHistory();
		if (frame.gpu) frame.gpu->syncCapture(prof.captureEpoch());
	}
	ImGui::SameLine();
	ImGui::TextDisabled("CPU scopes · previous frame");

	const float frameMs = prof.lastFrameMs();
	const float avgMs = prof.avgFrameMs();
	const float fpsEst = prof.fpsEstimate();
	const float p1 = prof.onePercentLowMs();

	ImGui::SeparatorText("CPU frame");
	ImGui::Text("%.1f FPS  |  %.2f ms  |  avg %.2f ms", fpsEst, frameMs, avgMs);
	if (p1 > 0.f)
		ImGui::Text("1%% low (slow frames): %.2f ms  (~%.0f FPS)", p1, p1 > 1e-4f ? 1000.f / p1 : 0.f);

	// Frame-time history graph (chronological order, from the profiler's
	// own 240-frame ring — per-frame resolution, unlike the 10 Hz console
	// histories).
	{
		const int n = prof.historyCount();
		std::vector<float> ordered;
		ordered.reserve(static_cast<size_t>(n > 0 ? n : 1));
		if (n > 0)
		{
			const float *hist = prof.frameHistory();
			const int write = prof.historyWriteIndex();
			const int start = (n < Profiler::kHistorySize) ? 0 : write;
			for (int i = 0; i < n; ++i)
				ordered.push_back(hist[(start + i) % Profiler::kHistorySize]);
		}
		else
		{
			ordered.push_back(frameMs);
		}

		float maxY = 16.7f;
		for (float v : ordered)
			maxY = std::max(maxY, v * 1.1f);
		maxY = std::max(maxY, 33.3f);

		ImGui::PlotLines("##ft", ordered.data(), static_cast<int>(ordered.size()), 0,
						 nullptr, 0.f, maxY, ImVec2(-1.f, 80.f));
		ImGui::TextDisabled("Graph scale 0–%.0f ms  (16.7 = 60 FPS, 33.3 = 30 FPS)", maxY);
	}

	// --- Flat scope table: current / average / peak, click to plot ---
	ImGui::SeparatorText("CPU scopes (flat, aggregated by name)");
	if (s.scopeStatCount == 0)
	{
		ImGui::TextDisabled("No scope statistics yet — enable Capture and wait a frame");
	}
	else
	{
		if (ImGui::BeginTable("scopestats", 5,
							  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
								  ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY,
							  ImVec2(0.f, 200.f)))
		{
			ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch, 0.f, 0);
			ImGui::TableSetupColumn("last ms", ImGuiTableColumnFlags_WidthFixed, 60.f, 1);
			ImGui::TableSetupColumn("avg ms", ImGuiTableColumnFlags_WidthFixed, 60.f, 2);
			ImGui::TableSetupColumn("peak ms", ImGuiTableColumnFlags_WidthFixed, 60.f, 3);
			ImGui::TableSetupColumn("frames", ImGuiTableColumnFlags_WidthFixed, 56.f, 4);
			ImGui::TableHeadersRow();

			// Sort a bounded index array per frame (<= kMaxScopeStats rows).
			static std::array<int, kMaxScopeStats> order{};
			static size_t orderSize = 0;
			if (orderSize != s.scopeStatCount)
			{
				orderSize = s.scopeStatCount;
				for (size_t i = 0; i < orderSize; ++i)
					order[i] = int(i);
			}
			if (ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs())
			{
				if (sortSpecs->SpecsCount > 0)
				{
					const auto col = sortSpecs->Specs[0].ColumnUserID;
					const bool asc = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
					std::stable_sort(order.begin(), order.begin() + ptrdiff_t(orderSize),
									 [&](int a, int b) {
										 const ScopeStats &sa = s.scopeStats[size_t(a)];
										 const ScopeStats &sb = s.scopeStats[size_t(b)];
										 float va = 0.f, vb = 0.f;
										 switch (col)
										 {
										 case 1: va = sa.lastMs; vb = sb.lastMs; break;
										 case 2: va = sa.avgMs; vb = sb.avgMs; break;
										 case 3: va = sa.peakMs; vb = sb.peakMs; break;
										 case 4: va = float(sa.frames); vb = float(sb.frames); break;
										 default: return false; // name sort below
										 }
										 if (va != vb)
											 return asc ? va < vb : va > vb;
										 return std::string_view(sa.name ? sa.name : "") <
												std::string_view(sb.name ? sb.name : "");
									 });
				}
			}

			for (size_t row = 0; row < orderSize; ++row)
			{
				const int slot = order[row];
				const ScopeStats &st = s.scopeStats[size_t(slot)];
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				char label[96];
				std::snprintf(label, sizeof(label), "%s%s", st.name ? st.name : "?",
							  s.selectedScopeGraph == slot ? "  <<" : "");
				if (ImGui::Selectable(label, s.selectedScopeGraph == slot,
									  ImGuiSelectableFlags_SpanAllColumns))
				{
					s.selectedScopeGraph = (s.selectedScopeGraph == slot) ? -1 : slot;
				}
				ImGui::TableNextColumn();
				const float ms = st.lastMs;
				ImVec4 col = healthColor(false);
				if (ms >= 8.f)
					col = ImVec4(1.f, 0.4f, 0.35f, 1.f);
				else if (ms >= 2.f)
					col = ImVec4(1.f, 0.85f, 0.35f, 1.f);
				ImGui::TextColored(col, "%.2f", ms);
				ImGui::TableNextColumn();
				ImGui::Text("%.2f", st.avgMs);
				ImGui::TableNextColumn();
				ImGui::Text("%.2f", st.peakMs);
				ImGui::TableNextColumn();
				ImGui::Text("%llu", static_cast<unsigned long long>(st.frames));
			}
			ImGui::EndTable();
		}
		ImGui::TextDisabled("Click a scope to toggle its history graph.");
		if (s.selectedScopeGraph >= 0 && s.selectedScopeGraph < int(s.scopeStatCount))
		{
			const ScopeStats &st = s.scopeStats[size_t(s.selectedScopeGraph)];
			ImGui::Text("%s — 10 Hz history (ms)", st.name ? st.name : "?");
			plotHistory("##scope_hist", st.history, 0.f, ImVec2(-1.f, 64.f));
		}

		// Hierarchy (per-frame tree, unchanged)
		if (ImGui::CollapsingHeader("CPU hierarchy (command recording, not GPU execution)"))
		{
			const float denom = frameMs > 1e-4f ? frameMs : 1.f;
			if (ImGui::BeginTable("scopes", 4,
								  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
								  ImVec2(0.f, 220.f)))
			{
				ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 56.f);
				ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 48.f);
				ImGui::TableSetupColumn("bar", ImGuiTableColumnFlags_WidthFixed, 100.f);
				ImGui::TableHeadersRow();

				const int count = prof.lastEntryCount();
				const ProfileEntry *entries = prof.lastEntries();
				for (int i = 0; i < count; ++i)
				{
					const ProfileEntry &e = entries[i];
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					if (e.depth > 0)
					{
						ImGui::Dummy(ImVec2(static_cast<float>(e.depth) * 12.f, 0.f));
						ImGui::SameLine(0.f, 0.f);
					}
					ImGui::TextUnformatted(e.name ? e.name : "?");

					ImGui::TableNextColumn();
					const float ms = e.durationMs;
					ImVec4 col(0.55f, 0.9f, 0.55f, 1.f);
					if (ms >= 8.f)
						col = ImVec4(1.f, 0.4f, 0.35f, 1.f);
					else if (ms >= 2.f)
						col = ImVec4(1.f, 0.85f, 0.35f, 1.f);
					ImGui::TextColored(col, "%.2f", ms);

					ImGui::TableNextColumn();
					ImGui::Text("%.0f", 100.f * ms / denom);

					ImGui::TableNextColumn();
					ImGui::ProgressBar(std::clamp(ms / denom, 0.f, 1.f), ImVec2(-1.f, 0.f), "");
				}
				if (count == 0)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextDisabled("No samples yet — wait a frame or enable Capture");
				}
				ImGui::EndTable();
			}
		}
	}

	// --- GPU ---
	ImGui::SeparatorText("GPU frame / passes");
	if (frame.gpu)
	{
		auto &gpu = *frame.gpu;
		bool enabled = gpu.enabled();
		if (ImGui::Checkbox("GPU timestamps", &enabled)) gpu.setEnabled(enabled);
		ImGui::TextDisabled("%s", gpu.status());
		const GpuFrameSample &latest = gpu.latest();
		if (latest.serial)
		{
			float maxPass = 0.001f;
			for (size_t i = 0; i < kGpuPassCount; ++i)
				if (latest.present[i])
					maxPass = std::max(maxPass, latest.ms[i]);
			for (size_t i = 0; i < kGpuPassCount; ++i)
			{
				if (!latest.present[i])
					continue;
				ImGui::Text("%-12s %8.3f ms", kGpuPassNames[i], latest.ms[i]);
				ImGui::SameLine(220.f);
				ImGui::ProgressBar(latest.ms[i] / maxPass, ImVec2(-1.f, 10.f), "");
			}
			ImGui::TextDisabled("Frame: %.3f ms — pass intervals may overlap; do not sum them.",
								latest.present[size_t(GpuPass::Frame)] ? latest.ms[size_t(GpuPass::Frame)] : 0.f);
			const int n = gpu.historyCount();
			if (n > 0)
			{
				std::vector<float> ordered;
				ordered.reserve(size_t(n));
				const int start = n < VkGpuProfiler::kHistorySize ? 0 : gpu.historyWrite();
				for (int i = 0; i < n; ++i)
					ordered.push_back(gpu.history()[(start + i) % VkGpuProfiler::kHistorySize]);
				ImGui::PlotLines("##gpu", ordered.data(), n, 0, nullptr, 0.f, FLT_MAX, ImVec2(-1.f, 80.f));
			}
		}
	}

	// --- Worker jobs ---
	ImGui::SeparatorText("Worker CPU (thread pool)");
	ImGui::TextDisabled("Totals can exceed frame time (parallel workers).");
	const int wc = prof.workerSnapshotCount();
	if (wc == 0)
	{
		ImGui::TextDisabled("No worker samples this frame");
	}
	else if (ImGui::BeginTable("workers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Job");
		ImGui::TableSetupColumn("count");
		ImGui::TableSetupColumn("avg ms");
		ImGui::TableSetupColumn("total ms");
		ImGui::TableHeadersRow();
		const WorkerSnapshot *ws = prof.workerSnapshots();
		for (int i = 0; i < wc; ++i)
		{
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(ws[i].name ? ws[i].name : "?");
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(ws[i].count));
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", ws[i].avgMs);
			ImGui::TableNextColumn();
			ImGui::Text("%.2f", ws[i].totalMs);
		}
		ImGui::EndTable();
	}

	// --- Spikes ---
	const int sc = prof.spikeCount();
	if (sc > 0)
	{
		ImGui::SeparatorText("Spikes (>20 ms)");
		const SpikeRecord *sp = prof.spikes();
		for (int i = sc - 1; i >= 0; --i)
		{
			ImGui::Text("%.1f ms  top: %s (%.1f ms)", sp[i].frameMs,
						sp[i].topScope ? sp[i].topScope : "?", sp[i].topMs);
		}
	}

	// --- Benchmark ---
	ImGui::Separator();
	if (ImGui::Button("Open Benchmark panel"))
		s.panels.benchmark = true;

	ImGui::End();
}

} // namespace debugui
