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

// --- Typed request feedback (corrections F03/F08) --------------------------

std::string_view feedbackMessage(MultiplayerOutcome outcome)
{
	switch (outcome)
	{
	case MultiplayerOutcome::StaleSession:
		return "The session changed before the action ran. Review and try again.";
	case MultiplayerOutcome::ServerChanged:
		return "The hosted server changed. Review the action against the current server.";
	case MultiplayerOutcome::Busy:
		return "Another session action is still finishing. Try again in a moment.";
	case MultiplayerOutcome::InvalidState:
		return "The server is not in a state where that action applies right now.";
	case MultiplayerOutcome::PermissionChanged:
		return "Permissions, player state or the server changed. Refresh and review the action again.";
	case MultiplayerOutcome::ActionUnavailable:
		return "That action is not available right now.";
	case MultiplayerOutcome::Submitted: case MultiplayerOutcome::Cancelled: break;
	}
	return {};
}

enum class FeedbackGroup { Join, Host, Admin, Profiles };

/// Refusal message for the notice belonging to this screen's actions, empty
/// when the notice targets another screen or was not a refusal (F03/F08).
std::string_view noticeText(const MultiplayerView &view, FeedbackGroup group)
{
	if (!view.notice || view.notice->outcome == MultiplayerOutcome::Cancelled ||
		view.notice->outcome == MultiplayerOutcome::Submitted)
		return {};
	const auto action = view.notice->action;
	const bool match = group == FeedbackGroup::Join
		? (action == MultiplayerAction::Connect || action == MultiplayerAction::Reconnect ||
			action == MultiplayerAction::Disconnect || action == MultiplayerAction::ReturnToSolo ||
			action == MultiplayerAction::Favorite)
		: group == FeedbackGroup::Host
			? (action == MultiplayerAction::StartHost || action == MultiplayerAction::JoinHost ||
				action == MultiplayerAction::StopHost || action == MultiplayerAction::ForceStop)
			: group == FeedbackGroup::Admin
				? action == MultiplayerAction::Administer
				: (action == MultiplayerAction::SelectProfile || action == MultiplayerAction::CreateProfile);
	return match ? feedbackMessage(view.notice->outcome) : std::string_view{};
}
}

void GameUiSample::setMultiplayerView(MultiplayerView view)
{
	if (view.sessionEpoch != m_multiplayer.sessionEpoch || view.launch != m_multiplayer.launch)
	{
		cancelPointerPress();
		m_mpError.clear();
		m_mpErrorOnHost = false;
	}
	// The administrative selection belongs to its server launch, its verified
	// world identity and a live authorization (correction F12): the same
	// numeric PlayerId is a different player in another world or launch, and
	// losing the channel never restores a target. The cached reply is
	// invalidated with it, and an armed pointer press from the previous
	// context is cancelled.
	const bool adminAuthorized = view.administration &&
		view.administration->role != localcontrol::Role::Player;
	const uint64_t adminWorld = adminAuthorized ? view.administration->world : 0;
	if (!adminSelectionCompatible(view.launch, adminWorld, adminAuthorized,
			m_mpAdminContextLaunch, m_mpAdminContextWorld, m_mpAdminContextAuthorized))
	{
		if (m_mpAdminTarget.valid() || m_mpAdminDisplayed)
			cancelPointerPress();
		m_mpAdminTarget = {};
		m_mpAdminDisplayed.reset();
		m_mpAdminContextLaunch = view.launch;
		m_mpAdminContextWorld = adminWorld;
		m_mpAdminContextAuthorized = adminAuthorized;
	}
	// Draft lifecycle (corrections F06/F10): the automatic endpoint is adopted
	// ONLY when its value changed and the player has not edited the draft. An
	// unchanged view never reconstructs the field, so the caret, selection and
	// IME composition survive every frame; an edited draft (its value no longer
	// matches the last automatic one) survives navigation and session changes.
	// An active composition is never interrupted (F10).
	const std::string endpointAuto = !view.endpoint.empty() ? view.endpoint :
		(view.preferences.recent.empty() ? "127.0.0.1:7777" : client::formatEndpoint(view.preferences.recent.front()));
	if (endpointAuto != m_joinEndpointAuto)
	{
		if (m_joinEndpoint.value() == m_joinEndpointAuto && m_joinEndpoint.composition().empty())
			setText(m_joinEndpoint, endpointAuto);
		m_joinEndpointAuto = endpointAuto;
	}
	m_multiplayer = std::move(view);
	if (m_mpInitialized)
		return;
	m_mpInitialized = true;
	setText(m_joinEndpoint, endpointAuto);
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
	const auto detail = [&](std::string_view message, float height = 64.f) {
		if (!message.empty())
		{
			m_mpDetail.assign(message);
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
	// Operation-specific errors never outlive their screen family (correction
	// F06): the Start validation message is dropped once the player leaves the
	// host screens entirely.
	if (m_mpErrorOnHost && screen != Screen::HostServer && screen != Screen::HostOptions)
	{
		m_mpError.clear();
		m_mpErrorOnHost = false;
	}
	// Transient clipboard feedback belongs to the Join screen (correction
	// F15): leaving it clears the note so it cannot mask later feedback.
	if (screen != Screen::JoinServer && !m_mpClipboard.empty())
		m_mpClipboard.clear();
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
	{
		text("Join server", 32.f);
		connection();
		// Ordered feedback (corrections F03/F08/F15): a typed refusal, then
		// the transient clipboard result — displayed independently of a
		// historical connection error — then the join error, the transition
		// status and the preferences error.
		std::string message{noticeText(view, FeedbackGroup::Join)};
		if (message.empty() && !m_mpClipboard.empty())
			message = m_mpClipboard;
		if (message.empty() && !view.joinError.empty())
			message = view.joinError;
		if (message.empty() && view.pending)
			message = "Finishing the previous session...";
		if (message.empty() && view.connection && view.connection->detail)
			message = view.connection->detail;
		if (message.empty())
			message = view.preferencesError;
		detail(message, 50.f);
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
	}
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
		detail(!noticeText(view, FeedbackGroup::Profiles).empty() ? std::string(noticeText(view, FeedbackGroup::Profiles)) :
			(!view.joinError.empty() ? view.joinError : view.preferencesError));
		break;
	case Screen::HostServer:
		text("Host server", 32.f);
		detail(!noticeText(view, FeedbackGroup::Host).empty() ? std::string(noticeText(view, FeedbackGroup::Host)) :
			(!m_mpError.empty() ? m_mpError : (!view.hostError.empty() ? view.hostError : view.hostMessage)), 56.f);
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
		// The roster rows rebuild only when the supervisor published a new
		// monitor sample or the verified roles changed (correction F07); the
		// headers and nearby players stay live.
		if (view.monitor != m_mpMonitorDisplayed || view.administration != m_mpRosterAdminDisplayed)
		{
			m_mpMonitorDisplayed = view.monitor;
			m_mpRosterAdminDisplayed = view.administration;
			m_mpRosterRows.clear();
			if (view.monitor)
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
					m_mpRosterRows.push_back(std::move(label));
				}
			refs(m_mpRosterLabels, m_mpRosterRows);
		}
		if (view.monitor)
		{
			text("Hosted sessions  |  " + std::to_string(view.monitor->transport.count) + " / " +
				std::to_string(view.monitor->transport.capacity));
			text("Offline registered profiles  |  " + std::to_string(view.monitor->offlineProfiles));
			if (m_mpRosterRows.empty())
				text("No admitted sessions");
			else
				list(AdminPlayers, "Live hosted roster", m_mpList, m_mpRosterLabels, 4);
		}
		else if (view.hostState == hosting::State::Ready)
			text("Waiting for a fresh hosted roster");
		else
			text("Full roster and roles are available on the local host.");
		m_mpSecondRows.clear();
		for (const auto id : view.nearby)
			m_mpSecondRows.push_back(playerLabel(id) + (id == view.self ? "  |  You" : ""));
		refs(m_mpSecondLabels, m_mpSecondRows);
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
			// A refusal produced while the channel was still verified stays
			// visible after the authorization disappeared (correction F15):
			// the player learns WHY the action failed, not just that the
			// channel is gone.
			const auto refusal = noticeText(view, FeedbackGroup::Admin);
			detail(refusal.empty() ? std::string_view(view.adminChannelFailed ?
				"Local administration channel unavailable." :
				"Waiting for verified local permissions. Remote roles do not grant local control.") :
				refusal);
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
		if (m_mpAdminDisplayed != view.administration)
		{
			// A roster refresh never retargets an action (correction F01):
			// the rows rebuild, an armed press against the old rows is
			// cancelled, and the explicit target identity is re-resolved below.
			cancelPointerPress();
			m_mpAdminDisplayed = view.administration;
			m_mpAdminRows.clear();
			for (const auto &row : admin->players)
				m_mpAdminRows.push_back(playerLabel(row.id) + "  |  " + std::string(localcontrol::label(row.role)) +
					(row.banned ? "  |  Banned" : "") + (row.localAdmin ? "  |  Local control" : ""));
			refs(m_mpAdminLabels, m_mpAdminRows);
		}
		// The target is a stable identity, never a row index (corrections
		// F01/F11): reorderings keep it and a disappearing target clears the
		// selection. Clicks and keyboard steps already drove
		// ListSelection::select themselves (with reveal); the build only
		// RECONCILES the row index of the identity and clamps the scroll
		// offset when the roster size changed — the scroll window is never
		// reset while the view is unchanged.
		const auto target = std::ranges::find(admin->players, m_mpAdminTarget, &localcontrol::Player::id);
		const int targetRow = target == admin->players.end() ? -1
			: static_cast<int>(target - admin->players.begin());
		if (targetRow < 0)
		{
			if (m_mpAdminTarget.valid())
				cancelPointerPress();
			m_mpAdminTarget = {};
		}
		m_mpList.selection.selected = targetRow;
		m_mpList.selection.first = std::clamp(m_mpList.selection.first, 0,
			std::max(0, static_cast<int>(m_mpAdminLabels.size()) - 4));
		// The admin list owns its selection state: the shared list() helper
		// clamps to a valid row, which would implicitly select a player.
		m_mpList.visibleRows = 4;
		m_mpList.items = m_mpAdminLabels;
		m_widgets.add({AdminPlayers, slot(22.f + 23.f * 4.f), "Registered players", {}, &m_mpList,
			!m_mpAdminLabels.empty()});
		if (target != admin->players.end())
		{
			const auto &row = *target;
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
		else if (!m_mpAdminLabels.empty())
			text("Select a player to moderate.");
		if (admin->role == localcontrol::Role::Owner)
		{
			button(Allowlist, admin->allowlist ? "Disable allowlist..." : "Enable allowlist...", canAct);
			button(AdminStop, "Stop and save server...", canAct);
		}
		detail(!noticeText(view, FeedbackGroup::Admin).empty() ? std::string(noticeText(view, FeedbackGroup::Admin)) :
			(!m_mpError.empty() ? m_mpError : view.hostError));
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
				if (action.operation == Operation::Kick)
					// A kick targets the persistent identity (correction F02):
					// the server stores a 60-second kickedUntil lockout on the
					// persistent access record, so the wording describes the
					// reconnection refusal, not just the disconnect.
					text("Disconnects the player and refuses reconnection for 60 seconds.");
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
	if (m_screens.top() == Screen::HostOptions)
	{
		if (id == Workers)
			m_mpWorkerChoice = selection(m_mpSecondList);
		if (id == Streaming)
			m_mpStreamingChoice = selection(m_mpThirdList);
		return;
	}
	// The administrative target follows the row selection as an identity
	// (correction F01). A press armed against the previous target belongs to
	// it: it is cancelled so a late release can never act on the new target.
	if (id == AdminPlayers && m_screens.top() == Screen::MultiplayerAdmin && m_mpAdminDisplayed)
	{
		const auto index = static_cast<size_t>(selection(m_mpList));
		const auto next = index < m_mpAdminDisplayed->players.size()
			? m_mpAdminDisplayed->players[index].id : sim::PlayerId{};
		if (next != m_mpAdminTarget)
		{
			m_mpAdminTarget = next;
			cancelPointerPress();
		}
	}
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
	case CopyEndpoint:
		// The clipboard result is real feedback (correction F08): a failed
		// copy must not look like a silent no-op.
		m_mpClipboard = SDL_SetClipboardText(view.endpoint.c_str())
			? "Endpoint copied to the clipboard." : "Could not copy the endpoint.";
		return true;
	case UseFavorite: m_mpClipboard.clear(); setText(m_joinEndpoint, selected(m_mpList)); return true;
	case UseRecent: m_mpClipboard.clear(); setText(m_joinEndpoint, selected(m_mpSecondList)); return true;
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
		m_mpClipboard.clear();
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
		m_mpErrorOnHost = false;
		if (!readInt(m_hostPort, options.port) || !readInt(m_hostSlots, options.playerSlots) ||
			!readInt(m_hostRadius, options.interestRadius))
		{
			m_mpError = "Use whole numbers for port, player slots and interest radius.";
			m_mpErrorOnHost = true;
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
	// Targeted actions address the EXPLICIT identity selection (correction
	// F01): with no target chosen nothing is addressed and no request is
	// built — the buttons are disabled; this is the defensive guard.
	const auto target = std::ranges::find(displayed.players, m_mpAdminTarget, &localcontrol::Player::id);
	if (id == Refresh)
		action.operation = Op::List;
	else if (id == Allowlist)
		action.operation = action.allowlist ? Op::DisableAllowlist : Op::EnableAllowlist;
	else if (id == AdminStop)
		action.operation = Op::Stop;
	else
	{
		if (target == displayed.players.end())
			return true;
		action.subject = *target;
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
