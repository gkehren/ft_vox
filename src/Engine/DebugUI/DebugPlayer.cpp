// Player/physics diagnostics window (issues #179/#184): the single console
// surface for raw player simulation data — solver step/cell/iteration
// counters, dropped fixed steps, motion flags and the low-level camera
// readout that used to clutter the old catch-all HUD. Renders exclusively
// from the debugui::UiState player snapshot; no engine pointers touched.

#include <Engine/DebugUI/DebugPanels.hpp>
#include <Engine/UiScale.hpp>
#include <Engine/UiTheme.hpp>
#include <Engine/PlayerUi.hpp>
#include <Engine/GameUI.hpp>

#include <imgui/imgui.h>

namespace debugui
{

void drawPlayerDiagnostics(UiState &s, GameUIFrame &frame)
{
	const float scale = ui::effectiveScale(frame.uiScale);
	ImGui::SetNextWindowSize(ImVec2(ui::scaled(340.f, scale), ui::scaled(320.f, scale)),
							 ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(ui::windows::kPlayerDiagnostics, &s.panels.playerDiagnostics))
	{
		ImGui::End();
		return;
	}

	if (!s.player.valid)
	{
		ImGui::TextDisabled("Waiting for the first frame sample…");
		ImGui::End();
		return;
	}

	const auto &p = s.player;

	ImGui::SeparatorText("Motion");
	// Label is a string literal -> data() is null-terminated (printf-safe).
	ImGui::Text("%s · %.2f blocks/s",
				playerui::playerMotionLabel(p.flight, p.waitingForTerrain, p.swimming,
											p.grounded)
					.data(),
				p.speed);
	ImGui::Text("Position  %.2f  %.2f  %.2f", p.position.x, p.position.y, p.position.z);
	ImGui::Text("Look  yaw %.1f°  pitch %.1f°", p.yaw, p.pitch);
	if (ImGui::BeginTable("player_flags", 2, ImGuiTableFlags_SizingStretchSame))
	{
		ImGui::TableNextColumn();
		ImGui::BulletText("Flight %s", p.flight ? "on" : "off");
		ImGui::TableNextColumn();
		ImGui::BulletText("Grounded %s", p.grounded ? "yes" : "no");
		ImGui::TableNextColumn();
		ImGui::BulletText("Swimming %s", p.swimming ? "yes" : "no");
		ImGui::TableNextColumn();
		ImGui::BulletText("Waiting terrain %s", p.waitingForTerrain ? "yes" : "no");
		ImGui::EndTable();
	}

	// Raw collision-solver counters (physics::PhysicsMetrics + QueryStats).
	ImGui::SeparatorText("Physics solver");
	ImGui::Text("Fixed steps this frame: %u", p.physicsSteps);
	ImGui::Text("Queried cells: %llu", static_cast<unsigned long long>(p.queriedCells));
	ImGui::Text("Query iterations: %llu",
				static_cast<unsigned long long>(p.queryIterations));
	ImGui::Text("Dropped fixed steps: %llu",
				static_cast<unsigned long long>(p.droppedSteps));
	if (p.submergedWater || p.submergedLava)
		ImGui::TextDisabled("Immersion: %s%s", p.submergedWater ? "water " : "",
							p.submergedLava ? "lava" : "");

	ImGui::SeparatorText("Camera");
	// Snapshot-only (issue #184 review): camera view state arrives through
	// the player snapshot like every other value — no direct Camera access.
	ImGui::Text("Mode  %s",
				p.cameraViewMode == playerui::CameraViewMode::Isometric ? "Isometric"
																		: "Perspective");
	ImGui::Text("Movement speed  %.1f", p.cameraMovementSpeed);
	ImGui::Text("Mouse sensitivity  %.3f", p.mouseSensitivity);
	if (p.cameraViewMode == playerui::CameraViewMode::Isometric)
		ImGui::Text("Isometric zoom  %.0f", p.isometricZoom);

	ImGui::End();
}

} // namespace debugui
