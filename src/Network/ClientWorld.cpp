#include "ClientWorld.hpp"

namespace net::udp
{
namespace
{
glm::ivec3 worldCoord(wire::ChunkCoord coord)
{
	return {coord.x, 0, coord.z};
}

std::optional<wire::ChunkVersion> versionAt(const ChunkProjection &chunks, wire::ChunkCoord coord)
{
	const auto found = chunks.find(coord);
	return found == chunks.end() ? std::nullopt : std::optional{found->second.version};
}
}

void ClientWorld::TerrainDependencies::observe(wire::ChunkCoord coord,
	std::optional<wire::ChunkVersion> available)
{
	const auto active = std::span(m_entries).first(m_count);
	if (std::ranges::find(active, coord, &Entry::coord) != active.end())
		return;
	if (m_count == m_entries.size())
		m_overflow = true;
	else
		m_entries[m_count++] = {coord, available};
}

std::optional<wire::ChunkVersion> ClientWorld::CollisionView::matchingVersion(
	wire::ChunkCoord coord, std::optional<wire::ChunkVersion> verified) const
{
	const auto published = m_client.m_installed.find(coord);
	return published != m_client.m_installed.end() && verified == published->second ? verified : std::nullopt;
}

physics::Cell ClientWorld::CollisionView::sample(glm::ivec3 cell) const
{
	if (const auto boundary = WorldCollisionView::boundaryCell(cell))
		return *boundary;
	const wire::ChunkCoord coord{floorDivChunk(cell.x), floorDivChunk(cell.z)};
	const auto active = std::span(m_cache).first(m_cached);
	const auto cached = std::ranges::find(active, coord, &Entry::coord);
	if (cached != active.end())
		return cached->matches ? m_world.sample(cell) : physics::Cell{};
	const auto version = matchingVersion(coord, versionAt(m_verified, coord));
	const auto value = version ? m_world.sample(cell) : physics::Cell{};
	if (m_dependencies)
		m_dependencies->observe(coord, value.available ? version : std::nullopt);
	m_cache[m_next] = {coord, value.available};
	m_next = (m_next + 1) % m_cache.size();
	m_cached = std::min(m_cached + 1, m_cache.size());
	return value;
}

ClientWorld::TerrainChanges ClientWorld::CollisionView::terrainChanges() const
{
	if (!m_dependencies)
		return {};
	TerrainChanges changes{.invalidated = m_dependencies->m_overflow};
	for (const auto &[coord, before] : std::span(m_dependencies->m_entries).first(m_dependencies->m_count))
	{
		const auto verified = versionAt(m_verified, coord);
		auto available = matchingVersion(coord, verified);
		// An interest re-entry can have the same wire version while the
		// old record is still retiring. Probe backing without tracking it.
		if (available && !m_world.sample(worldCoord(coord) * int(CHUNK_SIZE)).available)
			available.reset();
		changes.invalidated |= before != available;
		changes.awaitingPublication |= before && verified && !available;
	}
	return changes;
}

void ClientWorld::CollisionView::resetDependencies() const
{
	if (m_dependencies)
	{
		*m_dependencies = {};
		m_cached = m_next = 0;
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
