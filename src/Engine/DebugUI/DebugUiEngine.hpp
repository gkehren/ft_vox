#pragma once

// Engine-facing bridge for the developer console (issue #179): builds the
// read-only debug snapshots in UiState from the per-frame GameUIFrame and
// engine getters. Panels (DebugPanels.hpp) only consume UiState + the
// explicit settings structs; they never reach into ChunkManager/telemetry
// internals directly.

#include <Engine/DebugUI/DebugUiCore.hpp>
#include <Engine/GameUI.hpp>

namespace debugui
{

/// Refresh snapshots + histories + health from the live frame. Call once per
/// frame from GameUI::draw, before any panel. Heavy sampling (telemetry,
/// upload-backlog walk, history pushes) is throttled to 10 Hz and skipped
/// entirely when no consumer panel is open; `nowSeconds` is the UI time base
/// (ImGui::GetTime()).
void updateDebugUiState(UiState &state, const GameUIFrame &frame, double nowSeconds);

/// Extract the inspector view of one chunk (main-thread read of atomics and
/// main-thread-only chunk state; `loaded` is false for unknown coordinates).
ChunkDebugSnapshot makeChunkDebugSnapshot(const class ChunkManager &chunks,
										  const glm::ivec3 &coord,
										  const glm::vec3 &cameraPos);

} // namespace debugui
