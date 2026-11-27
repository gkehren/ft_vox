#include <Engine/DeveloperUi.hpp>
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
	if (const auto &sample = server.status; sample && monitoring::fresh(sample->sampledMs, monitoring::monotonicMs()))
	{
		const auto &transport = sample->transport;
		const auto number = [](uint64_t value) { return static_cast<unsigned long long>(value); };
		ImGui::Text("Uptime: %.1f s", static_cast<double>(sample->uptimeMs) / 1000.0);
		ImGui::Text("Admitted sessions: %u / %u", static_cast<unsigned>(transport.count),
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

static void drawMultiplayerNetworkStats(const ui::NetworkStats &stats)
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

static void drawHostedPlayers(const hosting::Snapshot *server);

void DeveloperUi::drawMultiplayerPanel(const DeveloperUiFrame &frame)
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

	if (frame.remote.multiplayerStatus)
	{
		const auto &remote = frame.remote;
		ImGui::Text("%s", remote.multiplayerStatus->label);
		if (remote.multiplayerStatus->detail)
			ImGui::TextWrapped("%s", remote.multiplayerStatus->detail);
		ImGui::TextWrapped("Server: %.*s", static_cast<int>(remote.multiplayerEndpoint.size()),
			remote.multiplayerEndpoint.data());
		ImGui::TextWrapped("Profile: %.*s", static_cast<int>(remote.multiplayerProfile.size()),
			remote.multiplayerProfile.data());
		if (ImGui::CollapsingHeader("Network", ImGuiTreeNodeFlags_DefaultOpen))
			drawMultiplayerNetworkStats(remote.networkStats);
	}
	if (frame.hostedServer && frame.hostedServer->active())
		drawHostedStatus(*frame.hostedServer);
	drawHostedPlayers(frame.hostedServer);
	ImGui::TextWrapped("Manage connections and hosted servers from the Game Menu.");
	ImGui::End();
}

static void drawHostedPlayers(const hosting::Snapshot *server)
{
	const auto *sample = server && server->state == hosting::State::Ready && server->status &&
		monitoring::fresh(server->status->sampledMs, monitoring::monotonicMs()) ? server->status.get() : nullptr;
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
	ImGui::Text("Admitted sessions: %u / %u", static_cast<unsigned>(players.size()),
		static_cast<unsigned>(sample->transport.capacity));
	ImGui::Text("Offline registered profiles: %llu", static_cast<unsigned long long>(sample->offlineProfiles));
	if (players.empty())
	{
		ImGui::TextUnformatted("No admitted sessions.");
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
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("Player #%u", static_cast<unsigned>(row.id.value));
				ImGui::TableSetColumnIndex(1);
				ImGui::TextUnformatted(sessionLabel(row.state));
			}
		ImGui::EndTable();
	}
}
