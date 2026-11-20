#pragma once

#include <Sim/PlayerId.hpp>
#include <World/WorldSave.hpp>

namespace worldsave
{
	inline constexpr char kIdentityFile[] = "server-identities.state";
	inline constexpr char kRosterFile[] = "players.state";
	inline constexpr size_t kMaxPersistentPlayers = 4096;

	struct RemotePlayerState
	{
		sim::PlayerId id;
		PlayerPersistState pose;
		InventoryPersistState inventory;
		std::array<double, 3> velocity{};
	};

	struct PlayerRoster
	{
		uint64_t worldId{};
		std::vector<RemotePlayerState> players; // ascending persistent ID
	};

	std::vector<uint8_t> encodePlayerRoster(const PlayerRoster &roster);
	SaveStatus readPlayerRoster(const std::filesystem::path &path, PlayerRoster &out,
								uint64_t *checksum = nullptr);
	SaveStatus decodePlayerRoster(const uint8_t *data, size_t size, PlayerRoster &out);
}
