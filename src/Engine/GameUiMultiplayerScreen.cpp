#include <Engine/GameUiSample.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <utility>

using namespace gameui;

namespace
{
enum MpControl : ControlId
{
	Back = 100, JoinPage, HostPage, OptionsPage, ProfilesPage, RosterPage, DiagnosticsPage,
	AdminPage, AuditPage, Play, Connect, Reconnect, Disconnect, Solo, Favorite,
	Endpoint, Seed, Recent, Favorites, UseRecent, UseFavorite, CopyEndpoint,
	Profile, ProfileName, SelectProfile, CreateProfile,
	HostWorld, SavedWorld, UseWorld, HostPort, HostLan, HostBind, HostSlots, HostRadius,
	Workers, Streaming, AutoJoin, Start, JoinHost, StopHost, ForceHost,
	AdminPlayers, AdminProfile, Refresh, Kick, Ban, Allow, Role, Delegate, Allowlist, AdminStop, Confirm
};

void setText(TextField &field, std::string_view value)
{
	field = {};
	field.insert(value);
}

int selection(const ListBox &list) { return list.selection.selected; }

std::string_view selected(const ListBox &list)
{
	const auto index = static_cast<size_t>(selection(list));
	return index < list.items.size() ? list.items[index] : std::string_view{};
}

bool active(hosting::State state)
{
	return state == hosting::State::Starting || state == hosting::State::Ready || state == hosting::State::Stopping;
}

std::string playerLabel(sim::PlayerId id)
{
	return "Player #" + std::to_string(id.value);
}

const char *presence(monitoring::SessionState state)
{
	switch (state)
	{
	case monitoring::SessionState::Connected: return "Connected";
	case monitoring::SessionState::Syncing: return "Syncing";
	case monitoring::SessionState::Disconnecting: return "Leaving";
	}
	return "Unavailable";
}

std::string_view operationTitle(localcontrol::Operation operation)
{
	using enum localcontrol::Operation;
	switch (operation)
	{
	case Bootstrap: return "Set up owner";
	case Provision: return "Authorize local control";
	case GrantAdmin: return "Grant admin";
	case RevokeAdmin: return "Revoke admin";
	case Transfer: return "Transfer owner";
	case Recover: return "Recover owner";
	case Kick: return "Kick";
	case Ban: return "Ban";
	case Unban: return "Unban";
	case Allow: return "Add to allowlist";
	case Unallow: return "Remove from allowlist";
	case EnableAllowlist: return "Enable allowlist";
	case DisableAllowlist: return "Disable allowlist";
	case Stop: return "Stop and save";
	case List: return "Refresh players and audit";
	}
	return "Server action";
}

std::string pingText(const ui::NetworkStats &stats)
{
	if (!stats.ping)
		return "Ping unavailable";
	const auto ms = std::lround(stats.ping->roundTrip.count() * 1000.);
	const auto age = std::lround(stats.ping->sampleAge.count());
	return "Ping " + std::to_string(ms) + " ms  |  Sample " + std::to_string(age) + " s ago";
}

bool readInt(const TextField &field, int &value)
{
	const auto &text = field.value();
	const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
	return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

constexpr std::array<std::string_view, 4> workerLabels{"Synchronous", "2 workers", "4 workers", "8 workers"};
constexpr std::array<int, 4> workerCounts{0, 2, 4, 8};
constexpr std::array<std::string_view, 3> presets{"Low", "Balanced", "High"};
}

void GameUiSample::setMultiplayerView(MultiplayerView view)
{
	if (view.sessionEpoch != m_multiplayer.sessionEpoch || view.launch != m_multiplayer.launch)
	{
		cancelPointerPress();
		m_mpError.clear();
	}
	m_multiplayer = std::move(view);
	if (m_mpInitialized)
		return;
	m_mpInitialized = true;
	const auto &prefs = m_multiplayer.preferences;
	setText(m_joinEndpoint, !m_multiplayer.endpoint.empty() ? m_multiplayer.endpoint :
		(prefs.recent.empty() ? "127.0.0.1:7777" : client::formatEndpoint(prefs.recent.front())));
	setText(m_hostPort, "7777");
	setText(m_hostSlots, std::to_string(hosting::Options{}.playerSlots));
	setText(m_hostRadius, std::to_string(hosting::Options{}.interestRadius));
}

std::optional<MultiplayerRequest> GameUiSample::takeMultiplayerRequest()
{
	return std::exchange(m_multiplayerRequest, std::nullopt);
}

void GameUiSample::buildMultiplayerScreen(Screen screen, StackLayout &column)
{
	using localcontrol::Operation;
	const auto &view = m_multiplayer;
	const auto slot = [&](float units) { return column.next(units * m_screenScale); };
	const auto text = [&](std::string value, float units = 24.f) {
		m_mpText.push_back(std::move(value));
		m_widgets.add({0, slot(units), m_mpText.back(), {}, Label{}});
	};
	const auto button = [&](ControlId id, std::string_view label, bool enabled = true,
		style::ButtonVariant variant = style::ButtonVariant::Secondary) {
		m_widgets.add({id, slot(30.f), label, {}, Button{}, enabled, variant});
	};
	const auto field = [&](ControlId id, std::string_view label, TextField &editor) {
		m_widgets.add({id, slot(38.f), label, {}, &editor, !view.pending});
	};
	const auto toggle = [&](ControlId id, std::string_view label, Toggle &value) {
		m_widgets.add({id, slot(28.f), label, {}, &value, !view.pending});
	};
	const auto list = [&](ControlId id, std::string_view label, ListBox &box,
		std::span<const std::string_view> labels, int rows) {
		box.visibleRows = rows;
		box.items = labels;
		box.selection.select(box.selection.selected, static_cast<int>(labels.size()), rows);
		m_widgets.add({id, slot(22.f + 23.f * static_cast<float>(rows)), label, {}, &box, !labels.empty()});
	};
	const auto refs = [](std::vector<std::string_view> &output, const auto &input) {
		output.clear();
		for (const auto &value : input)
			output.emplace_back(value);
	};
	const auto endpoints = [&](std::vector<std::string> &rows, const auto &addresses) {
		rows.clear();
		for (const auto address : addresses)
			rows.push_back(client::formatEndpoint(address));
	};
	const auto detail = [&](std::string message, float height = 64.f) {
		if (!message.empty())
		{
			m_mpDetail = std::move(message);
			m_mpMessage = slot(height);
		}
	};
	const auto profile = [&] {
		text(view.preferences.selectedProfile.empty() ? "Choose or create a profile" :
			"Local profile  |  " + view.preferences.selectedProfile);
		button(ProfilesPage, "Profiles");
	};
	const auto connection = [&] {
		text(view.connection ? view.connection->label : "Disconnected", 30.f);
		if (!view.endpoint.empty())
			text(view.endpoint + "  |  " + view.sessionProfile);
		text(pingText(view.network));
	};
	const auto *admin = view.administration.get();
	const bool authorized = admin && admin->role != localcontrol::Role::Player;
	const bool available = !view.pending;
	const bool canJoin = available && !view.preferences.selectedProfile.empty() && !joinTransitionInProgress(view.connection);
	m_mpMessage = {};
	m_mpDetail.clear();
	m_mpText.clear();
	if (m_mpListRevision != m_screens.revision())
	{
		m_mpList.selection = {};
		m_mpSecondList.selection = {};
		m_mpThirdList.selection = {};
		m_mpListRevision = m_screens.revision();
	}
	switch (screen)
	{
	case Screen::Multiplayer:
		text("Multiplayer", 32.f);
		connection();
		detail(view.pending ? "Finishing the previous session..." :
			(view.connection && view.connection->detail ? view.connection->detail : ""));
		button(Play, "Play", view.playable && available, style::ButtonVariant::Primary);
		button(JoinPage, "Join server");
		button(HostPage, "Host server");
		button(RosterPage, "Players");
		button(DiagnosticsPage, "Connection details");
		break;
	case Screen::JoinServer:
		text("Join server", 32.f);
		connection();
		detail(!view.joinError.empty() ? view.joinError :
			(view.pending ? "Finishing the previous session..." :
				(view.connection && view.connection->detail ? view.connection->detail : view.preferencesError)), 50.f);
		button(Play, "Play", view.playable && available);
		if (view.disconnect)
			button(Disconnect, view.playable ? "Disconnect" : "Cancel connection", available);
		if (view.reconnect)
			button(Reconnect, "Reconnect", available);
		if (view.returnToSolo)
			button(Solo, "Return to solo / menu", available);
		field(Endpoint, "Private IPv4 address and port", m_joinEndpoint);
		field(Seed, "Expected world seed (optional)", m_joinSeed);
		profile();
		button(Connect, view.disconnect ? "Switch server or profile" : "Connect",
			canJoin, style::ButtonVariant::Primary);
		button(Favorite, "Toggle favorite", available);
		button(CopyEndpoint, "Copy connected endpoint", !view.endpoint.empty());
		endpoints(m_mpRows, view.preferences.favorites);
		endpoints(m_mpSecondRows, view.preferences.recent);
		refs(m_mpLabels, m_mpRows);
		refs(m_mpSecondLabels, m_mpSecondRows);
		if (!m_mpRows.empty())
		{
			list(Favorites, "Favorites", m_mpList, m_mpLabels, 2);
			button(UseFavorite, "Use favorite");
		}
		if (!m_mpSecondRows.empty())
		{
			list(Recent, "Recent endpoints", m_mpSecondList, m_mpSecondLabels, 2);
			button(UseRecent, "Use recent endpoint");
		}
		break;
	case Screen::MultiplayerProfiles:
		text("Local profiles", 32.f);
		text(view.preferences.selectedProfile.empty() ? "No profile selected" :
			"Selected  |  " + view.preferences.selectedProfile);
		m_mpRows = view.profiles;
		refs(m_mpLabels, m_mpRows);
		list(Profile, "Saved profiles", m_mpList, m_mpLabels, 5);
		button(SelectProfile, "Use selected profile", available && !view.profiles.empty());
		field(ProfileName, "New profile name", m_profileName);
		button(CreateProfile, "Create profile", available && !m_profileName.value().empty());
		detail(!view.joinError.empty() ? view.joinError : view.preferencesError);
		break;
	case Screen::HostServer:
		text("Host server", 32.f);
		detail(!m_mpError.empty() ? m_mpError : (!view.hostError.empty() ? view.hostError : view.hostMessage), 56.f);
		if (active(view.hostState))
		{
			text(view.hostWorld);
			if (!view.hostEndpoint.empty())
				text(view.hostEndpoint);
		}
		if (view.checkpoint)
			text("Checkpoint " + std::to_string(view.checkpoint));
		if (view.hostState == hosting::State::Ready)
		{
			if (view.connection)
				connection();
			profile();
			button(JoinHost, "Join hosted server", canJoin);
			button(Play, "Play", view.playable && available);
			button(RosterPage, "Players");
			button(AdminPage, "Administration");
		}
		if (view.hostState == hosting::State::Starting || view.hostState == hosting::State::Ready)
			button(StopHost, "Stop and save server...", available);
		if (view.hostState == hosting::State::Stopping && view.delayed)
			button(ForceHost, "Force stop...", available, style::ButtonVariant::Danger);
		if (!active(view.hostState))
		{
			field(HostWorld, "New or existing server world", m_hostWorld);
			m_mpRows = view.worlds;
			refs(m_mpLabels, m_mpRows);
			if (!view.worlds.empty())
			{
				list(SavedWorld, "Saved hosted worlds", m_mpList, m_mpLabels, 3);
				button(UseWorld, "Use selected world");
			}
			field(HostPort, "Port (0 assigns a free port)", m_hostPort);
			profile();
			toggle(AutoJoin, "Join automatically when ready", m_autoJoin);
			button(OptionsPage, "Private network and streaming options");
			button(Start, "Start server", available && !view.preferences.selectedProfile.empty(), style::ButtonVariant::Primary);
		}
		break;
	case Screen::HostOptions:
		text("Hosting options", 32.f);
		toggle(HostLan, "Allow trusted LAN / VPN connections", m_hostLan);
		m_mpSecondList.selection.selected = m_mpWorkerChoice;
		m_mpThirdList.selection.selected = m_mpStreamingChoice;
		if (m_hostLan.value)
		{
			detail("Anyone who can reach this address can connect. Use a trusted private LAN or VPN and an IPv4 address assigned to this machine.", 60.f);
			field(HostBind, "Private bind IPv4 address", m_hostBind);
		}
		else
			text("Loopback only (127.0.0.1)");
		field(HostSlots, "Maximum player slots (1-255)", m_hostSlots);
		field(HostRadius, "Interest radius (0-8 chunks)", m_hostRadius);
		list(Workers, "Terrain workers", m_mpSecondList, workerLabels, 4);
		list(Streaming, "Streaming budget", m_mpThirdList, presets, 3);
		break;
	case Screen::MultiplayerRoster:
		text("Players", 32.f);
		text(pingText(view.network));
		m_mpRows.clear();
		m_mpSecondRows.clear();
		if (view.monitor)
		{
			text("Hosted sessions  |  " + std::to_string(view.monitor->transport.count) + " / " +
				std::to_string(view.monitor->transport.capacity));
			text("Offline registered profiles  |  " + std::to_string(view.monitor->offlineProfiles));
			for (const auto &row : view.monitor->transport.roster())
			{
				auto label = playerLabel(row.id) + "  |  " + presence(row.state);
				if (admin)
				{
					const auto role = std::ranges::lower_bound(admin->players, row.id.value, {},
						[](const auto &p) { return p.id.value; });
					if (role != admin->players.end() && role->id == row.id)
						label += "  |  " + std::string(localcontrol::label(role->role));
				}
				m_mpRows.push_back(std::move(label));
			}
		}
		else if (view.hostState == hosting::State::Ready)
			text("Waiting for a fresh hosted roster");
		else
			text("Full roster and roles are available on the local host.");
		for (const auto id : view.nearby)
			m_mpSecondRows.push_back(playerLabel(id) + (id == view.self ? "  |  You" : ""));
		refs(m_mpLabels, m_mpRows);
		refs(m_mpSecondLabels, m_mpSecondRows);
		if (view.monitor)
		{
			if (m_mpRows.empty())
				text("No admitted sessions");
			else
				list(AdminPlayers, "Live hosted roster", m_mpList, m_mpLabels, 4);
		}
		if (m_mpSecondRows.empty())
			text("Nearby player snapshots unavailable");
		else
			list(Recent, "Nearby players in this client", m_mpSecondList, m_mpSecondLabels, 4);
		break;
	case Screen::MultiplayerDiagnostics:
		text("Connection details", 32.f);
		connection();
		if (view.network.snapshot)
		{
			const auto &sample = *view.network.snapshot;
			text("Player snapshot age  |  " + std::to_string(std::lround(sample.age.count() * 1000.)) + " ms");
			if (sample.missingFraction)
				text("Missing player snapshots  |  " + std::to_string(std::lround(*sample.missingFraction * 100.)) + "% estimated");
		}
		else
			text("Player snapshot unavailable");
		if (view.monitor)
			text("Hosted monitor age  |  " + std::to_string(monitoring::monotonicMs() -
				std::min(monitoring::monotonicMs(), view.monitor->sampledMs)) + " ms");
		else
			text("Fresh hosted monitor unavailable");
		detail(view.connection && view.connection->detail ? view.connection->detail : "");
		break;
	case Screen::MultiplayerAdmin:
	{
		text("Local administration", 32.f);
		if (!authorized)
		{
			detail(view.adminChannelFailed ? "Local administration channel unavailable." :
				"Waiting for verified local permissions. Remote roles do not grant local control.");
			break;
		}
		text("Local permission  |  " + std::string(localcontrol::label(admin->role)));
		text(admin->allowlist ? "Allowlist enabled" : "Allowlist disabled");
		if (view.adminPending)
			text("Waiting for " + std::string(operationTitle(view.adminOperation)));
		else if (view.adminResult)
			text(std::string(operationTitle(view.adminOperation)) + "  |  " + std::string(localcontrol::label(*view.adminResult)));
		const bool canAct = available && !view.adminPending;
		button(Refresh, "Refresh players and audit", canAct);
		button(AuditPage, "Recent audit history");
		const auto oldIndex = static_cast<size_t>(selection(m_mpList));
		const auto oldSubject = m_mpAdminDisplayed && oldIndex < m_mpAdminDisplayed->players.size() ?
			m_mpAdminDisplayed->players[oldIndex].id : sim::PlayerId{};
		if (m_mpAdminDisplayed != view.administration)
		{
			cancelPointerPress();
			m_mpAdminDisplayed = view.administration;
			m_mpAdminRows.clear();
			for (const auto &row : admin->players)
				m_mpAdminRows.push_back(playerLabel(row.id) + "  |  " + std::string(localcontrol::label(row.role)) +
					(row.banned ? "  |  Banned" : "") + (row.localAdmin ? "  |  Local control" : ""));
			refs(m_mpAdminLabels, m_mpAdminRows);
			const auto found = std::ranges::find(admin->players, oldSubject, &localcontrol::Player::id);
			m_mpList.selection = {};
			if (found != admin->players.end())
				m_mpList.selection.selected = static_cast<int>(found - admin->players.begin());
		}
		list(AdminPlayers, "Registered players", m_mpList, m_mpAdminLabels, 4);
		const auto index = static_cast<size_t>(selection(m_mpList));
		if (index < admin->players.size())
		{
			const auto &row = admin->players[index];
			const bool online = view.monitor && std::ranges::any_of(view.monitor->transport.roster(), [&](const auto &p) {
				return p.id == row.id && p.state == monitoring::SessionState::Connected;
			});
			button(Kick, "Kick for 60 seconds...", canAct && online && localcontrol::permitted(admin->role, Operation::Kick, row.role));
			button(Ban, row.banned ? "Unban..." : "Ban...", canAct && localcontrol::permitted(admin->role, localcontrol::Operation::Ban, row.role));
			button(Allow, row.allowed ? "Remove from allowlist..." : "Add to allowlist...",
				canAct && localcontrol::permitted(admin->role, localcontrol::Operation::Allow, row.role));
			if (localcontrol::permitted(admin->role, Operation::GrantAdmin, row.role))
			{
				button(Role, row.role == localcontrol::Role::Admin ? "Revoke admin..." : "Grant admin role...", canAct);
				m_mpSecondRows = view.profiles;
				refs(m_mpSecondLabels, m_mpSecondRows);
				list(AdminProfile, "Local control profile", m_mpSecondList, m_mpSecondLabels, 3);
				button(Delegate, "Authorize local control...", canAct && !view.profiles.empty());
			}
		}
		if (admin->role == localcontrol::Role::Owner)
		{
			button(Allowlist, admin->allowlist ? "Disable allowlist..." : "Enable allowlist...", canAct);
			button(AdminStop, "Stop and save server...", canAct);
		}
		detail(!m_mpError.empty() ? m_mpError : view.hostError);
		break;
	}
	case Screen::MultiplayerAudit:
		text("Recent audit history", 32.f);
		m_mpRows.clear();
		if (authorized)
			for (auto it = admin->audit.rbegin(); it != admin->audit.rend(); ++it)
				m_mpRows.push_back(std::to_string(it->revision) + "  |  " + std::string(operationTitle(it->operation)) +
					"  |  " + (it->actor.valid() ? playerLabel(it->actor) : "Local setup") +
					(it->target.valid() ? " -> " + playerLabel(it->target) : ""));
		refs(m_mpLabels, m_mpRows);
		list(Recent, "Committed actions", m_mpList, m_mpLabels, 10);
		break;
	case Screen::MultiplayerConfirm:
		text("Confirm server action", 32.f);
		text(view.hostWorld);
		if (m_multiplayerConfirm)
		{
			const auto &intent = *m_multiplayerConfirm;
			bool eligible = available && intent.launch == view.launch && intent.sessionEpoch == view.sessionEpoch;
			if (intent.administration)
			{
				const auto &action = *intent.administration;
				text(std::string(action.profile.empty() ? operationTitle(action.operation) : operationTitle(Operation::Provision)));
				if (action.subject)
					text(playerLabel(action.subject->id) + "  |  " + std::string(localcontrol::label(action.subject->role)));
				if (!action.profile.empty())
					text("Authorize profile " + action.profile);
				eligible &= admin && !view.adminPending && eligibleAdministration(action, view.launch, *admin, view.monitor.get());
			}
			else if (intent.action == MultiplayerAction::ForceStop)
			{
				text("Force stop");
				eligible &= view.hostState == hosting::State::Stopping && view.delayed;
			}
			else
			{
				text("Stop and save");
				eligible &= view.hostState == hosting::State::Starting || view.hostState == hosting::State::Ready;
			}
			detail(!eligible ? "The server, player or permissions changed. Go back and review the action." :
				(intent.action == MultiplayerAction::ForceStop ? "Recent changes may be lost." :
					"This action can affect active players."), 58.f);
			button(Back, "Cancel");
			button(Confirm, "Confirm", eligible, style::ButtonVariant::Danger);
			return;
		}
		break;
	default: break;
	}
	button(Back, "Back");
}

void GameUiSample::changeMultiplayerControl(ControlId id)
{
	if (m_screens.top() != Screen::HostOptions)
		return;
	if (id == Workers)
		m_mpWorkerChoice = selection(m_mpSecondList);
	if (id == Streaming)
		m_mpStreamingChoice = selection(m_mpThirdList);
}

bool GameUiSample::activateMultiplayerControl(ControlId id)
{
	using A = MultiplayerAction;
	using Op = localcontrol::Operation;
	if (!multiplayerScreen(m_screens.top()))
		return false;
	const auto &view = m_multiplayer;
	const auto base = [&](A action) {
		MultiplayerRequest request;
		request.action = action;
		request.launch = view.launch;
		request.sessionEpoch = view.sessionEpoch;
		return request;
	};
	const auto enqueue = [&](MultiplayerRequest request) {
		if (!m_multiplayerRequest)
			m_multiplayerRequest = std::move(request);
	};
	const auto chooseProfile = [&](A action, std::string value) {
		auto request = base(action);
		request.profile = std::move(value);
		enqueue(std::move(request));
	};
	const auto confirm = [&](MultiplayerRequest request) {
		m_multiplayerConfirm = std::move(request);
		openScreen(Screen::MultiplayerConfirm);
	};
	switch (id)
	{
	case Back: back(); return true;
	case JoinPage: openScreen(Screen::JoinServer); return true;
	case HostPage: openScreen(Screen::HostServer); return true;
	case OptionsPage: openScreen(Screen::HostOptions); return true;
	case ProfilesPage: openScreen(Screen::MultiplayerProfiles); return true;
	case RosterPage: openScreen(Screen::MultiplayerRoster); return true;
	case DiagnosticsPage: openScreen(Screen::MultiplayerDiagnostics); return true;
	case AdminPage: openScreen(Screen::MultiplayerAdmin); return true;
	case AuditPage: openScreen(Screen::MultiplayerAudit); return true;
	case Play: if (view.playable && !view.pending) resetScreens(); return true;
	case Reconnect: enqueue(base(A::Reconnect)); return true;
	case Disconnect: enqueue(base(A::Disconnect)); return true;
	case Solo: enqueue(base(A::ReturnToSolo)); return true;
	case CopyEndpoint: SDL_SetClipboardText(view.endpoint.c_str()); return true;
	case UseFavorite: setText(m_joinEndpoint, selected(m_mpList)); return true;
	case UseRecent: setText(m_joinEndpoint, selected(m_mpSecondList)); return true;
	case UseWorld: setText(m_hostWorld, selected(m_mpList)); return true;
	case SelectProfile: chooseProfile(A::SelectProfile, std::string(selected(m_mpList))); return true;
	case CreateProfile: chooseProfile(A::CreateProfile, m_profileName.value()); return true;
	case JoinHost: chooseProfile(A::JoinHost, view.preferences.selectedProfile); return true;
	case StopHost: confirm(base(A::StopHost)); return true;
	case ForceHost: confirm(base(A::ForceStop)); return true;
	case Confirm:
		if (m_multiplayerConfirm)
			enqueue(std::move(*m_multiplayerConfirm));
		back();
		return true;
	case Connect: case Favorite:
	{
		auto request = base(id == Connect ? A::Connect : A::Favorite);
		request.join = {m_joinEndpoint.value(), view.preferences.selectedProfile, m_joinSeed.value()};
		enqueue(std::move(request));
		return true;
	}
	case Start:
	{
		auto request = base(A::StartHost);
		auto &options = request.options;
		m_mpError.clear();
		if (!readInt(m_hostPort, options.port) || !readInt(m_hostSlots, options.playerSlots) ||
			!readInt(m_hostRadius, options.interestRadius))
		{
			m_mpError = "Use whole numbers for port, player slots and interest radius.";
			return true;
		}
		options.world = m_hostWorld.value();
		options.lan = m_hostLan.value;
		options.address = m_hostBind.value();
		options.workers = workerCounts[static_cast<size_t>(m_mpWorkerChoice)];
		options.streaming = static_cast<hosting::StreamingPreset>(m_mpStreamingChoice);
		request.autoJoin = m_autoJoin.value;
		request.profile = view.preferences.selectedProfile;
		enqueue(std::move(request));
		return true;
	}
	case Refresh: case Kick: case Ban: case Allow: case Role: case Delegate: case Allowlist: case AdminStop:
		break;
	default: return false;
	}

	if (!view.administration || !m_mpAdminDisplayed || view.pending || view.adminPending)
		return true;
	const auto &displayed = *m_mpAdminDisplayed;
	auto request = base(A::Administer);
	AdministrationIntent action;
	action.launch = view.launch;
	action.world = displayed.world;
	action.epoch = displayed.epoch;
	action.role = displayed.role;
	action.allowlist = displayed.allowlist;
	const auto index = static_cast<size_t>(selection(m_mpList));
	if (id == Refresh)
		action.operation = Op::List;
	else if (id == Allowlist)
		action.operation = action.allowlist ? Op::DisableAllowlist : Op::EnableAllowlist;
	else if (id == AdminStop)
		action.operation = Op::Stop;
	else
	{
		if (index >= displayed.players.size())
			return true;
		action.subject = displayed.players[index];
		switch (id)
		{
		case Kick: action.operation = Op::Kick; break;
		case Ban: action.operation = action.subject->banned ? Op::Unban : Op::Ban; break;
		case Allow: action.operation = action.subject->allowed ? Op::Unallow : Op::Allow; break;
		case Role: action.operation = action.subject->role == localcontrol::Role::Admin ? Op::RevokeAdmin : Op::GrantAdmin; break;
		case Delegate: action.operation = Op::GrantAdmin; action.profile = selected(m_mpSecondList); break;
		default: return true;
		}
	}
	if (!eligibleAdministration(action, view.launch, *view.administration, view.monitor.get()))
	{
		m_mpError = "The server or selected player changed. Refresh and review the action.";
		return true;
	}
	request.administration = std::move(action);
	if (id == Refresh)
		enqueue(std::move(request));
	else
		confirm(std::move(request));
	return true;
}
