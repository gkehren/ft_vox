#pragma once

#include "ClientTransport.hpp"

#include <World/VoxelWorld.hpp>

namespace net::udp
{

// One session's disposable render/query backing. Wire versions are kept
// separately from the record identities used by local mesh and light jobs.
class ClientWorld
{
public:
	static constexpr size_t kPublicationsPerFrame = 4;

	void reconcile(const ChunkProjection &chunks, glm::vec3 observer);
	std::optional<wire::ChunkVersion> observedVersion(glm::ivec3 cell) const;
	VoxelWorld &world() { return m_world; }

private:
	VoxelWorld m_world;
	std::map<wire::ChunkCoord, wire::ChunkVersion> m_installed;
};

} // namespace net::udp
