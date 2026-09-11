// Streaming panel (issue #179): distance/budget settings plus live
// queue-depth, pool and upload telemetry with bounded histories. Console
// values come from the StreamingDebugSnapshot; the settings sliders mutate
// RenderSettings directly (the engine's settings API).

#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/UiScale.hpp>
#include <Engine/GameUI.hpp>
#include <Engine/DebugUI/DebugPanelUtil.hpp>
#include <Engine/Profiler.hpp>

#include <imgui/imgui.h>

#include <cmath>

namespace debugui
{

void drawStreaming(UiState &s, GameUIFrame &frame)
{
	const float scale = ui::effectiveScale(frame.uiScale);
	ImGui::SetNextWindowSize(ImVec2(ui::scaled(420.f, scale), ui::scaled(560.f, scale)), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(ui::windows::kStreaming, &s.panels.streaming))
	{
		ImGui::End();
		return;
	}

	if (!frame.render || !frame.chunks)
	{
		ImGui::TextDisabled("Chunk manager not ready.");
		ImGui::End();
		return;
	}

	auto &rs = *frame.render;
	ImGui::SeparatorText("Distance");
	const int prevMaxRd = rs.maxRenderDistance;
	ImGui::SliderInt("View distance (blocks)", &rs.maxRenderDistance, 64, 640);
	if (rs.minRenderDistance > rs.maxRenderDistance)
		rs.minRenderDistance = rs.maxRenderDistance;
	ImGui::SliderInt("Full-mesh near range", &rs.minRenderDistance, 32, rs.maxRenderDistance);
	ImGui::SliderFloat("Front load bias", &rs.streamFrontBias, 0.f, kSafeMaxStreamFrontBias, "%.2f");
	ImGui::TextDisabled("Ahead reach ~ ×%.2f, behind ~ ×%.2f",
						1.0f / std::sqrt(1.0f - normalizedStreamFrontBias(rs.streamFrontBias)),
						1.0f / std::sqrt(1.0f + normalizedStreamFrontBias(rs.streamFrontBias)));
	ImGui::TextDisabled("Unload at ~%.2f× view distance; bias capped so ahead reach stays inside it",
						kChunkUnloadDistanceFactor);

	const size_t poolNeed = estimateChunkPoolCapacity(rs.maxRenderDistance);
	if (frame.pool && rs.maxRenderDistance != prevMaxRd)
		frame.pool->ensureCapacity(poolNeed);
	ImGui::TextDisabled("Pool need for this view: ~%zu chunks", poolNeed);

	ImGui::SeparatorText("Pipeline budgets (ops / sec)");
	ImGui::SliderInt("Load/s", &rs.loadPerSec, 10, 1000);
	ImGui::SliderInt("Gen/s", &rs.genPerSec, 5, 800);
	ImGui::SliderInt("Mesh/s", &rs.meshPerSec, 5, 600);
	ImGui::SliderInt("Light cache/s", &rs.lightCachePerSec, 0, 256);
	ImGui::SliderInt("Upload/s", &rs.uploadPerSec, 5, 800);
	ImGui::SliderFloat("Stream ms/frame", &rs.maxStreamMs, 0.f, 16.f, "%.1f");
	// Shadow distance moved to Graphics ▸ Shadows (issue #185).

	ImGui::SeparatorText("Live stats");
	ImGui::Text("Loaded chunks: %zu", s.streaming.loadedChunks);
	ImGui::Text("Draw list:     %zu", s.streaming.drawCount);
	ImGui::Text("Queues load/gen/mesh/light: %zu / %zu / %zu / %zu",
				s.streaming.pendingLoad, s.streaming.pendingGen,
				s.streaming.pendingMesh, s.streaming.pendingLight);
	ImGui::Text("Upload backlog: %zu   |   deferred releases: %zu",
				s.streaming.uploadBacklog, s.streaming.deferredReleases);
	ImGui::Text("Dispatched: %s mesh   |   %s light-cache jobs",
				formatCount(s.streaming.meshJobsDispatched).c_str(),
				formatCount(s.streaming.lightJobsDispatched).c_str());
	ImGui::TextDisabled("Queue depths (10 Hz): mesh / light");
	plotHistory("##st_mesh", s.pendingMesh, 0.f, ImVec2(-1.f, ui::scaled(44.f, scale)));
	plotHistory("##st_light", s.pendingLight, 0.f, ImVec2(-1.f, ui::scaled(44.f, scale)));

	if (frame.pool)
	{
		ImGui::SeparatorText("Chunk pool");
		ImGui::Text("Capacity %zu  |  free %zu  |  acquired %zu",
					s.streaming.poolCapacity, s.streaming.poolFree, s.streaming.poolAcquired);
		ImGui::Text("Need ~%zu for view %d  |  grows: %zu",
					poolNeed, s.streaming.viewDistance, s.streaming.poolGrows);
		if (s.streaming.poolFree == 0)
			ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f),
							   "Pool full — load back-pressure active");
		if (s.streaming.poolRejects > 0)
			ImGui::TextColored(ImVec4(1.f, 0.55f, 0.2f, 1.f), "Acquire rejects: %zu",
							   s.streaming.poolRejects);
	}

	if (frame.camera && ImGui::Button("Inspect chunk under player (F9)"))
	{
		const glm::vec3 p = frame.camera->getPosition();
		s.inspectCoord = glm::ivec3(int(std::floor(p.x / CHUNK_SIZE)), 0,
									int(std::floor(p.z / CHUNK_SIZE)));
		s.inspectHasTarget = true;
		s.panels.chunkInspector = true;
	}

	{
		ImGui::SeparatorText("CPU timings (ms)");
		Profiler &prof = GetProfiler();
		if (ImGui::BeginTable("perf", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("Stage");
			ImGui::TableSetupColumn("ms");
			ImGui::TableHeadersRow();
			auto row = [](const char *name, float v) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(name);
				ImGui::TableNextColumn();
				ImGui::Text("%.2f", v);
			};
			row("Visibility", prof.lastScopeMs("Visibility"));
			row("Gen dispatch", prof.lastScopeMs("GenDispatch"));
			row("Mesh dispatch", prof.lastScopeMs("MeshDispatch"));
			row("Mesh upload", prof.lastScopeMs("MeshUpload"));
			row("Streaming", prof.lastScopeMs("Streaming"));
			row("Acquire (fence)", prof.lastScopeMs("Acquire"));
			row("Record", prof.lastScopeMs("Record"));
			row("Frame total", prof.lastFrameMs());
			ImGui::EndTable();
		}
		ImGui::TextDisabled("Full hierarchy + graphs: Performance (F7)");
	}

	if (frame.deviceName)
	{
		ImGui::SeparatorText("Device");
		ImGui::TextWrapped("%s", frame.deviceName);
		ImGui::Text("Vulkan %u.%u  |  validation %s",
					VK_VERSION_MAJOR(frame.vkApiVersion),
					VK_VERSION_MINOR(frame.vkApiVersion),
					frame.validation ? "on" : "off");
	}

	ImGui::End();
}

} // namespace debugui
