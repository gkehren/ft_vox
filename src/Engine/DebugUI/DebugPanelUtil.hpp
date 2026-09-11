#pragma once

// Small ImGui helpers shared by the developer panels (issue #179).
// Panel-only utilities: the pure logic they build on lives in DebugUiCore.

#include <Engine/DebugUI/DebugUiCore.hpp>
#include <imgui/imgui.h>

namespace debugui
{

/// Chronological (oldest-first) scratch copy of a history for ImGui plots.
/// Panels all run on the main thread; the buffer is reused per call.
inline const float *orderedHistory(const MetricHistory &h)
{
	static std::array<float, kMetricHistorySize> buf;
	h.copyOrdered(buf.data());
	return buf.data();
}

inline void plotHistory(const char *label, const MetricHistory &h, float scaleMin, ImVec2 size)
{
	if (h.count() == 0)
	{
		ImGui::TextDisabled("no samples");
		return;
	}
	ImGui::PlotLines(label, orderedHistory(h), static_cast<int>(h.count()), 0,
					 nullptr, scaleMin, FLT_MAX, size);
}

inline ImVec4 healthColor(bool bad)
{
	return bad ? ImVec4(1.f, 0.4f, 0.35f, 1.f) : ImVec4(0.55f, 0.9f, 0.55f, 1.f);
}

/// Value row helper for state tables.
inline void kvRow(const char *k, const char *fmt, auto... args)
{
	ImGui::TableNextRow();
	ImGui::TableNextColumn();
	ImGui::TextUnformatted(k);
	ImGui::TableNextColumn();
	ImGui::Text(fmt, args...);
}

} // namespace debugui
