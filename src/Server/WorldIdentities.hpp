#pragma once

#include <Network/WireProtocol.hpp>

#include <filesystem>
#include <map>
#include <string>

namespace server
{

// Identity metadata is published independently of gameplay. New allocations
// can survive a rollback, while possession and item effects live in checkpoints.
class WorldIdentities
{
public:
	~WorldIdentities();
	WorldIdentities() = default;
	WorldIdentities(const WorldIdentities &) = delete;
	WorldIdentities &operator=(const WorldIdentities &) = delete;

	bool lock(const std::filesystem::path &worldDir, std::string &error);
	bool load(int seed, uint32_t generator, uint64_t checkpointWorldId, bool hasCheckpoint,
			  std::string &error);
	bool markCheckpointRequired();
	uint64_t worldId() const { return m_worldId; }
	size_t size() const { return m_profiles.size(); }
	bool contains(sim::PlayerId id) const { return m_profiles.contains(id); }
	bool includesItemWatermark(uint64_t next) const { return next <= m_itemLimit; }
	sim::PlayerId find(const net::wire::PlayerKey &key) const;
	sim::PlayerId allocatePlayer();
	bool enroll(sim::PlayerId id, const net::wire::PlayerKey &key);
	bool reserveItems(uint64_t checkpointNext, uint64_t &next, uint64_t &limit);

private:
	bool save() const;
	bool decode(const std::vector<uint8_t> &bytes);

	std::filesystem::path m_path;
	// Native lock ownership lasts through checkpoint publication and world teardown.
	intptr_t m_lock{-1};
	uint64_t m_worldId{};
	int m_seed{};
	uint32_t m_generator{};
	bool m_checkpointRequired{};
	uint32_t m_actorLimit{2};
	uint32_t m_nextActor{2};
	uint64_t m_itemLimit{1};
	std::map<sim::PlayerId, net::wire::PlayerKey> m_profiles;
};

} // namespace server
