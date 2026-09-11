// Memory & workload view (issue #179): surfaces WorkloadTelemetry
// interactively via the non-destructive sampleLive() path plus main-thread
// arena/staging samples. Current/peak columns; progress bars only where a
// real capacity denominator exists. Event "since refresh" deltas are taken
// between the 10 Hz UI samples (not benchmark capture windows).

#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/GameUI.hpp>
#include <Engine/DebugUI/DebugPanelUtil.hpp>

#include <imgui/imgui.h>

namespace debugui
{
namespace
{
void bytesRow(const char *label, uint64_t current, uint64_t peak, uint64_t capacity = 0)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(label);
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(formatBytes(current).c_str());
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(formatBytes(peak).c_str());
	ImGui::TableNextColumn();
	if (capacity > 0)
	{
		ImGui::ProgressBar(capacity ? float(current) / float(capacity) : 0.f,
						   ImVec2(-1.f, 12.f), "");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("capacity %s", formatBytes(capacity).c_str());
	}
	else
	{
		ImGui::TextDisabled("—");
	}
}

void countRow(const char *label, uint64_t current, uint64_t peak)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(label);
	ImGui::TableNextColumn();
	ImGui::Text("%llu", static_cast<unsigned long long>(current));
	ImGui::TableNextColumn();
	ImGui::Text("%llu", static_cast<unsigned long long>(peak));
	ImGui::TableNextColumn();
	ImGui::TextDisabled("—");
}

void eventRow(const char *label, uint64_t total, uint64_t delta, bool bytes = false)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(label);
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(bytes ? formatBytes(total).c_str() : formatCount(total).c_str());
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(bytes ? formatBytes(delta).c_str() : formatCount(delta).c_str());
	ImGui::TableNextColumn();
	ImGui::TextDisabled("—");
}

void stageTable(const char *familyLabel, const telemetry::Registry::LiveSnapshot &live, size_t family)
{
	if (!ImGui::CollapsingHeader(familyLabel))
		return;
	if (ImGui::BeginTable("stages", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
	{
		ImGui::TableSetupColumn("Stage");
		ImGui::TableSetupColumn("calls", ImGuiTableColumnFlags_WidthFixed, 90.f);
		ImGui::TableSetupColumn("avg us/call", ImGuiTableColumnFlags_WidthFixed, 100.f);
		ImGui::TableHeadersRow();
		for (size_t i = 0; i < telemetry::StageCount; ++i)
		{
			const uint64_t calls = live.stageCalls[family][i];
			const uint64_t ns = live.stageNs[family][i];
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(telemetry::stageNames[i]);
			ImGui::TableNextColumn();
			ImGui::Text("%llu", static_cast<unsigned long long>(calls));
			ImGui::TableNextColumn();
			if (calls)
				ImGui::Text("%.1f", double(ns) / double(calls) / 1e3);
			else
				ImGui::TextDisabled("—");
		}
		ImGui::EndTable();
	}
	ImGui::TextDisabled("Cumulative since the last benchmark capture / world reload.");
}
} // namespace

void drawMemory(UiState &s, GameUIFrame &frame)
{
	ImGui::SetNextWindowSize(ImVec2(560, 660), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Memory", &s.panels.memory))
	{
		ImGui::End();
		return;
	}

	const auto &m = s.memory;
	if (!m.telemetryEnabled)
	{
		ImGui::TextDisabled("Telemetry disabled (FT_VOX_TELEMETRY=0) — live values unavailable.");
		ImGui::End();
		return;
	}

	const auto &g = m.live.current;
	const auto &pk = m.live.peak;
	const auto &d = s.eventDelta;

	// --- CPU voxel data + pools ---
	if (ImGui::CollapsingHeader("CPU voxel data & pools", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::BeginTable("cpu", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("CPU voxel / pools", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("current", ImGuiTableColumnFlags_WidthFixed, 84.f);
			ImGui::TableSetupColumn("peak", ImGuiTableColumnFlags_WidthFixed, 84.f);
			ImGui::TableSetupColumn("fill", ImGuiTableColumnFlags_WidthFixed, 110.f);
			ImGui::TableHeadersRow();

			bytesRow("Voxels (active chunks)", g[telemetry::VoxelBytes], pk[telemetry::VoxelBytes]);
			bytesRow("Neighbor shells", g[telemetry::ShellBytes], pk[telemetry::ShellBytes],
					 g[telemetry::ShellCapacity]);
			bytesRow("Columns + occupancy",
					 g[telemetry::ColumnBytes] + g[telemetry::OccupancyBytes],
					 pk[telemetry::ColumnBytes] + pk[telemetry::OccupancyBytes]);
			countRow("Chunk slots capacity / acquired / free",
					 g[telemetry::PoolCapacity], pk[telemetry::PoolCapacity]);
			countRow("Voxel pool blocks", g[telemetry::VoxelPoolActive], pk[telemetry::VoxelPoolActive]);
			bytesRow("Voxel pool retained", g[telemetry::VoxelPoolCapacityBytes],
					 pk[telemetry::VoxelPoolCapacityBytes]);
			countRow("Border pool blocks", g[telemetry::BorderPoolActive], pk[telemetry::BorderPoolActive]);
			bytesRow("Border pool retained", g[telemetry::BorderPoolCapacityBytes],
					 pk[telemetry::BorderPoolCapacityBytes]);
			ImGui::EndTable();
		}
	}

	// --- CPU mesh payload ---
	if (ImGui::CollapsingHeader("CPU mesh payload (in-flight / pooled)", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::BeginTable("cpumesh", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("CPU mesh", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("current", ImGuiTableColumnFlags_WidthFixed, 84.f);
			ImGui::TableSetupColumn("peak", ImGuiTableColumnFlags_WidthFixed, 84.f);
			ImGui::TableSetupColumn("fill", ImGuiTableColumnFlags_WidthFixed, 110.f);
			ImGui::TableHeadersRow();
			bytesRow("Opaque vertices", g[telemetry::OpaqueVertexBytes], pk[telemetry::OpaqueVertexBytes],
					 g[telemetry::OpaqueVertexCapacity]);
			bytesRow("Opaque indices", g[telemetry::OpaqueIndexBytes], pk[telemetry::OpaqueIndexBytes],
					 g[telemetry::OpaqueIndexCapacity]);
			bytesRow("Water vertices", g[telemetry::WaterVertexBytes], pk[telemetry::WaterVertexBytes],
					 g[telemetry::WaterVertexCapacity]);
			bytesRow("Water indices", g[telemetry::WaterIndexBytes], pk[telemetry::WaterIndexBytes],
					 g[telemetry::WaterIndexCapacity]);
			bytesRow("Mesh result pool retained", g[telemetry::MeshPoolCapacityBytes],
					 pk[telemetry::MeshPoolCapacityBytes]);
			countRow("Mesh result blocks", g[telemetry::MeshPoolActive], pk[telemetry::MeshPoolActive]);
			countRow("Light storage blocks", g[telemetry::LightPoolActive], pk[telemetry::LightPoolActive]);
			bytesRow("Light storage retained", g[telemetry::LightPoolCapacityBytes],
					 pk[telemetry::LightPoolCapacityBytes]);
			ImGui::EndTable();
		}
	}

	// --- GPU mesh resources + arena ---
	if (ImGui::CollapsingHeader("GPU mesh resources & arena", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (ImGui::BeginTable("gpu", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("GPU mesh", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("current", ImGuiTableColumnFlags_WidthFixed, 84.f);
			ImGui::TableSetupColumn("peak", ImGuiTableColumnFlags_WidthFixed, 84.f);
			ImGui::TableSetupColumn("fill", ImGuiTableColumnFlags_WidthFixed, 110.f);
			ImGui::TableHeadersRow();
			bytesRow("Opaque vertices", g[telemetry::GpuOpaqueVertex], pk[telemetry::GpuOpaqueVertex]);
			bytesRow("Opaque indices", g[telemetry::GpuOpaqueIndex], pk[telemetry::GpuOpaqueIndex]);
			bytesRow("Water vertices", g[telemetry::GpuWaterVertex], pk[telemetry::GpuWaterVertex]);
			bytesRow("Water indices", g[telemetry::GpuWaterIndex], pk[telemetry::GpuWaterIndex]);
			bytesRow("GPU mesh live (all streams)", g[telemetry::GpuLiveBytes], pk[telemetry::GpuLiveBytes]);
			bytesRow("Retired (awaiting deferred destroy)", g[telemetry::RetiredBytes],
					 pk[telemetry::RetiredBytes]);
			countRow("Retired buffers", g[telemetry::RetiredBuffers], pk[telemetry::RetiredBuffers]);
			ImGui::EndTable();
		}
		ImGui::Text("Arena: %u pages  |  live %s  |  free %s  |  high-water %s",
					unsigned(m.arenaPages), formatBytes(m.arenaLiveBytes).c_str(),
					formatBytes(m.arenaFreeBytes).c_str(),
					formatBytes(m.arenaHighWaterBytes).c_str());
		const uint64_t arenaTotal = m.arenaLiveBytes + m.arenaFreeBytes;
		if (arenaTotal > 0)
			ImGui::ProgressBar(float(m.arenaLiveBytes) / float(arenaTotal), ImVec2(-1.f, 0.f), "");
		ImGui::TextDisabled("Arena utilization (live / live+free), 10 Hz");
		plotHistory("##mem_arena", s.arenaUtilization, 0.f, ImVec2(-1.f, 44.f));
	}

	// --- Upload / staging ---
	if (ImGui::CollapsingHeader("Upload & staging", ImGuiTreeNodeFlags_DefaultOpen))
	{
		if (m.stagingCapacityBytes > 0)
		{
			ImGui::Text("Staging slice: %s of %s (last completed frame)",
						formatBytes(m.stagingUsedBytes).c_str(),
						formatBytes(m.stagingCapacityBytes).c_str());
			ImGui::ProgressBar(float(m.stagingUsedBytes) / float(m.stagingCapacityBytes),
							   ImVec2(-1.f, 0.f), "");
		}
		else
		{
			ImGui::Text("Staging slice used: %s", formatBytes(m.stagingUsedBytes).c_str());
		}
		ImGui::TextDisabled("Staging usage history (10 Hz)");
		plotHistory("##mem_staging", s.stagingUsed, 0.f, ImVec2(-1.f, 44.f));

		if (ImGui::BeginTable("updevents", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("Event (since world load)", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("total", ImGuiTableColumnFlags_WidthFixed, 90.f);
			ImGui::TableSetupColumn("per refresh", ImGuiTableColumnFlags_WidthFixed, 90.f);
			ImGui::TableHeadersRow();
			eventRow("Chunks uploaded", m.live.events[telemetry::UploadChunks],
					 d[telemetry::UploadChunks]);
			eventRow("Vertex bytes uploaded", m.live.events[telemetry::UploadVertexBytes],
					 d[telemetry::UploadVertexBytes], true);
			eventRow("Index bytes uploaded", m.live.events[telemetry::UploadIndexBytes],
					 d[telemetry::UploadIndexBytes], true);
			eventRow("Deferred upload attempts", m.live.events[telemetry::UploadDeferred],
					 d[telemetry::UploadDeferred]);
			eventRow("Staging allocation failures", m.live.events[telemetry::StagingFailures],
					 d[telemetry::StagingFailures]);
			ImGui::EndTable();
		}
	}

	// --- Workload / draws / growth ---
	if (ImGui::CollapsingHeader("Workload events"))
	{
		if (ImGui::BeginTable("wlevents", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("Event (since world load)", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("total", ImGuiTableColumnFlags_WidthFixed, 90.f);
			ImGui::TableSetupColumn("per refresh", ImGuiTableColumnFlags_WidthFixed, 90.f);
			ImGui::TableHeadersRow();
			eventRow("Opaque draws", m.live.events[telemetry::OpaqueDraws], d[telemetry::OpaqueDraws]);
			eventRow("Water draws", m.live.events[telemetry::WaterDraws], d[telemetry::WaterDraws]);
			eventRow("Shadow draws (cascade 0)", m.live.events[telemetry::Shadow0], d[telemetry::Shadow0]);
			eventRow("Shadow draws (cascade 1)", m.live.events[telemetry::Shadow1], d[telemetry::Shadow1]);
			eventRow("Shadow draws (cascade 2)", m.live.events[telemetry::Shadow2], d[telemetry::Shadow2]);
			eventRow("GPU allocations created", m.live.events[telemetry::AllocCreated],
					 d[telemetry::AllocCreated]);
			eventRow("GPU allocations destroyed", m.live.events[telemetry::AllocDestroyed],
					 d[telemetry::AllocDestroyed]);
			eventRow("Chunk pool rejects", m.live.events[telemetry::PoolRejected],
					 d[telemetry::PoolRejected]);
			eventRow("Voxel pool growths", m.live.events[telemetry::VoxelPoolGrow],
					 d[telemetry::VoxelPoolGrow]);
			eventRow("Border pool growths", m.live.events[telemetry::BorderPoolGrow],
					 d[telemetry::BorderPoolGrow]);
			eventRow("Mesh pool growths", m.live.events[telemetry::MeshPoolGrow],
					 d[telemetry::MeshPoolGrow]);
			eventRow("Arena growths", m.live.events[telemetry::ArenaGrow], d[telemetry::ArenaGrow]);
			ImGui::EndTable();
		}
		ImGui::TextDisabled("Draw counts per refresh are deltas between 10 Hz UI samples,\n"
							"not benchmark windows. Gauges also feed the benchmark report.");
	}

	// --- Mesh stage timings ---
	if (ImGui::CollapsingHeader("Mesh / light-cache stage timings", ImGuiTreeNodeFlags_DefaultOpen))
	{
		stageTable("Mesh builds (skylight/blocklight/occupancy/faces/LOD)", m.live, telemetry::MeshFamily);
		stageTable("Light-cache-only builds", m.live, telemetry::LightCacheFamily);
	}

	ImGui::End();
}

} // namespace debugui
