#include "ClientWorld.hpp"

namespace net::udp
{
namespace
{
glm::ivec3 worldCoord(wire::ChunkCoord coord)
{
	return {coord.x, 0, coord.z};
}
}

void ClientWorld::reconcile(const ChunkProjection &chunks, glm::vec3 observer)
{
	static_assert(wire::kChunkBytes == CHUNK_VOLUME);
	assert(chunks.size() <= wire::kMaxReplicatedChunks);
	for (auto it = m_installed.begin(); it != m_installed.end();)
	{
		const auto next = chunks.find(it->first);
		const auto coord = worldCoord(it->first);
		const auto *record = m_world.getChunk(coord);
		if ((next == chunks.end() || next->second.version.generation != it->second.generation ||
		     record->isReplicaRetiring()) && m_world.retireReplicaChunk(coord))
			it = m_installed.erase(it);
		else
			++it;
	}

	struct Candidate
	{
		wire::ChunkCoord coord;
		const ReplicatedChunk *image;
		float distance;
	};
	std::array<Candidate, wire::kMaxReplicatedChunks> candidates;
	size_t count = 0;
	for (const auto &[coord, image] : chunks)
	{
		const auto installed = m_installed.find(coord);
		if (installed != m_installed.end())
		{
			const auto *record = m_world.getChunk(worldCoord(coord));
			if (installed->second == image.version || record->isInTransit() || record->isReplicaRetiring())
				continue;
		}
		const glm::vec2 delta = glm::vec2(coord.x, coord.z) * float(CHUNK_SIZE) +
			float(CHUNK_SIZE / 2) - glm::vec2(observer.x, observer.z);
		candidates[count++] = {coord, &image, glm::dot(delta, delta)};
	}
	const size_t budget = std::min(count, kPublicationsPerFrame);
	std::partial_sort(candidates.begin(), candidates.begin() + budget, candidates.begin() + count,
		[](const Candidate &a, const Candidate &b) {
			return a.distance == b.distance ? a.coord < b.coord : a.distance < b.distance;
		});
	std::array<uint8_t, CHUNK_VOLUME> materials;
	for (const auto &[coord, image, distance] : std::span(candidates).first(budget))
	{
		if (m_installed.size() >= wire::kMaxReplicatedChunks && !m_installed.contains(coord))
			continue;
		std::ranges::transform(image->materials, materials.begin(),
			[](uint8_t material) { return material == wire::kAirMaterial ? uint8_t(AIR) : material; });
		if (m_world.publishReplicaChunk(worldCoord(coord), materials))
			m_installed.insert_or_assign(coord, image->version);
	}
}

std::optional<wire::ChunkVersion> ClientWorld::observedVersion(glm::ivec3 cell) const
{
	const wire::ChunkCoord coord{floorDivChunk(cell.x), floorDivChunk(cell.z)};
	const auto it = m_installed.find(coord);
	if (it == m_installed.end() || m_world.getChunk(worldCoord(coord))->isReplicaRetiring())
		return std::nullopt;
	return it->second;
}

} // namespace net::udp
