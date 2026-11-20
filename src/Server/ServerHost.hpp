#pragma once

#include <Sim/GameSimulation.hpp>
#include <Network/ServerTransport.hpp>
#include <Server/WorldIdentities.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace server
{

inline constexpr int kTickRate = net::wire::kTickRate;
inline constexpr double kTickDt = 1.0 / kTickRate;

struct HostConfig
{
	sim::GameSimulation::Config world;
	unsigned workers{2}; // zero uses synchronous generation
	int interestRadiusChunks{3};
	PlayerStreamingBudget streaming;
};

enum class TerrainTiming { Background, QuiescePerTick };

struct HostMetrics
{
	uint64_t ticks{0};
	uint64_t worldEpoch{0};
	uint64_t nextEditId{0};
	uint64_t checkpointGeneration{0};
	size_t chunks{0};
	size_t players{0};
	size_t mobs{0};
	size_t items{0};
	size_t pendingGeneration{0};
	size_t waitingForRecord{0};
	size_t waitingForGeneration{0};
	int seed{0};
};

/// One world, driven exclusively by its caller thread. Workers compute terrain
/// and write persistence payloads; the owner publishes results and mutates
/// gameplay state. The executable runs this host on its main thread.
class ServerHost : public net::udp::SessionOwner
{
public:
	explicit ServerHost(HostConfig config);

	[[nodiscard]] bool start(std::string &error);
	[[nodiscard]] bool step(TerrainTiming terrainTiming, std::string &error);
	[[nodiscard]] bool stop(std::string &error);
	HostMetrics metrics() const;
	uint64_t ticks() const { return m_ticks; }
	bool restoredPlayer() const { return m_restoredPlayer; }
	net::udp::SessionWorld sessionWorld() const;
	net::udp::SessionAdmission joinRemote(const net::wire::PlayerKey &key = net::udp::randomPlayerKey()) override;
	bool activateRemote(sim::PlayerId player) override;
	void leaveRemote(sim::PlayerId player) override;
	std::optional<net::wire::ActionResult> receiveRemote(
		sim::PlayerId player, const net::udp::SessionIntents &intents) override;
	net::wire::PlayerSnapshot snapshotRemote(sim::PlayerId player) const override;
	std::optional<net::wire::Inventory> inventoryRemote(sim::PlayerId player) override;
	std::optional<net::udp::EntitySnapshots> entitySnapshotsRemote(sim::PlayerId player) const override;
	std::optional<net::udp::WorldUpdate> nextWorldUpdate(sim::PlayerId player, bool allowBaseline) override;
	void suspendPlayerInput();

private:
	friend struct ServerHostProbe;

	bool start(std::string &error, void (*beforeInitialCheckpointForTests)());
	bool quiesce(std::string &error);
	bool save(std::string &error);
	bool reserveItemIds(std::string &error);
	void advancePlayers();
	void distributeWorldEdits();
	net::wire::ActionResult applyAction(sim::PlayerId player, const net::wire::Action &action);

	struct RemotePlayer
	{
		net::udp::InputQueue inputs{};
		uint64_t lastQueuedSequence{}, lastQueuedTick{}, lastProcessedInput{};
		uint64_t nextActionId{1};
		net::wire::Inventory inventory;
		std::map<net::wire::ChunkCoord, net::wire::ChunkVersion> chunks;
		std::deque<net::wire::WorldEdit> edits;
		uint64_t nextTransferId{1};
		bool worldGap{};
		std::optional<net::wire::PlayerKey> pendingKey;
	};

	HostConfig m_config;
	std::unique_ptr<WorldIdentities> m_identities;
	// Destruction is reversed: drain and destroy the world before its pool.
	std::unique_ptr<ThreadPool> m_workers;
	std::unique_ptr<sim::GameSimulation> m_sim;
	std::map<sim::PlayerId, RemotePlayer> m_remotePlayers;
	std::map<sim::PlayerId, worldsave::RemotePlayerState> m_savedPlayers;
	uint64_t m_itemIdLimit{1};
	glm::dvec3 m_spawnCenter{};
	std::vector<WorldEvent> m_worldEvents;
	std::vector<LogicalVoxelEdit> m_logicalEdits;
	std::vector<items::ItemEntityEvent> m_itemEvents;
	HostMetrics m_finalMetrics;
	uint64_t m_ticks{0};
	float m_yaw{0.f};
	float m_pitch{0.f};
	bool m_restoredPlayer{false};
};

} // namespace server
