#include "Engine/GameUIResourcePack.hpp"
#include "Engine/GameUI.hpp"

#include <ImGuiFileDialog.h>
#include <imgui/imgui.h>

#include <cstdio>
#include <cstring>

namespace
{
	constexpr const char *kResourcePackDlgKey = "ChooseResourcePackZip";

	void tryApply(GameUIFrame &frame, ResourcePackUiState &state, const char *path)
	{
		if (!frame.applyResourcePack)
		{
			state.status = "Resource pack reload not available.";
			state.statusError = true;
			state.statusWarning = false;
			return;
		}

		const GameUIFrame::ResourcePackUiResult r = frame.applyResourcePack(path ? path : "");
		state.status = r.message.empty() ? (r.atlasOk ? "Done." : "Failed.") : r.message;
		state.statusError = r.isError;
		state.statusWarning = r.isWarning && !r.isError;

		if (frame.resourcePackRoot)
			state.lastActiveRoot = *frame.resourcePackRoot;
		// Keep the path the user typed when invalid so they can fix it; clear buffer only for bundled/default.
		if (r.atlasOk && path && path[0] == '\0')
			state.pathBuf[0] = '\0';
		if (r.atlasOk && frame.resourcePackRoot && !r.isError)
			std::snprintf(state.pathBuf, sizeof(state.pathBuf), "%s", frame.resourcePackRoot->c_str());
	}
} // namespace

void drawResourcePackSection(GameUIFrame &frame, ResourcePackUiState &state, bool &showGraphics)
{
	(void)showGraphics;
	if (!ImGui::CollapsingHeader("Resource pack", ImGuiTreeNodeFlags_DefaultOpen))
		return;

	// Resync edit buffer when the engine's authoritative path changes (not while editing an error path).
	const std::string activeRoot =
		(frame.resourcePackRoot && !frame.resourcePackRoot->empty()) ? *frame.resourcePackRoot : std::string{};
	if (activeRoot != state.lastActiveRoot && !state.statusError)
	{
		std::snprintf(state.pathBuf, sizeof(state.pathBuf), "%s", activeRoot.c_str());
		state.lastActiveRoot = activeRoot;
	}
	else if (activeRoot != state.lastActiveRoot)
	{
		state.lastActiveRoot = activeRoot;
	}

	const char *activeLabel = activeRoot.empty() ? "ressources/default-resource-pack.zip" : activeRoot.c_str();
	ImGui::Text("Active: %s", activeLabel);
	if (frame.worldRenderer)
		ImGui::Text("Atlas layer: %u×%u", frame.worldRenderer->textureLayerSize(),
					frame.worldRenderer->textureLayerSize());

	ImGui::InputText("Pack path (.zip)", state.pathBuf, sizeof(state.pathBuf));
	ImGui::TextDisabled("Select a .zip Minecraft resource pack file.");

	if (ImGui::Button("Browse"))
	{
		IGFD::FileDialogConfig config;
		config.path = state.pathBuf[0] != '\0' ? state.pathBuf : ".";
		config.countSelectionMax = 1;
		config.flags = ImGuiFileDialogFlags_Modal;
		ImGuiFileDialog::Instance()->OpenDialog(kResourcePackDlgKey, "Choose Resource Pack (.zip)", ".zip",
												config);
	}
	ImGui::SameLine();
	if (ImGui::Button("Apply pack"))
		tryApply(frame, state, state.pathBuf);
	ImGui::SameLine();
	if (ImGui::Button("Reset to default"))
	{
		state.pathBuf[0] = '\0';
		tryApply(frame, state, "");
	}

	if (!state.status.empty())
	{
		if (state.statusError)
			ImGui::TextColored(ImVec4(1.f, 0.35f, 0.3f, 1.f), "%s", state.status.c_str());
		else if (state.statusWarning)
			ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f), "%s", state.status.c_str());
		else
			ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.45f, 1.f), "%s", state.status.c_str());
	}
}

void displayResourcePackFileDialog(ResourcePackUiState &state, bool &showGraphics)
{
	const ImGuiIO &io = ImGui::GetIO();
	const ImVec2 maxSize(io.DisplaySize.x * 0.85f, io.DisplaySize.y * 0.85f);
	const ImVec2 minSize(maxSize.x * 0.45f, maxSize.y * 0.45f);

	if (!ImGuiFileDialog::Instance()->Display(kResourcePackDlgKey, ImGuiWindowFlags_NoCollapse, minSize, maxSize))
		return;

	if (ImGuiFileDialog::Instance()->IsOk())
	{
		const std::string filePath = ImGuiFileDialog::Instance()->GetFilePathName();
		if (!filePath.empty())
		{
			std::snprintf(state.pathBuf, sizeof(state.pathBuf), "%s", filePath.c_str());
			state.status = "Path selected — click Apply pack to load.";
			state.statusError = false;
			state.statusWarning = false;
			showGraphics = true;
		}
	}
	ImGuiFileDialog::Instance()->Close();
}
