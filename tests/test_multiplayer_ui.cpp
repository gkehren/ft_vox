#include <Engine/MultiplayerUi.hpp>

#include "TestMain.hpp"

#include <string_view>

int main()
{
	using net::udp::ClientStatus;
	using net::wire::RejectReason;
	using ui::MultiplayerState;
	using namespace std::chrono_literals;

	const auto checkState = [](ClientStatus transport, MultiplayerState expected,
		bool live, bool ready = false, bool degraded = false)
	{
		const auto status = ui::multiplayerStatus(transport, std::nullopt, false, ready, degraded);
		CHECK(status.state == expected && *status.label, "user-visible connection state");
		CHECK(ui::hasLiveNetworkStats(status.state) == live, "telemetry limited to the live scope");
	};
	checkState(ClientStatus::Disconnected, MultiplayerState::Disconnected, false);
	checkState(ClientStatus::Connecting, MultiplayerState::Connecting, false);
	checkState(ClientStatus::Connected, MultiplayerState::SyncingTerrain, true);
	checkState(ClientStatus::Connected, MultiplayerState::Connected, true, true);
	checkState(ClientStatus::Connected, MultiplayerState::Degraded, true, false, true);
	checkState(ClientStatus::Connected, MultiplayerState::Degraded, true, true, true);
	checkState(ClientStatus::Resyncing, MultiplayerState::Resyncing, false);
	checkState(ClientStatus::TimedOut, MultiplayerState::TimedOut, false);
	checkState(ClientStatus::Leaving, MultiplayerState::Leaving, false);

	const auto rejection = ui::multiplayerStatus(ClientStatus::Rejected, RejectReason::IdentityInUse, false, false, false);
	CHECK(rejection.state == MultiplayerState::Rejected && rejection.detail &&
		std::string_view(rejection.detail) == "Player profile is already connected", "rejection gives a safe explanation");
	const auto mismatch = ui::multiplayerStatus(ClientStatus::Disconnected, std::nullopt, true, false, false);
	CHECK(mismatch.state == MultiplayerState::Rejected && mismatch.detail &&
		std::string_view(mismatch.detail) == "World seed mismatch", "seed mismatch survives departure");
	const auto leaving = ui::multiplayerStatus(ClientStatus::Leaving, std::nullopt, true, false, false);
	CHECK(leaving.state == MultiplayerState::Leaving, "seed mismatch still shows in-flight departure");
	CHECK(!ui::hasLiveNetworkStats(mismatch.state) && !ui::hasLiveNetworkStats(rejection.state),
		"rejection hides retired telemetry");

	ui::NetworkStats stats;
	CHECK(!stats.ping && !stats.snapshot && !stats.traffic && !stats.prediction &&
		!stats.correctionsPerSecond(), "new or retired scope has no measurements");
	stats.snapshot = ui::NetworkStats::Snapshot{50ms, 2s, std::nullopt};
	stats.prediction = ui::NetworkStats::Prediction{};
	CHECK(!stats.snapshot->missingFraction && stats.correctionsPerSecond() == 0.0,
		"a first snapshot establishes prediction counters but cannot measure loss");
	stats.snapshot->missingFraction = 0.0;
	stats.traffic = ui::NetworkStats::Traffic{0.0, 0.0};
	stats.prediction->corrections = 3;
	CHECK(stats.snapshot->missingFraction == 0.0 && stats.traffic && stats.correctionsPerSecond() == 1.5,
		"measured zero and positive counters are preserved");
	return testmain::exitCode();
}
