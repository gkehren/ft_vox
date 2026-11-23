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

template<size_t N>
void setJoinField(std::array<char, N> &field, std::string_view text)
{
	const auto count = std::min(text.size(), N - 1);
	std::copy_n(text.begin(), count, field.begin());
	field[count] = '\0';
}

void endpointChoices(const char *label, const std::vector<net::udp::Address> &endpoints,
	std::array<char, 64> &field)
{
	if (endpoints.empty())
		return;
	ImGui::SetNextItemWidth(-1.f);
	ImGui::PushID(label);
	if (ImGui::BeginCombo("##endpoints", label, ImGuiComboFlags_HeightSmall))
	{
		for (auto address : endpoints)
		{
			const auto endpoint = client::formatEndpoint(address);
			if (ImGui::Selectable(endpoint.c_str(), endpoint == field.data()))
				setJoinField(field, endpoint);
		}
		ImGui::EndCombo();
	}
	ImGui::PopID();
}
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
		drawJoinPanel(frame);
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

void GameUI::drawJoinPanel(const GameUIFrame &frame)
{
	const auto &prefs = *frame.joinPreferences;
	if (!m_joinDraftInitialized)
	{
		if (!frame.multiplayerEndpoint.empty())
			setJoinField(m_joinEndpoint, frame.multiplayerEndpoint);
		else if (!prefs.recent.empty())
			setJoinField(m_joinEndpoint, client::formatEndpoint(prefs.recent.front()));
		else
			setJoinField(m_joinEndpoint, "127.0.0.1:7777");
		setJoinField(m_profileName, prefs.selectedProfile);
		m_joinDraftInitialized = true;
	}
	if (frame.multiplayerStatus)
	{
		const auto &status = *frame.multiplayerStatus;
		ImGui::TextWrapped("%s", status.label);
		if (status.detail)
			ImGui::TextWrapped("%s", status.detail);
		ImGui::TextWrapped("Server endpoint: %.*s", static_cast<int>(frame.multiplayerEndpoint.size()),
			frame.multiplayerEndpoint.data());
		ImGui::TextWrapped("Session profile: %.*s", static_cast<int>(frame.multiplayerProfile.size()),
			frame.multiplayerProfile.data());
		if (ImGui::Button("Copy endpoint"))
			ImGui::SetClipboardText(std::string(frame.multiplayerEndpoint).c_str());
		if (frame.leaveServer && ImGui::Button("Disconnect"))
			frame.leaveServer();
		if (frame.joinServer && ImGui::Button("Reconnect"))
			frame.joinServer();
		if (frame.returnToSolo && ImGui::Button("Return to solo"))
			frame.returnToSolo();
	}
	else
		ImGui::TextUnformatted("Disconnected");
	if (frame.switchingSession)
		ImGui::TextWrapped("Finishing the previous session.");

	ImGui::Separator();
	ImGui::BeginDisabled(frame.switchingSession);
	endpointChoices("Favorite servers", prefs.favorites, m_joinEndpoint);
	endpointChoices("Recent servers", prefs.recent, m_joinEndpoint);
	ImGui::TextUnformatted("Server address and port");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputTextWithHint("##endpoint", "127.0.0.1:7777", m_joinEndpoint.data(), m_joinEndpoint.size());
	if (ImGui::Button("Toggle favorite"))
		frame.toggleFavorite(m_joinEndpoint.data());
	ImGui::SetItemTooltip("Add or remove this address in Favorite servers.");
	ImGui::TextUnformatted("Expected world seed (optional)");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("##seed", m_joinSeed.data(), m_joinSeed.size());

	if (!frame.joinProfiles.empty())
	{
		ImGui::SetNextItemWidth(-1.f);
		if (ImGui::BeginCombo("##profiles", "Saved profiles", ImGuiComboFlags_HeightSmall))
		{
			for (const std::string &profile : frame.joinProfiles)
			{
				if (ImGui::Selectable(profile.c_str(), profile == m_profileName.data()))
				{
					setJoinField(m_profileName, profile);
					frame.selectJoinProfile(profile);
				}
			}
			ImGui::EndCombo();
		}
	}
	ImGui::TextUnformatted("Local profile name");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("##profile", m_profileName.data(), m_profileName.size());
	if (ImGui::Button("Create profile"))
		frame.createJoinProfile(m_profileName.data());
	ImGui::SetItemTooltip("Create an identity for a new name. Reuse that name to resume the same player.");
	if (ImGui::Button(frame.leaveServer ? "Switch server or profile" : "Connect"))
		frame.connectServer({m_joinEndpoint.data(), m_profileName.data(), m_joinSeed.data()});
	ImGui::EndDisabled();
	if (!frame.joinError.empty())
		ImGui::TextWrapped("%s", frame.joinError.c_str());
	if (!frame.joinPreferencesError.empty())
		ImGui::TextWrapped("%s", frame.joinPreferencesError.c_str());
}
