#include <Engine/GameUI.hpp>
#include <Engine/UiTheme.hpp>

#include <cstdio>

namespace
{
template<class T>
void networkMetric(const char *label, std::optional<T> value, const char *format, const char *help)
{
	char text[48] = "Unavailable";
	if (value)
		std::snprintf(text, sizeof(text), format, *value);
	ImGui::TextWrapped("%s: %s", label, text);
	if (ImGui::BeginItemTooltip())
	{
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
		ImGui::TextUnformatted(help);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

using Milliseconds = std::chrono::duration<double, std::milli>;
}

void ui::drawMultiplayerNetworkStats(const NetworkStats &stats)
{
	const auto &ping = stats.ping;
	const auto &snapshot = stats.snapshot;
	const auto &traffic = stats.traffic;
	const auto &prediction = stats.prediction;
	const bool corrected = prediction && prediction->corrections > 0;
	const auto loss = snapshot ? snapshot->missingFraction : std::nullopt;

	networkMetric("Ping (estimated RTT)", ping ? std::optional{Milliseconds(ping->roundTrip).count()} : std::nullopt,
		"%.1f ms", "Smoothed round-trip time from a successful join or a gameplay action acknowledgment.");
	networkMetric("Last ping sample", ping ? std::optional{ping->sampleAge.count()} : std::nullopt,
		"%.1f s ago", "Time since the last accepted round-trip sample. Hotbar changes or block actions can refresh it.");
	networkMetric("Snapshot age", snapshot ? std::optional{Milliseconds(snapshot->age).count()} : std::nullopt,
		"%.0f ms", "Time since this client received the latest player snapshot.");
	networkMetric("Missing snapshots (estimate)", loss ? std::optional{100.0 * *loss} : std::nullopt,
		"%.1f%%", "Gaps in the observed player snapshot stream this session. Includes skipped server captures and snapshots coalesced by the client.");
	networkMetric("Receive", traffic ? std::optional{traffic->receiveKiB} : std::nullopt,
		"%.1f KiB/s", "Incoming UDP payload traffic over the last completed sample window.");
	networkMetric("Send", traffic ? std::optional{traffic->sendKiB} : std::nullopt,
		"%.1f KiB/s", "Outgoing UDP payload traffic over the last completed sample window.");
	networkMetric("Pending predicted inputs", prediction ? std::optional{prediction->pendingInputs} : std::nullopt,
		"%zu", "Recent local movement inputs still in the prediction replay queue.");
	networkMetric("Corrections", prediction ? std::optional{static_cast<unsigned long long>(prediction->corrections)} : std::nullopt,
		"%llu", "Movement corrections applied to your predicted player this session.");
	networkMetric("Correction rate", stats.correctionsPerSecond(),
		"%.1f/s", "Average local movement corrections per second since the first player snapshot.");
	networkMetric("Last correction", corrected ? std::optional{prediction->lastCorrection} : std::nullopt,
		"%.3f m", "Size of the most recent local movement correction.");
	networkMetric("Largest correction", corrected ? std::optional{prediction->maxCorrection} : std::nullopt,
		"%.3f m", "Largest local movement correction this session.");
	networkMetric("Hard resets", prediction ? std::optional{static_cast<unsigned long long>(prediction->hardResets)} : std::nullopt,
		"%llu", "Times movement prediction discarded its pending state and reset.");
}

void GameUI::drawMultiplayerPanel(const GameUIFrame &frame)
{
	const float scale = ui::effectiveScale(frame.uiScale);
	const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
	ImGui::SetNextWindowSize(ImVec2(std::min(ui::scaled(390.f, scale), work.x * 0.9f),
								   std::min(ui::scaled(540.f, scale), work.y * 0.9f)),
							ImGuiCond_FirstUseEver);
	if (!ImGui::Begin(ui::windows::kMultiplayer, &m_debug.panels.multiplayer))
	{
		ImGui::End();
		return;
	}

	if (ImGui::CollapsingHeader("Join", ImGuiTreeNodeFlags_DefaultOpen))
	{
		const auto &status = *frame.multiplayerStatus;
		ImGui::TextWrapped("%s", status.label);
		if (status.detail)
			ImGui::TextWrapped("%s", status.detail);
		ImGui::TextWrapped("Server endpoint: %.*s", static_cast<int>(frame.multiplayerEndpoint.size()),
			frame.multiplayerEndpoint.data());
		ImGui::TextWrapped("Choose a server with --connect at launch.");

		const bool reconnect = static_cast<bool>(frame.joinServer);
		const auto &action = reconnect ? frame.joinServer : frame.leaveServer;
		ImGui::BeginDisabled(!action);
		if (ImGui::Button(reconnect ? "Reconnect" : "Disconnect") && action)
			action();
		ImGui::EndDisabled();
	}
	if (ImGui::CollapsingHeader("Network", ImGuiTreeNodeFlags_DefaultOpen))
		ui::drawMultiplayerNetworkStats(frame.networkStats);
	if (ImGui::CollapsingHeader("Host"))
		ImGui::TextWrapped("In-game hosting is unavailable.");
	if (ImGui::CollapsingHeader("Players"))
		ImGui::TextWrapped("The server roster is unavailable.");
	if (ImGui::CollapsingHeader("Administration"))
		ImGui::TextWrapped("Server controls are unavailable.");
	ImGui::End();
}
