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

	struct TerrainChanges
	{
		bool invalidated{};
		bool awaitingPublication{};
	};

	class CollisionView;
	class TerrainDependencies
	{
	public:
		static constexpr size_t kCapacity = 16;

	private:
		friend class CollisionView;
		struct Entry
		{
			wire::ChunkCoord coord;
			std::optional<wire::ChunkVersion> available;
		};
		void observe(wire::ChunkCoord coord, std::optional<wire::ChunkVersion> available);

		std::array<Entry, kCapacity> m_entries{};
		size_t m_count{};
		bool m_overflow{};
	};

	// Only matching, readable backing is available to collision.
	class CollisionView final : public physics::VoxelCollisionWorld
	{
	public:
		CollisionView(const ClientWorld &client, const ChunkProjection &verified,
			TerrainDependencies *dependencies = nullptr)
			: m_client(client), m_verified(verified), m_world(client.m_world), m_dependencies(dependencies) {}
		physics::Cell sample(glm::ivec3 cell) const override;
		TerrainChanges terrainChanges() const;
		void resetDependencies() const;

	private:
		std::optional<wire::ChunkVersion> matchingVersion(wire::ChunkCoord coord,
			std::optional<wire::ChunkVersion> verified) const;

		const ClientWorld &m_client;
		const ChunkProjection &m_verified;
		WorldCollisionView m_world;
		TerrainDependencies *m_dependencies;
		struct Entry { wire::ChunkCoord coord; bool matches; };
		mutable std::array<Entry, 4> m_cache{};
		mutable size_t m_cached{}, m_next{};
	};

	void reconcile(const ChunkProjection &chunks, glm::vec3 observer);
	std::optional<wire::ChunkVersion> observedVersion(glm::ivec3 cell) const;
	VoxelWorld &world() { return m_world; }

private:
	VoxelWorld m_world;
	std::map<wire::ChunkCoord, wire::ChunkVersion> m_installed;
};

} // namespace net::udp
