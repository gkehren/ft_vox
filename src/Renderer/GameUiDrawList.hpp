#pragma once

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace gameui
{
// Logical window coordinates. Colors are authored in sRGB with linear alpha.
struct Clip
{
	glm::vec4 rect{};
	bool operator==(const Clip &) const = default;

	Clip intersect(Clip other) const
	{
		return {{std::max(rect.x, other.rect.x), std::max(rect.y, other.rect.y),
			std::min(rect.z, other.rect.z), std::min(rect.w, other.rect.w)}};
	}

	bool empty() const { return !(rect.x < rect.z && rect.y < rect.w); }
};

struct AtlasIcon
{
	uint64_t generation{};
	uint32_t layer{};
};

struct Quad
{
	glm::vec4 rect;
	glm::vec4 uv;
	glm::vec4 color;
	int32_t layer; // -1 solid, -2 font coverage, otherwise a block-atlas layer
};

struct Batch
{
	Clip clip;
	uint32_t first;
	uint32_t count;
};

class DrawList
{
public:
	static constexpr uint32_t kMaxQuads = 2048;
	static constexpr uint32_t kMaxBatches = 32;

	void reset(glm::vec2 logicalSize, uint64_t atlasGeneration)
	{
		m_size = logicalSize;
		m_atlasGeneration = atlasGeneration;
		m_count = m_batchCount = 0;
		m_dropped = 0;
	}

	Clip bounds() const { return {{0.f, 0.f, m_size.x, m_size.y}}; }
	glm::vec2 size() const { return m_size; }
	uint64_t atlasGeneration() const { return m_atlasGeneration; }
	uint32_t dropped() const { return m_dropped; }
	std::span<const Quad> quads() const { return {m_quads.data(), m_count}; }
	std::span<const Batch> batches() const { return {m_batches.data(), m_batchCount}; }

	void panel(glm::vec4 rect, glm::vec4 color, Clip clip)
	{
		append({rect, {}, color, -1}, clip);
	}

	void glyph(glm::vec4 rect, glm::vec4 uv, glm::vec4 color, Clip clip)
	{
		append({rect, uv, color, -2}, clip);
	}

	void icon(glm::vec4 rect, AtlasIcon icon, glm::vec4 color, Clip clip)
	{
		if (icon.generation != m_atlasGeneration || icon.layer > INT32_MAX)
			return;
		append({rect, {0.f, 0.f, 1.f, 1.f}, color, static_cast<int32_t>(icon.layer)}, clip);
	}

private:
	void append(const Quad &quad, Clip clip)
	{
		clip = clip.intersect(bounds());
		if (clip.intersect({quad.rect}).empty())
			return;
		const bool newBatch = m_batchCount == 0 || !(m_batches[m_batchCount - 1].clip == clip);
		if (m_count == kMaxQuads || (newBatch && m_batchCount == kMaxBatches))
		{
			++m_dropped;
			return;
		}
		if (newBatch)
			m_batches[m_batchCount++] = {clip, m_count, 0};
		m_quads[m_count++] = quad;
		++m_batches[m_batchCount - 1].count;
	}

	std::array<Quad, kMaxQuads> m_quads{};
	std::array<Batch, kMaxBatches> m_batches{};
	glm::vec2 m_size{};
	uint64_t m_atlasGeneration{};
	uint32_t m_count{}, m_batchCount{}, m_dropped{};
};
} // namespace gameui
