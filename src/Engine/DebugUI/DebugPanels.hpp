#pragma once

// Developer-tool panel entry points (issue #179). Each panel owns one
// domain window, renders from the read-only snapshots in debugui::UiState,
// and mutates only through the explicit settings structs carried by
// GameUIFrame (RenderSettings / ShaderParameters / PostProcessSettings) or
// the frame callbacks. ImGui backend/platform integration stays in
// ImGuiLayer; window shell/shortcuts stay in GameUI.

#include <Engine/DebugUI/DebugUiCore.hpp>

struct GameUIFrame;

namespace debugui
{

/// Compact health dashboard: frame, streaming, memory and sustained
/// health-warning indicators (F8).
void drawOverview(UiState &state, GameUIFrame &frame);

/// Graphics tuning (F2, "Graphics"): category navigation (General / Display /
/// Lighting / Atmosphere / Shadows / Water / Post / Resources, issue #185)
/// over quality preset, resource pack, atmosphere, lighting, water/shadow/
/// post settings. Settings only — diagnostic views live in drawRenderDebug.
void drawRendering(UiState &state, GameUIFrame &frame);

/// Rendering diagnostics (F12): debug views (shadow/water/SSAO), exposure
/// readout, active-pass pipeline summary with per-pass GPU cost.
void drawRenderDebug(UiState &state, GameUIFrame &frame);

/// Streaming settings + live queue/pool telemetry (F3).
void drawStreaming(UiState &state, GameUIFrame &frame);

/// CPU/GPU profiling (F7, "Performance"): frame history, scope table with
/// current/avg/peak + per-scope graphs, GPU pass view, worker jobs, spikes.
void drawPerformance(UiState &state, GameUIFrame &frame);

/// Player/physics diagnostics (Developer menu): raw solver counters, motion
/// flags and camera readout removed from the gameplay surfaces (issue #184).
void drawPlayerDiagnostics(UiState &state, GameUIFrame &frame);

/// Chunk-centric inspector (F9): lifecycle/mesh/light-cache/upload state
/// for the chunk under the player/target or manual coordinates, plus the
/// opt-in bounded lifecycle event trace.
void drawChunkInspector(UiState &state, GameUIFrame &frame);

/// Memory & workload (F11): CPU pools, GPU mesh resources, mesh arena,
/// staging/retire queues, allocation/growth events and mesh stage timings.
void drawMemory(UiState &state, GameUIFrame &frame);

/// Benchmark setup/progress (window from menu or Performance panel).
void drawBenchmarkPanel(UiState &state, GameUIFrame &frame);

/// Scored report window (opened via benchmark state).
void drawBenchmarkReport(UiState &state, GameUIFrame &frame);

} // namespace debugui
