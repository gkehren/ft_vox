// Streaming panel (issues #179/#186): configure and quickly assess world
// streaming — Distance / Pipeline (first-class CPU budget, presets with
// truthful Custom semantics, advanced stage rates) / compact read-only Live
// health. Console values come from the StreamingDebugSnapshot; the settings
// sliders mutate RenderSettings directly (the engine's settings API).
// Deep pool/timing/device diagnostics deliberately live elsewhere in the
// developer console (Overview, Performance, Memory, Help > About) instead
// of being duplicated here.

#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/UiScale.hpp>
#include <Engine/UiTheme.hpp>
#include <Engine/GameUI.hpp>
#include <Engine/DebugUI/DebugPanelUtil.hpp>

#include <imgui/imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace debugui
{
namespace
{
// Queue-bar display caps (issue #186 §4): a full bar is 2x the sustained
// backlog threshold used by DebugHealth (64 sections ~= one chunk), so the
// scale is meaningful instead of an arbitrary percentage. Upload counts
// chunks rather than sections; its cap is a display reference only.
constexpr float kQueueBarCap = 128.f;
constexpr float kUploadBarCap = 64.f;

/// Horizontal queue bar with a fixed, labelled display cap. The bar turns
/// warn-tinted only while the corresponding SUSTAINED health monitor is
/// active (issue #186 §6): ordinary one-frame bursts while moving fast stay
/// neutral — a healthy system naturally creates short queues.
void queueBar(const char *label, size_t pending, float displayCap, bool sustainedWarn, float scale)
{
	ImGui::TextUnformatted(label);
	ImGui::SameLine(ui::scaled(96.f, scale));
	char overlay[32];
	std::snprintf(overlay, sizeof(overlay), "%zu", pending);
	const float fraction = std::clamp(static_cast<float>(pending) / displayCap, 0.f, 1.f);
	if (sustainedWarn)
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ui::statusColor(ui::StatusKind::Warn));
	ImGui::ProgressBar(fraction, ImVec2(-1.f, 0.f), overlay);
	if (sustainedWarn)
		ImGui::PopStyleColor();
}

/// One sustained-warning row: warn-tinted bullet + details on hover. The
/// hints point at the developer-console surfaces that own the deep data.
void warningRow(const char *label, const char *hint)
{
	ImGui::TextColored(ui::statusColor(ui::StatusKind::Warn), "•  %s", label);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", hint);
}
} // namespace

void drawStreaming(UiState &s, GameUIFrame &frame)
{
	const float scale = ui::effectiveScale(frame.uiScale);
	ImGui::SetNextWindowSize(ImVec2(ui::scaled(420.f, scale), ui::scaled(560.f, scale)),
							 ImGuiCond_FirstUseEver);
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

	// --- Distance (issue #186 §1): how far / how aggressively to stream. ---
	ImGui::SeparatorText("Distance");
	ImGui::SliderInt("View distance", &rs.maxRenderDistance, 64, 640, "%d blocks");
	rs.minRenderDistance = clampedNearRenderDistance(rs.minRenderDistance, rs.maxRenderDistance);
	ImGui::SliderInt("Full-quality distance", &rs.minRenderDistance, 32, rs.maxRenderDistance,
					 "%d blocks");
	ImGui::SliderFloat("Directional front bias", &rs.streamFrontBias, 0.f, kSafeMaxStreamFrontBias,
					   "%.2f");
	ImGui::TextDisabled("Streams farther ahead than behind (ahead ~×%.2f, behind ~×%.2f)",
						1.0f / std::sqrt(1.0f - normalizedStreamFrontBias(rs.streamFrontBias)),
						1.0f / std::sqrt(1.0f + normalizedStreamFrontBias(rs.streamFrontBias)));

	// The consequence of raising the view distance, made explicit (issue
	// #186 §1). The engine itself grows the pool per frame
	// (Engine::tickStreaming — a cheap no-op once large enough); the panel
	// only surfaces the estimate, it does not mutate the pool.
	const size_t poolNeed = estimateChunkPoolCapacity(rs.maxRenderDistance);
	ImGui::TextDisabled("Estimated resident capacity: ~%s chunks",
						formatCount(poolNeed).c_str());

	// --- Pipeline (issue #186 §2): the main-thread CPU budget stays
	// first-class; per-stage rates move to the Advanced disclosure;
	// optional presets with the same truthful Custom semantics as the
	// Graphics panel (#185): exact equality against documented, reproducible
	// values (Balanced == engine defaults). ---
	ImGui::SeparatorText("Pipeline");
	ImGui::SliderFloat("CPU streaming budget", &rs.maxStreamMs, 0.f, 16.f, "%.1f ms/frame");
	ImGui::TextDisabled("Main-thread streaming work cap per frame (0 = unlimited)");

	// Buttons first, THEN detection, THEN badge (issue #191 review): the
	// badge must reflect the same frame's click, never lag one frame behind.
	ImGui::Spacing();
	if (ImGui::Button("Conservative"))
		applyStreamingPreset(rs, StreamingQualityPreset::Conservative);
	ImGui::SameLine();
	if (ImGui::Button("Balanced"))
		applyStreamingPreset(rs, StreamingQualityPreset::Balanced);
	ImGui::SameLine();
	if (ImGui::Button("Aggressive"))
		applyStreamingPreset(rs, StreamingQualityPreset::Aggressive);
	ImGui::SameLine();
	if (const std::optional<StreamingQualityPreset> active = matchingStreamingPreset(rs))
	{
		const char *name = *active == StreamingQualityPreset::Conservative ? "Conservative"
						   : *active == StreamingQualityPreset::Balanced   ? "Balanced"
																		   : "Aggressive";
		ui::statusBadge(name, ui::StatusKind::Ok);
	}
	else
	{
		ui::statusBadge("Custom", ui::StatusKind::Info);
		ImGui::SetItemTooltip("Hand-edited streaming settings; click a preset to restore it.");
	}

	if (ImGui::CollapsingHeader("Advanced stage rates"))
	{
		ImGui::SliderInt("Load /s", &rs.loadPerSec, 10, 1000);
		ImGui::SliderInt("Generate /s", &rs.genPerSec, 5, 800);
		ImGui::SliderInt("Mesh /s", &rs.meshPerSec, 5, 600);
		ImGui::SliderInt("Light cache /s", &rs.lightCachePerSec, 0, 256);
		ImGui::SliderInt("Upload /s", &rs.uploadPerSec, 5, 800);
	}
	// Renderer-specific coverage is owned by Graphics > Shadows since
	// #185 (shadowDistance): this panel owns residency + scheduling only.

	// --- Live health (issue #186 §4/§6): compact read-only summary from the
	// 10 Hz console snapshot. Bars have a fixed meaningful scale (2x the
	// sustained backlog threshold); warn tinting follows the sustained
	// DebugHealth monitors, never single-frame spikes. Detailed histories
	// remain in Overview (F8) / Performance (F7). ---
	ImGui::SeparatorText("Live health");
	ImGui::Text("Loaded %zu   |   Visible (draw list) %zu",
				s.streaming.loadedChunks, s.streaming.drawCount);
	queueBar("Load", s.streaming.pendingLoad, kQueueBarCap, false, scale);
	queueBar("Generate", s.streaming.pendingGen, kQueueBarCap, false, scale);
	queueBar("Mesh", s.streaming.pendingMesh, kQueueBarCap, s.health.meshBacklog.active(), scale);
	queueBar("Light", s.streaming.pendingLight, kQueueBarCap, s.health.lightBacklog.active(), scale);
	queueBar("Upload", s.streaming.uploadBacklog, kUploadBarCap, s.health.uploadBacklog.active(),
			 scale);

	ImGui::Spacing();
	if (s.health.poolRejects.active())
		warningRow("Pool pressure",
				   "Chunk acquires were refused recently — the pool is too small for this "
				   "view distance. Details: Memory (F11) / Overview (F8).");
	if (s.health.meshBacklog.active())
		warningRow("Mesh queue backlog",
				   "The mesh queue stayed deep for several seconds; workers cannot keep up "
				   "or the budget is too low. Details: Overview (F8) / Performance (F7).");
	if (s.health.lightBacklog.active())
		warningRow("Light-cache backlog",
				   "The light-cache queue stayed deep for several seconds. Details: Overview (F8).");
	if (s.health.uploadBacklog.active())
		warningRow("Upload pressure",
				   "Chunks kept staged meshes awaiting GPU copies for several seconds. "
				   "Details: Memory (F11).");
	if (!(s.health.poolRejects.active() || s.health.meshBacklog.active() ||
		  s.health.lightBacklog.active() || s.health.uploadBacklog.active()))
		ImGui::TextColored(ui::statusColor(ui::StatusKind::Ok), "Streaming nominal");

	if (frame.camera && ImGui::Button("Inspect chunk under player (F9)"))
	{
		const glm::vec3 p = frame.camera->getPosition();
		s.inspectCoord = glm::ivec3(int(std::floor(p.x / CHUNK_SIZE)), 0,
									int(std::floor(p.z / CHUNK_SIZE)));
		s.inspectHasTarget = true;
		s.panels.chunkInspector = true;
	}

	ImGui::End();
}

} // namespace debugui
