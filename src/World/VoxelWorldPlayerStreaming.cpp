#include "VoxelWorld.hpp"

#include <algorithm>
#include <climits>
#include <tuple>

namespace
{
constexpr int kMaxRadius = sim::kMaxPlayerInterestRadiusChunks;

constexpr int squaredDistance(glm::ivec3 offset)
{
	return offset.x * offset.x + offset.z * offset.z;
}

const std::vector<glm::ivec3> &nearestOffsets()
{
	static const auto offsets = [] {
		std::vector<glm::ivec3> result;
		for (int x = -kMaxRadius; x <= kMaxRadius; ++x)
			for (int z = -kMaxRadius; z <= kMaxRadius; ++z)
				if (x * x + z * z <= kMaxRadius * kMaxRadius)
					result.emplace_back(x, 0, z);
		std::sort(result.begin(), result.end(), [](glm::ivec3 a, glm::ivec3 b) {
			return std::tuple(squaredDistance(a), a.x, a.z) <
				   std::tuple(squaredDistance(b), b.x, b.z);
		});
		return result;
	}();
	return offsets;
}

bool insideWorld(glm::ivec3 coord)
{
	const int x = coord.x * CHUNK_SIZE;
	const int z = coord.z * CHUNK_SIZE;
	return x >= SHRT_MIN && x < SHRT_MAX && z >= SHRT_MIN && z < SHRT_MAX;
}
}

// Each source gets one coordinate per turn. Drop exhausted sources for this
// pass and resume after the last served id on the next tick, even if it left.
// The optional source at invalid id 0 drains canonical edits left outside
// interest, competing for the same generation budget as ordinary work.
template <typename Eligible>
std::vector<glm::ivec3> VoxelWorld::selectPlayerChunks(size_t budget, sim::PlayerId &lastServed,
													 std::span<const glm::ivec3> stranded,
													 Eligible eligible) const
{
	std::vector<glm::ivec3> selected;
	if (budget == 0)
		return selected;
	struct Cursor
	{
		sim::PlayerId id;
		glm::ivec3 center;
		int radiusSq;
		size_t next{0};
	};
	std::deque<Cursor> sources;
	if (!stranded.empty())
		sources.push_back({{}, {}, 0});
	for (const auto &interest : m_playerInterests)
		sources.push_back({interest.id, interest.center, interest.radiusChunks * interest.radiusChunks});
	const auto first = std::upper_bound(sources.begin(), sources.end(), lastServed,
		[](sim::PlayerId id, const Cursor &source) { return id < source.id; });
	std::rotate(sources.begin(), first, sources.end());

	selected.reserve(budget);
	std::unordered_set<glm::ivec3, IVec3Hash> picked;
	picked.reserve(budget);
	while (selected.size() < budget && !sources.empty())
	{
		Cursor source = sources.front();
		sources.pop_front();
		const bool player = source.id.valid();
		const auto coords = player ? std::span<const glm::ivec3>(nearestOffsets()) : stranded;
		while (source.next < coords.size())
		{
			const glm::ivec3 offset = coords[source.next++];
			if (player && squaredDistance(offset) > source.radiusSq)
				break;
			const glm::ivec3 coord = player ? source.center + offset : offset;
			if (!insideWorld(coord) || !eligible(coord) || !picked.insert(coord).second)
				continue;
			selected.push_back(coord);
			lastServed = source.id;
			sources.push_back(source);
			break;
		}
	}
	return selected;
}

bool VoxelWorld::advancePlayerStreaming(std::span<const PlayerInterest> interests, PlayerStreamingBudget budget)
{
	if (!m_playerStreaming)
	{
		// Transient solo worlds historically forget edits on unload. Refuse
		// switching after any such edit, since its previous value is untracked.
		if (hasPendingLogicalEdits() || outstandingGenerationCount() != 0 ||
			(!isWorldOpen() && m_nextEditId != 1))
			return false;
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		m_playerStreaming = true;
		m_loadQueue.clear();
		m_loadQueueHead = 0;
		m_enqueuedLoads.clear();
		m_desiredFootprint = {};
	}
	budget.loads = std::min(budget.loads, PlayerStreamingBudget::kMaxLoads);
	budget.generations = std::min(budget.generations, PlayerStreamingBudget::kMaxGenerations);
	budget.evictions = std::min(budget.evictions, PlayerStreamingBudget::kMaxEvictions);

	// Completion and pending edits go first. A worker lease stays on its
	// record until publication; no interest change can recycle its backing.
	pump();
	if (!std::ranges::equal(interests, m_playerInterests))
	{
		m_playerInterests.assign(interests.begin(), interests.end());
		m_playerDesired.clear();
		for (const auto &interest : interests)
		{
			const int radiusSq = interest.radiusChunks * interest.radiusChunks;
			for (glm::ivec3 offset : nearestOffsets())
			{
				if (squaredDistance(offset) > radiusSq)
					break;
				const glm::ivec3 coord = interest.center + offset;
				if (insideWorld(coord))
					m_playerDesired.insert(coord);
			}
		}
	}

	std::unordered_set<glm::ivec3, IVec3Hash> pinned;
	std::vector<glm::ivec3> stranded;
	for (const PendingVoxelEdit &edit : m_pendingEdits)
	{
		if (!edit.borderNeighbor && edit.generation == edit.chunk->generation() &&
			!m_playerDesired.contains(edit.chunkPos) &&
			pinned.insert(edit.chunkPos).second)
			stranded.push_back(edit.chunkPos);
	}

	std::vector<glm::ivec3> retire;
	PlayerStreamingStats stats;
	stats.observers = interests.size();
	stats.desiredChunks = m_playerDesired.size();
	size_t registeredDesired = 0;
	for (WorldChunk *chunk : m_activeChunks)
	{
		const glm::ivec3 coord = chunk->coord();
		const bool inTransit = chunk->isInTransit();
		if (m_playerDesired.contains(coord))
		{
			++registeredDesired;
			if (!inTransit && chunk->getState() == ChunkState::UNLOADED)
				++stats.waitingForGeneration;
		}
		else
		{
			++stats.outsideInterest;
			if (!inTransit && retire.size() < budget.evictions && !pinned.contains(coord))
				retire.push_back(coord);
		}
	}
	stats.evicted = retireChunks(retire);
	stats.outsideInterest -= stats.evicted;
	stats.waitingForRecord = stats.desiredChunks - registeredDesired;

	if (budget.loads > 0 && stats.waitingForRecord > 0)
	{
		ensureRecordCapacity(stats.desiredChunks + stats.outsideInterest);
		const auto loads = selectPlayerChunks(std::min<size_t>(budget.loads, recordFreeCount()),
			m_lastPlayerLoad, {}, [this](glm::ivec3 coord) { return !m_chunks.contains(coord); });
		stats.loaded = loads.size() - loadChunkRecords(loads).size();
		stats.waitingForRecord -= stats.loaded;
		stats.waitingForGeneration += stats.loaded;
	}

	if (m_threadPool)
	{
		const size_t limit = PlayerStreamingBudget::kMaxOutstandingGenerations;
		budget.generations = static_cast<unsigned>(std::min<size_t>(budget.generations,
			limit - std::min(limit, outstandingGenerationCount())));
	}
	if (budget.generations > 0 && (stats.waitingForGeneration > 0 || !stranded.empty()))
	{
		std::lock_guard<std::shared_mutex> lock(m_mutex);
		const auto candidates = selectPlayerChunks(budget.generations, m_lastPlayerGeneration, stranded,
			[this](glm::ivec3 coord) {
				const auto it = m_chunks.find(coord);
				return it != m_chunks.end() && !it->second->isInTransit() &&
					   it->second->getState() == ChunkState::UNLOADED;
			});
		std::vector<GenerationCandidate> queue;
		queue.reserve(candidates.size());
		for (glm::ivec3 coord : candidates)
			queue.push_back({m_chunks.at(coord)});
		// Use a common task priority so worker priority cannot supersede the
		// fair dispatch order. Completion order can still differ by worker.
		stats.generationStarts = m_threadPool ? dispatchGeneration(queue, std::nullopt) :
											   generateSynchronously(queue);
	}
	if (!m_threadPool && stats.generationStarts > 0)
		applyPendingEdits();

	// Successful starts include stranded edits. Count just the remaining
	// desired unknowns when that distinction matters to the backlog.
	if (stats.generationStarts > 0)
	{
		stats.waitingForGeneration = 0;
		for (const auto *chunk : m_activeChunks)
			if (!chunk->isInTransit() && chunk->getState() == ChunkState::UNLOADED &&
				m_playerDesired.contains(chunk->coord()))
				++stats.waitingForGeneration;
	}
	stats.pinnedEdits = pinned.size();
	stats.residentChunks = m_activeChunks.size();
	stats.outstandingGenerations = outstandingGenerationCount();
	m_playerStreamingStats = stats;
	return true;
}

size_t VoxelWorld::outstandingGenerationCount() const
{
	std::lock_guard lock(m_completedJobsMutex);
	return m_pendingGenJobsCount.load() + m_completedGenerationChunks.size();
}

void VoxelWorld::applyTransientPlayerEdits(WorldChunk *chunk)
{
	const auto it = m_transientPlayerEdits.find(chunk->coord());
	if (it == m_transientPlayerEdits.end())
		return;
	for (const auto &[index, type] : it->second)
	{
		const int x = index % CHUNK_SIZE;
		const int y = index / (CHUNK_SIZE * CHUNK_SIZE);
		const int z = (index / CHUNK_SIZE) % CHUNK_SIZE;
		chunk->setVoxel(x, y, z, static_cast<TextureType>(type));
	}
}
