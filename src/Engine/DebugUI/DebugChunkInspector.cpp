// Chunk inspector (issue #179): lifecycle, meshing, light-cache and GPU
// upload state for a single chunk — under the player, under the voxel
// highlight, or manual coordinates. Includes the opt-in, globally bounded
// lifecycle event trace: when disabled it costs one branch per event site;
// when enabled it never grows beyond ChunkManager::kChunkEventRingSize.

#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/DebugUI/DebugPanelUtil.hpp>
#include <Engine/DebugUI/DebugUiEngine.hpp>
#include <Chunk/ChunkManager.hpp>

#include <imgui/imgui.h>

#include <cmath>
#include <bitset>

namespace debugui
{
namespace
{
const char *chunkStateName(int state)
{
	switch (state)
	{
	case ChunkState::UNLOADED: return "UNLOADED";
	case ChunkState::GENERATED: return "GENERATED";
	case ChunkState::MESHED: return "MESHED";
	default: return "?";
	}
}

const char *eventKindName(const char *kind)
{
	// Static event kinds -> friendly labels (unknown kinds pass through).
	if (!kind) return "?";
	return kind;
}
} // namespace

void drawChunkInspector(UiState &s, GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(460, 560), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Chunk Inspector", &s.panels.chunkInspector))
	{
		ImGui::End();
		return;
	}

	if (!frame.chunks || !frame.camera)
	{
		ImGui::TextDisabled("Chunk manager not ready.");
		ImGui::End();
		return;
	}
	ChunkManager &chunks = *frame.chunks;
	const glm::vec3 camPos = frame.camera->getPosition();

	// Selection: chunk under the player by default.
	const glm::ivec3 playerChunk(int(std::floor(camPos.x / CHUNK_SIZE)), 0,
								 int(std::floor(camPos.z / CHUNK_SIZE)));
	if (!s.inspectHasTarget)
	{
		s.inspectCoord = playerChunk;
		s.inspectHasTarget = true;
	}

	ImGui::SeparatorText("Select chunk");
	if (ImGui::Button("Under player"))
		s.inspectCoord = playerChunk;
	ImGui::SameLine();
	if (frame.highlight && frame.highlight->active)
	{
		if (ImGui::Button("Under target"))
			s.inspectCoord = glm::ivec3(int(std::floor(frame.highlight->position.x / CHUNK_SIZE)), 0,
										int(std::floor(frame.highlight->position.z / CHUNK_SIZE)));
	}
	else
	{
		ImGui::BeginDisabled();
		ImGui::Button("Under target");
		ImGui::EndDisabled();
	}
	int x = s.inspectCoord.x, z = s.inspectCoord.z;
	if (ImGui::InputInt("##cx", &x))
		s.inspectCoord.x = x;
	ImGui::SameLine();
	if (ImGui::InputInt("##cz", &z))
		s.inspectCoord.z = z;
	ImGui::SameLine();
	ImGui::TextDisabled("X / Z (Y=0)");

	s.inspectCoord.x = std::clamp(s.inspectCoord.x, -4096, 4096);
	s.inspectCoord.z = std::clamp(s.inspectCoord.z, -4096, 4096);

	// Opt-in bounded trace. The flag lives in UiState (it survives world
	// reloads); updateDebugUiState re-applies it to the ChunkManager every
	// frame — even with this panel closed — so a recreated manager inherits
	// it. This checkbox only flips the UI-owned flag.
	ImGui::Checkbox("Trace lifecycle events (bounded ring of 256)", &s.eventTraceEnabled);

	// --- Selected chunk state ---
	ImGui::SeparatorText("State");
	ChunkDebugSnapshot snap = makeChunkDebugSnapshot(chunks, s.inspectCoord, camPos);
	if (!snap.loaded)
	{
		ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f),
						   "Chunk (%d, %d) is not loaded (distance %.0f m)",
						   s.inspectCoord.x, s.inspectCoord.z, snap.distance);
		ImGui::TextDisabled("It may be outside the view distance or not yet streamed.");
	}
	else
	{
		if (ImGui::BeginTable("chunkstate", 2,
							  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
								  ImGuiTableFlags_SizingStretchProp))
		{
			ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 170.f);
			ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			kvRow("Coordinates", "(%d, %d)  |  %.0f m from camera",
				  s.inspectCoord.x, s.inspectCoord.z, snap.distance);
			kvRow("Lifecycle state", "%s%s%s", chunkStateName(snap.state),
				  snap.inTransit ? "  + job in flight" : "",
				  snap.lodMesh ? "  (LOD mesh)" : "");
			kvRow("Generation / revision", "%llu / %llu",
				  static_cast<unsigned long long>(snap.meshGeneration),
				  static_cast<unsigned long long>(snap.meshRevision));
			kvRow("Dirty sections", "0x%04x (%u of 16)",
				  unsigned(snap.dirtySections),
				  std::bitset<16>(snap.dirtySections).count());
			kvRow("Mesh result staged", "%s%s", snap.hasPendingMeshResult ? "yes (awaiting publish)" : "no",
				  snap.unuploadedFullMesh ? "  + full mesh unuploaded" : "");
			kvRow("GPU upload pending", "%s", snap.uploadPending ? "yes (awaiting copy)" : "no");
			kvRow("GPU sections live", "%u of 16", snap.liveGpuSections);
			kvRow("Geometry", "opaque %u idx / water %u idx (%u + %u draws)",
				  snap.opaqueIndexCount, snap.waterIndexCount,
				  snap.opaqueDrawCount, snap.waterDrawCount);
			kvRow("Light cache", "%s / %s",
				  snap.lightCacheWanted ? "wanted" : "not wanted",
				  snap.lightCachePresent ? "present" : "absent");
			kvRow("Pool slot / visibility", "#%u  |  %s",
				  snap.activeIndex, snap.visible ? "visible" : "hidden");
			ImGui::EndTable();
		}

		if (snap.dirtySections)
			ImGui::TextColored(ImVec4(1.f, 0.85f, 0.35f, 1.f),
							   "Sections dirty — remesh queued via GENERATED state.");
		if (snap.uploadPending && snap.inTransit)
			ImGui::TextDisabled("Upload pending but chunk in transit — will upload after the job lands.");
	}

	// --- Nearby chunks (bounded to a 5x5 neighborhood) ---
	ImGui::SeparatorText("Nearby chunks");
	if (ImGui::BeginTable("nearby", 5,
						  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
						  ImVec2(0.f, 160.f)))
	{
		ImGui::TableSetupColumn("chunk");
		ImGui::TableSetupColumn("state");
		ImGui::TableSetupColumn("rev");
		ImGui::TableSetupColumn("dist");
		ImGui::TableSetupColumn("flags");
		ImGui::TableHeadersRow();
		// Direct 5x5 lookups instead of scanning the whole active set:
		// O(25) map probes vs O(activeChunks) per frame (issue #179 review).
		for (int dz = -2; dz <= 2; ++dz)
		{
			for (int dx = -2; dx <= 2; ++dx)
			{
				const glm::ivec3 ci = s.inspectCoord + glm::ivec3(dx, 0, dz);
				const Chunk *chunk = chunks.getChunk(ci);
				if (!chunk)
					continue;
				const ChunkDebugSnapshot n = makeChunkDebugSnapshot(chunks, ci, camPos);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				char coordLabel[32];
				std::snprintf(coordLabel, sizeof(coordLabel), "(%d, %d)", ci.x, ci.z);
				if (ImGui::Selectable(coordLabel, ci == s.inspectCoord))
					s.inspectCoord = ci;
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(chunkStateName(n.state));
				ImGui::TableNextColumn();
				ImGui::Text("%llu", static_cast<unsigned long long>(n.meshRevision));
				ImGui::TableNextColumn();
				ImGui::Text("%.0f", n.distance);
				ImGui::TableNextColumn();
				ImGui::Text("%s%s%s", n.uploadPending ? "up" : "",
							n.dirtySections ? " dirty" : "",
							n.lightCachePresent ? " lc" : "");
			}
		}
		ImGui::EndTable();
	}

	// --- Event trace (filtered to the selected chunk) ---
	ImGui::SeparatorText("Event trace");
	if (!s.eventTraceEnabled)
	{
		ImGui::TextDisabled("Enable tracing above to record load/gen/mesh/edit/upload\n"
							"events (bounded ring, main-thread only).");
	}
	else
	{
		const std::vector<ChunkDebugEvent> events = chunks.chunkDebugEvents();
		const double nowSec = std::chrono::duration<double>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
		size_t matched = 0;
		// Show only the most recent 32 events for this chunk (newest last).
		constexpr size_t kMaxShown = 32;
		std::vector<const ChunkDebugEvent *> shown;
		for (auto it = events.rbegin(); it != events.rend(); ++it)
		{
			if (it->chunk.x != s.inspectCoord.x || it->chunk.z != s.inspectCoord.z)
				continue;
			++matched;
			if (shown.size() < kMaxShown)
				shown.push_back(&*it);
		}
		ImGui::Text("%zu events for this chunk (ring holds %zu total)", matched, events.size());
		for (auto it = shown.rbegin(); it != shown.rend(); ++it)
		{
			const ChunkDebugEvent *e = *it;
			ImGui::Text("%6.1fs ago  %-14s rev %llu  sections 0x%04x",
						nowSec - e->timeSec, eventKindName(e->kind),
						static_cast<unsigned long long>(e->revision), unsigned(e->sections));
		}
		if (matched == 0)
			ImGui::TextDisabled("No recorded events for this chunk yet.");
	}

	ImGui::End();
}

} // namespace debugui
