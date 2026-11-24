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

bool endpointChoices(const char *label, const std::vector<net::udp::Address> &endpoints,
	client::JoinDraft &draft)
{
	if (endpoints.empty())
		return false;
	bool changed = false;
	ImGui::SetNextItemWidth(-1.f);
	ImGui::PushID(label);
	if (ImGui::BeginCombo("##endpoints", label, ImGuiComboFlags_HeightSmall))
	{
		for (auto address : endpoints)
		{
			const auto endpoint = client::formatEndpoint(address);
			if (ImGui::Selectable(endpoint.c_str(), endpoint == draft.endpoint.data()))
				changed = draft.selectSavedEndpoint(address);
		}
		ImGui::EndCombo();
	}
	ImGui::PopID();
	return changed;
}

const std::string *profileChoice(const char *id, std::span<const std::string> profiles, std::string_view current)
{
	if (profiles.empty())
		return nullptr;
	const std::string *choice = nullptr;
	ImGui::SetNextItemWidth(-1.f);
	if (ImGui::BeginCombo(id, "Saved profiles", ImGuiComboFlags_HeightSmall))
	{
		for (const auto &profile : profiles)
			if (ImGui::Selectable(profile.c_str(), profile == current))
				choice = &profile;
		ImGui::EndCombo();
	}
	return choice;
}

const char *sessionLabel(monitoring::SessionState phase)
{
	switch (phase)
	{
	case monitoring::SessionState::Syncing: return "Syncing";
	case monitoring::SessionState::Connected: return "Connected";
	case monitoring::SessionState::Disconnecting: return "Disconnecting";
	}
	return "Unavailable";
}

void drawHostedStatus(const hosting::Snapshot &server)
{
	if (!ImGui::TreeNode("Server status"))
		return;
	if (const auto &sample = server.status)
	{
		const auto &transport = sample->transport;
		const auto number = [](uint64_t value) { return static_cast<unsigned long long>(value); };
		ImGui::Text("Uptime: %.1f s", static_cast<double>(sample->uptimeMs) / 1000.0);
		ImGui::Text("Online sessions: %u / %u", static_cast<unsigned>(transport.count),
			static_cast<unsigned>(transport.capacity));
		ImGui::Text("Offline registered profiles: %llu", number(sample->offlineProfiles));
		ImGui::Text("Completed ticks: %llu   Dropped ticks: %llu", number(sample->ticks), number(sample->droppedTicks));
		ImGui::Text("Resident chunks: %llu", number(sample->chunks));
		ImGui::TextWrapped("Streaming: %llu awaiting records, %llu awaiting generation, %llu generating",
			number(sample->waitingForRecord), number(sample->waitingForGeneration), number(sample->generating));
		ImGui::TextWrapped("World sends: %u pending/in flight, %u baselines",
			static_cast<unsigned>(transport.worldSends), static_cast<unsigned>(transport.baselines));
		ImGui::TextWrapped("Server UDP total: %.1f KiB received, %.1f KiB sent",
			static_cast<double>(transport.receivedBytes) / 1024.0, static_cast<double>(transport.sentBytes) / 1024.0);
		ImGui::TextWrapped("World traffic total: %.1f KiB sent, %llu retransmits, %llu resyncs",
			static_cast<double>(transport.worldBytes) / 1024.0, number(transport.retransmits), number(transport.resyncs));
	}
	else
		ImGui::TextWrapped("Waiting for a current server sample.");
	ImGui::TreePop();
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
	if (m_focusHostPanel)
		ImGui::SetNextWindowFocus();
	if (!ImGui::Begin(ui::windows::kMultiplayer, &m_debug.panels.multiplayer))
	{
		ImGui::End();
		return;
	}

	if (ImGui::CollapsingHeader("Join", ImGuiTreeNodeFlags_DefaultOpen))
		drawJoinPanel(frame);
	if (m_focusHostPanel)
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);
	if (ImGui::CollapsingHeader("Host") && frame.hostedServer)
	{
		if (m_focusHostPanel)
			ImGui::SetScrollHereY(0.f);
		m_focusHostPanel = false;
		drawHostPanel(frame);
	}
	if (ImGui::CollapsingHeader("Network", ImGuiTreeNodeFlags_DefaultOpen))
		ui::drawMultiplayerNetworkStats(frame.networkStats);
	drawHostedPlayers(frame.hostedServer);
	if (ImGui::CollapsingHeader("Administration"))
		ImGui::TextWrapped("Server controls are unavailable.");
	ImGui::End();
}

void GameUI::drawHostedPlayers(const hosting::Snapshot *server)
{
	const auto *sample = server && server->state == hosting::State::Ready ? server->status.get() : nullptr;
	if (!sample || m_hostSelectionLaunch != server->launch)
		m_hostPlayerSelection = {};
	m_hostSelectionLaunch = server ? server->launch : 0;
	if (sample && m_hostPlayerSelection.valid())
	{
		const auto players = sample->transport.roster();
		const auto found = std::ranges::lower_bound(players, m_hostPlayerSelection.value, {}, [](const auto &player) {
			return player.id.value;
		});
		if (found == players.end() || found->id != m_hostPlayerSelection)
			m_hostPlayerSelection = {};
	}
	if (!ImGui::CollapsingHeader("Players"))
		return;
	if (!sample)
	{
		ImGui::TextWrapped("%s", server && server->state == hosting::State::Ready ?
			"Waiting for a current server roster." : "The full roster is available when hosting a local server.");
		return;
	}
	const auto players = sample->transport.roster();
	ImGui::TextWrapped("Hosted world: %s", server->world.c_str());
	ImGui::Text("Online sessions: %u / %u", static_cast<unsigned>(players.size()),
		static_cast<unsigned>(sample->transport.capacity));
	ImGui::Text("Offline registered profiles: %llu", static_cast<unsigned long long>(sample->offlineProfiles));
	if (players.empty())
	{
		ImGui::TextUnformatted("No online players.");
		return;
	}
	ImGui::TextWrapped("Server-side player RTT is unavailable. The Network section shows your client RTT.");
	if (ImGui::BeginTable("host-roster", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
		ImVec2(0.f, ImGui::GetTextLineHeightWithSpacing() * 10.f)))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Player");
		ImGui::TableSetupColumn("Session");
		ImGui::TableHeadersRow();
		ImGuiListClipper clipper;
		clipper.Begin(static_cast<int>(players.size()));
		while (clipper.Step())
			for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
			{
				const auto &row = players[static_cast<size_t>(index)];
				char label[32];
				std::snprintf(label, sizeof(label), "Player #%u", static_cast<unsigned>(row.id.value));
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				if (ImGui::Selectable(label, m_hostPlayerSelection == row.id, ImGuiSelectableFlags_SpanAllColumns))
					m_hostPlayerSelection = row.id;
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(sessionLabel(row.state));
			}
		ImGui::EndTable();
	}
}

void GameUI::showHostPanel()
{
	leaveLayoutTilesHidden();
	m_debug.panels.multiplayer = true;
	m_focusHostPanel = true;
}

void GameUI::drawHostProfile(const GameUIFrame &frame)
{
	if (!m_hostProfileInitialized)
	{
		std::snprintf(m_hostProfile.data(), m_hostProfile.size(), "%s", frame.joinPreferences->selectedProfile.c_str());
		m_hostProfileInitialized = true;
	}
	ImGui::TextUnformatted("Local profile for hosted server");
	if (const auto *profile = profileChoice("##host-profiles", *frame.joinProfiles, m_hostProfile.data()))
		std::snprintf(m_hostProfile.data(), m_hostProfile.size(), "%s", profile->c_str());
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputTextWithHint("##host-profile", "Existing profile name", m_hostProfile.data(), m_hostProfile.size());
	ImGui::SetItemTooltip("Starting a new world binds its primary owner to this profile.");
}

void GameUI::drawHostPanel(const GameUIFrame &frame)
{
	using hosting::State;
	const auto &server = *frame.hostedServer;
	if (!server.message.empty())
		ImGui::TextWrapped("%s", server.message.c_str());
	if (!frame.hostError.empty())
		ImGui::TextWrapped("%s", frame.hostError.c_str());
	if (frame.exitAfterHostedStop)
		ImGui::TextWrapped("The game will exit after the server confirms its checkpoint.");

	if (server.active())
		ImGui::TextWrapped("World: %s", server.world.c_str());
	if (server.state == State::Ready)
	{
		ImGui::TextWrapped("Address: %s:%u", server.address.c_str(), static_cast<unsigned>(server.port));
		ImGui::BeginDisabled(frame.switchingSession);
		drawHostProfile(frame);
		if (ImGui::Button("Join hosted server"))
			frame.joinHostedServer(m_hostProfile.data());
		ImGui::EndDisabled();
	}
	if (server.state == State::Starting || server.state == State::Ready)
	{
		if (ImGui::Button("Stop server"))
			frame.stopHostedServer();
	}
	if (server.state == State::Stopping && server.delayed && ImGui::Button("Force stop…"))
		ImGui::OpenPopup("Force stop hosted server?");
	if (ImGui::BeginPopupModal("Force stop hosted server?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
		ImGui::TextUnformatted("Force stopping can lose changes made since the last committed checkpoint.");
		ImGui::PopTextWrapPos();
		if (ImGui::Button("Keep waiting"))
			ImGui::CloseCurrentPopup();
		ImGui::SameLine();
		ImGui::BeginDisabled(server.state != State::Stopping || !server.delayed);
		if (ImGui::Button("Force stop"))
		{
			frame.forceStopHostedServer();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndDisabled();
		ImGui::EndPopup();
	}

	if (!server.active())
	{
		if (!frame.hostWorlds.empty())
		{
			ImGui::SetNextItemWidth(-1.f);
			if (ImGui::BeginCombo("##host-worlds", "Saved server worlds", ImGuiComboFlags_HeightSmall))
			{
				for (const auto &world : frame.hostWorlds)
					if (ImGui::Selectable(world.c_str(), world == m_hostWorld.data()))
						std::snprintf(m_hostWorld.data(), m_hostWorld.size(), "%s", world.c_str());
				ImGui::EndCombo();
			}
		}
		ImGui::TextUnformatted("Server world");
		ImGui::SetNextItemWidth(-1.f);
		ImGui::InputTextWithHint("##host-world", "New or existing world name", m_hostWorld.data(), m_hostWorld.size());
		ImGui::TextUnformatted("Port (0 assigns a free port)");
		ImGui::SetNextItemWidth(-1.f);
		ImGui::InputInt("##host-port", &m_hostOptions.port);
		ImGui::Checkbox("Allow LAN / VPN connections", &m_hostOptions.lan);
		if (m_hostOptions.lan)
		{
			ImGui::TextWrapped("Players who can reach this address can join. Use a trusted private LAN or VPN.");
			ImGui::TextWrapped("Use an IPv4 address assigned to this machine. The server checks it when binding.");
			ImGui::SetNextItemWidth(-1.f);
			ImGui::InputTextWithHint("##host-address", "Private IPv4 address on this machine",
				m_hostAddress.data(), m_hostAddress.size());
		}
		else
			ImGui::TextUnformatted("Loopback only (127.0.0.1)");

		ImGui::TextUnformatted("Maximum players");
		ImGui::SetNextItemWidth(-1.f);
		ImGui::SliderInt("##host-players", &m_hostOptions.playerSlots, 1, hosting::kMaxPlayerSlots, "%d",
			ImGuiSliderFlags_AlwaysClamp);
		ImGui::TextUnformatted("Interest radius (chunks)");
		ImGui::SetNextItemWidth(-1.f);
		ImGui::SliderInt("##host-radius", &m_hostOptions.interestRadius, 0, hosting::kMaxInterestRadius, "%d",
			ImGuiSliderFlags_AlwaysClamp);
		static constexpr std::array workers{0, 2, 4, 8};
		static constexpr std::array workerLabels{"Synchronous (0)", "2 workers", "4 workers", "8 workers"};
		const auto workerIndex = std::ranges::find(workers, m_hostOptions.workers) - workers.begin();
		ImGui::TextUnformatted("Terrain workers");
		ImGui::SetNextItemWidth(-1.f);
		if (ImGui::BeginCombo("##host-workers", workerLabels[static_cast<size_t>(workerIndex)]))
		{
			for (size_t i = 0; i < workers.size(); ++i)
				if (ImGui::Selectable(workerLabels[i], m_hostOptions.workers == workers[i]))
					m_hostOptions.workers = workers[i];
			ImGui::EndCombo();
		}
		int streaming = static_cast<int>(m_hostOptions.streaming);
		ImGui::TextUnformatted("Streaming budget");
		ImGui::SetNextItemWidth(-1.f);
		if (ImGui::Combo("##host-streaming", &streaming, "Low\0Balanced\0High\0"))
			m_hostOptions.streaming = static_cast<hosting::StreamingPreset>(streaming);
		drawHostProfile(frame);
		ImGui::Checkbox("Join automatically when ready", &m_hostAutoJoin);
		if (ImGui::Button("Start server"))
		{
			m_hostOptions.world = m_hostWorld.data();
			m_hostOptions.address = m_hostAddress.data();
			frame.startHostedServer(m_hostOptions, m_hostAutoJoin, m_hostProfile.data());
		}
	}
	if (server.checkpoint > 0)
		ImGui::Text("Checkpoint generation: %llu", static_cast<unsigned long long>(server.checkpoint));
	if (server.state == State::Ready)
		drawHostedStatus(server);
	if (!server.logs.empty() && ImGui::TreeNode("Server log"))
	{
		ImGui::BeginChild("##host-log", ImVec2(0.f, ImGui::GetTextLineHeightWithSpacing() * 9.f));
		for (const auto &line : server.logs)
			ImGui::TextWrapped("%s", line.c_str());
		ImGui::EndChild();
		ImGui::TreePop();
	}
}

void GameUI::drawJoinPanel(const GameUIFrame &frame)
{
	const auto &prefs = *frame.joinPreferences;
	if (!m_joinDraftInitialized)
	{
		if (!frame.multiplayerEndpoint.empty())
			m_joinDraft.setEndpoint(frame.multiplayerEndpoint);
		else if (!prefs.recent.empty())
			m_joinDraft.setEndpoint(client::formatEndpoint(prefs.recent.front()));
		else
			m_joinDraft.setEndpoint("127.0.0.1:7777");
		m_joinDraft.setProfile(prefs.selectedProfile);
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
	endpointChoices("Favorite servers", prefs.favorites, m_joinDraft);
	endpointChoices("Recent servers", prefs.recent, m_joinDraft);
	ImGui::TextUnformatted("Server address and port");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputTextWithHint("##endpoint", "127.0.0.1:7777", m_joinDraft.endpoint.data(), m_joinDraft.endpoint.size());
	if (ImGui::Button("Toggle favorite"))
		frame.toggleFavorite(m_joinDraft.endpoint.data());
	ImGui::SetItemTooltip("Add or remove this address in Favorite servers.");
	ImGui::TextUnformatted("Expected world seed (optional)");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("##seed", m_joinDraft.expectedSeed.data(), m_joinDraft.expectedSeed.size());

	if (const auto *profile = profileChoice("##profiles", *frame.joinProfiles, m_joinDraft.profile.data()))
	{
		m_joinDraft.setProfile(*profile);
		frame.selectJoinProfile(*profile);
	}
	ImGui::TextUnformatted("Local profile name");
	ImGui::SetNextItemWidth(-1.f);
	ImGui::InputText("##profile", m_joinDraft.profile.data(), m_joinDraft.profile.size());
	if (ImGui::Button("Create profile"))
		frame.createJoinProfile(m_joinDraft.profile.data());
	ImGui::SetItemTooltip("Create an identity for a new name. Reuse that name to resume the same player.");
	if (ImGui::Button(frame.leaveServer ? "Switch server or profile" : "Connect"))
		frame.connectServer(m_joinDraft.request());
	ImGui::EndDisabled();
	if (!frame.joinError.empty())
		ImGui::TextWrapped("%s", frame.joinError.c_str());
	if (!frame.joinPreferencesError.empty())
		ImGui::TextWrapped("%s", frame.joinPreferencesError.c_str());
}
