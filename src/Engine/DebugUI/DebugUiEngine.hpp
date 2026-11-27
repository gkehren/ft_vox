#pragma once

// Engine-facing bridge for the developer console (issue #179): builds the
// read-only debug snapshots in UiState from the per-frame DeveloperUiFrame and
// engine getters. Panels (DebugPanels.hpp) only consume UiState + the
// explicit settings structs; they never reach into RenderWorldBridge/telemetry
// internals directly.

#include <Engine/DebugUI/DebugUiCore.hpp>
#include <Engine/DeveloperUi.hpp>

class Profiler;
struct GpuFrameSample;

namespace debugui
{

/// Refresh snapshots + histories + health from the live frame. Call once per
/// frame from DeveloperUi::draw, before any panel. Heavy sampling (telemetry,
/// upload-backlog walk, history pushes) is throttled to 10 Hz and skipped
/// entirely when no consumer panel is open; `nowSeconds` is monotonic wall time.
void updateDebugUiState(UiState &state, const DeveloperUiFrame &frame, double nowSeconds);

/// Reconcile after the Performance controls mutate either global capture.
void syncDebugCpuCapture(UiState &state, const Profiler &profiler);
void syncDebugGpuCapture(UiState &state, const VkGpuProfiler *profiler, uint64_t cpuEpoch,
						 bool recordObservation = false);

/// Update the latest global GPU reading. Overview can append one observation
/// per UI sampling tick. A missing generation denotes unavailable capture.
void observeGpuCapture(UiState &state, std::optional<uint64_t> generation,
					   const GpuFrameSample *sample, bool recordObservation);

/// Extract the inspector view of one chunk (main-thread read of atomics and
/// main-thread-only chunk state; `loaded` is false for unknown coordinates).
ChunkDebugSnapshot makeChunkDebugSnapshot(const class RenderWorldBridge &chunks,
										  const glm::ivec3 &coord,
										  const glm::vec3 &cameraPos);

} // namespace debugui
