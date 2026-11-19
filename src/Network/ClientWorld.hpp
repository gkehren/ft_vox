#pragma once

#include "ClientTransport.hpp"

#include <World/VoxelWorld.hpp>
#include <World/WorldCollisionView.hpp>

namespace net::udp
{

// One session's disposable render/query backing. Wire versions are kept
// separately from the record identities used by local mesh and light jobs.
class ClientWorld
{
public:
	static constexpr size_t kPublicationsPerFrame = 4;

	// The collision view excludes terrain whose publication is still pending
	// so both an old mesh and the newest verified revision can block prediction.
	class CollisionView final : public physics::VoxelCollisionWorld
	{
	public:
		CollisionView(const ClientWorld &client, const ChunkProjection &verified)
			: m_client(client), m_verified(verified), m_world(client.m_world) {}
		physics::Cell sample(glm::ivec3 cell) const override;

	private:
		const ClientWorld &m_client;
		const ChunkProjection &m_verified;
		WorldCollisionView m_world;
		struct Entry { wire::ChunkCoord coord; bool matches; };
		mutable std::array<Entry, 4> m_cache{};
		mutable size_t m_cached{}, m_next{};
	};

	struct TerrainFootprint
	{
		wire::ChunkCoord center;
		std::array<std::optional<wire::ChunkVersion>, 9> versions{};
		bool invalidatedBy(const ChunkProjection &verified) const;
	};
	static TerrainFootprint terrainAround(const ChunkProjection &verified, glm::dvec3 feet);

	void reconcile(const ChunkProjection &chunks, glm::vec3 observer);
	std::optional<wire::ChunkVersion> observedVersion(glm::ivec3 cell) const;
	VoxelWorld &world() { return m_world; }

private:
	VoxelWorld m_world;
	std::map<wire::ChunkCoord, wire::ChunkVersion> m_installed;
};

} // namespace net::udp
