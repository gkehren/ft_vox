#pragma once

#include <Sim/PlayerId.hpp>
#include <World/WorldSave.hpp>

namespace worldsave
{
	inline constexpr char kIdentityFile[] = "server-identities.state";
	inline constexpr char kRosterFile[] = "players.state";
	inline constexpr size_t kMaxPersistentPlayers = 4096;
	inline constexpr size_t kIdentityProfileBytes = 4 + 32; // actor ID + key
	inline constexpr size_t kIdentityChecksumBytes = 8;
	inline constexpr size_t kEmptyIdentityRegistryBytes = 41 + kIdentityChecksumBytes;
	inline constexpr uint32_t kFirstRemotePlayerId = 2;

	struct IdentityRegistryHeader
	{
		uint64_t worldId{};
		int seed{};
		uint32_t generatorVersion{};
		bool checkpointRequired{};
		uint32_t actorLimit{kFirstRemotePlayerId};
		uint64_t itemLimit{1};
		uint32_t profileCount{};
	};

	// Validate the header, exact file size and checksum. On success the reader
	// is positioned at the first profile record.
	bool readIdentityRegistryHeader(const std::vector<uint8_t> &bytes, ByteReader &reader,
									IdentityRegistryHeader &out);
	void writeIdentityRegistryHeader(ByteWriter &writer, const IdentityRegistryHeader &header);

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
