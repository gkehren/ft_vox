// Overview dashboard (issue #179): a compact "is the engine healthy?" view.
// Answers in a few seconds what limits the frame, what is backlogged, where
// memory sits, and which sustained warning conditions are active. Renders
// exclusively from debugui::UiState snapshots; heavy sampling behind it is
// throttled to 10 Hz in updateDebugUiState and skipped when closed.

#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/GameUI.hpp>
#include <Engine/DebugUI/DebugPanelUtil.hpp>

#include <imgui/imgui.h>

#include <cmath>

namespace debugui
{

void drawOverview(UiState &s, GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(440, 560), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Overview", &s.panels.overview))
	{
		ImGui::End();
		return;
	}

	// --- Frame ---
	ImGui::SeparatorText("Frame");
	ImGui::Text("%.1f FPS   |   CPU frame %.2f ms", s.frame.fps, s.frame.cpuFrameMs);
	if (std::abs(s.frame.simulationDtMs - s.frame.cpuFrameMs) > 0.5f)
		ImGui::TextDisabled("Simulation tick %.2f ms (paced dt diverges from frame time)",
							s.frame.simulationDtMs);
	if (s.frame.gpuValid)
		ImGui::Text("GPU frame: %.2f ms (pass intervals overlap)", s.frame.gpuFrameMs);
	else
		ImGui::TextDisabled("GPU frame: no timestamps");
	ImGui::Text("avg %.2f ms   |   1%% low %.2f ms   |   VSync %s (%s)",
				s.frame.avgMs, s.frame.onePercentLowMs,
				s.frame.vsync ? "on" : "off", s.frame.presentMode);
	ImGui::TextDisabled("CPU frame history (10 Hz, 25 s window)");
	plotHistory("##ov_cpu", s.cpuMs, 0.f, ImVec2(-1.f, 56.f));

	// --- Streaming ---
	ImGui::SeparatorText("Streaming");
	ImGui::Text("Chunks %zu   |   draw list %zu   |   view %d m (near %d m)",
				s.streaming.loadedChunks, s.streaming.drawCount,
				s.streaming.viewDistance, s.streaming.nearRange);
	ImGui::Text("Queues load/gen/mesh/light: %zu / %zu / %zu / %zu",
				s.streaming.pendingLoad, s.streaming.pendingGen,
				s.streaming.pendingMesh, s.streaming.pendingLight);
	ImGui::Text("Upload backlog: %zu chunks   |   deferred releases: %zu",
				s.streaming.uploadBacklog, s.streaming.deferredReleases);
	if (frame.mobCount || frame.mobVisible)
		ImGui::Text("Mobs: %zu active / %zu visible", frame.mobCount, frame.mobVisible);
	ImGui::TextDisabled("Mesh queue depth (10 Hz)");
	plotHistory("##ov_mesh", s.pendingMesh, 0.f, ImVec2(-1.f, 48.f));

	// --- Memory / resources ---
	ImGui::SeparatorText("Memory");
	if (!s.memory.telemetryEnabled)
	{
		ImGui::TextDisabled("Telemetry disabled (FT_VOX_TELEMETRY=0)");
	}
	else
	{
		const auto &g = s.memory.live.current;
		const uint64_t cpuVoxel = g[telemetry::VoxelBytes] + g[telemetry::ShellBytes];
		const uint64_t cpuMeshPayload = g[telemetry::OpaqueVertexBytes] + g[telemetry::OpaqueIndexBytes] +
										g[telemetry::WaterVertexBytes] + g[telemetry::WaterIndexBytes];
		ImGui::Text("CPU voxel+shell %s   |   mesh payload %s",
					formatBytes(cpuVoxel).c_str(), formatBytes(cpuMeshPayload).c_str());
		ImGui::Text("GPU mesh live %s   |   retired %s (%s bufs)",
					formatBytes(g[telemetry::GpuLiveBytes]).c_str(),
					formatBytes(g[telemetry::RetiredBytes]).c_str(),
					formatCount(g[telemetry::RetiredBuffers]).c_str());
		const uint64_t arenaTotal = s.memory.arenaLiveBytes + s.memory.arenaFreeBytes;
		if (arenaTotal > 0)
		{
			ImGui::ProgressBar(float(s.memory.arenaLiveBytes) / float(arenaTotal),
							   ImVec2(-1.f, 0.f),
							   formatBytes(s.memory.arenaLiveBytes).c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Mesh arena: %s live of %s reserved (high-water %s)",
								  formatBytes(s.memory.arenaLiveBytes).c_str(),
								  formatBytes(arenaTotal).c_str(),
								  formatBytes(s.memory.arenaHighWaterBytes).c_str());
		}
		if (s.memory.stagingCapacityBytes > 0)
		{
			ImGui::ProgressBar(float(s.memory.stagingUsedBytes) / float(s.memory.stagingCapacityBytes),
							   ImVec2(-1.f, 0.f),
							   formatBytes(s.memory.stagingUsedBytes).c_str());
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("Staging ring slice: %s of %s this frame",
								  formatBytes(s.memory.stagingUsedBytes).c_str(),
								  formatBytes(s.memory.stagingCapacityBytes).c_str());
		}
	}

	// --- Health (sustained conditions only) ---
	ImGui::SeparatorText("Health");
	struct HealthRow
	{
		bool active;
		const char *label;
		const char *hint;
	};
	const HealthRow rows[] = {
		{s.health.poolRejects.active(), "Chunk pool rejects (back-pressure)",
		 "Load had to refuse chunks — pool too small for this view distance."},
		{s.health.stagingFailures.active(), "Staging allocation failures",
		 "Upload copies did not fit the frame's staging slice; they retry next frame."},
		{s.health.meshBacklog.active(), "Persistent mesh backlog",
		 "Mesh queue stayed deep — workers cannot keep up or budget too low."},
		{s.health.lightBacklog.active(), "Persistent light-cache backlog",
		 "Entity light-cache queue stayed deep."},
		{s.health.uploadBacklog.active(), "Persistent upload backlog",
		 "Chunks keep staged meshes awaiting GPU copies."},
		{s.health.retiredBacklog.active(), "Large retired-resource backlog",
		 "GPU resources awaiting deferred destruction keep piling up."},
		{s.health.gpuMemoryGrowth.active(), "GPU mesh memory growing",
		 "Live GPU mesh bytes rose steadily — possible arena churn or leak."},
		{s.health.slowFrames.active(), "Sustained slow frames",
		 "1% low frame time above 33 ms (240-frame window)."},
	};
	bool anyBad = false;
	for (const HealthRow &r : rows)
		anyBad = anyBad || r.active;
	if (!anyBad)
		ImGui::TextColored(healthColor(false), "All indicators nominal");
	for (const HealthRow &r : rows)
	{
		if (!r.active)
			continue;
		ImGui::TextColored(healthColor(true), "•  %s", r.label);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", r.hint);
	}

	ImGui::Separator();
	ImGui::TextDisabled("Details: Performance (F7) · Streaming (F3) · Memory (F11) · Chunk inspector (F9)");

	ImGui::End();
}

} // namespace debugui
