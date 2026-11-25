#pragma once

#include <glm/common.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>

namespace gameui
{
// Logical window coordinates.
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

inline glm::vec2 size(Clip rect)
{
	return {std::max(0.f, rect.rect.z - rect.rect.x), std::max(0.f, rect.rect.w - rect.rect.y)};
}

inline Clip rectangle(glm::vec2 origin, glm::vec2 extent)
{
	return {{origin.x, origin.y, origin.x + extent.x, origin.y + extent.y}};
}

inline Clip inset(Clip rect, float padding)
{
	return {{rect.rect.x + padding, rect.rect.y + padding, rect.rect.z - padding, rect.rect.w - padding}};
}

inline bool hit(Clip rect, glm::vec2 point)
{
	return point.x >= rect.rect.x && point.x < rect.rect.z &&
		point.y >= rect.rect.y && point.y < rect.rect.w;
}

inline Clip anchor(Clip bounds, glm::vec2 preferred, glm::vec2 alignment)
{
	const auto available = size(bounds);
	const auto extent = glm::min(glm::max(preferred, glm::vec2(0)), available);
	const auto position = glm::vec2(bounds.rect) + (available - extent) * glm::clamp(alignment, 0.f, 1.f);
	return rectangle(position, extent);
}

class StackLayout
{
public:
	StackLayout(Clip bounds, bool vertical, float spacing)
		: m_bounds(bounds), m_axis(vertical ? 1 : 0), m_spacing(std::max(0.f, spacing)),
		  m_cursor(bounds.rect[m_axis]) {}

	Clip next(float length)
	{
		auto result = m_bounds;
		const float end = m_bounds.rect[m_axis + 2];
		result.rect[m_axis] = std::min(m_cursor, end);
		result.rect[m_axis + 2] = std::min(m_cursor + std::max(0.f, length), end);
		m_cursor = result.rect[m_axis + 2] + m_spacing;
		return result;
	}

private:
	Clip m_bounds;
	int m_axis;
	float m_spacing, m_cursor;
};

inline Clip gridCell(Clip bounds, unsigned columns, unsigned rows, unsigned column, unsigned row, float spacing)
{
	if (!columns || !rows || column >= columns || row >= rows)
		return {};
	spacing = std::max(0.f, spacing);
	const glm::vec2 count{columns, rows};
	const glm::vec2 cell = glm::max(glm::vec2(0), (size(bounds) - (count - 1.f) * spacing) / count);
	return rectangle(glm::vec2(bounds.rect) + glm::vec2(column, row) * (cell + spacing), cell).intersect(bounds);
}

struct ScrollViewport
{
	Clip clip{};
	float contentHeight{}, offset{};

	void setOffset(float requested)
	{
		offset = std::clamp(requested, 0.f, std::max(0.f, contentHeight - size(clip).y));
	}
	Clip place(Clip content) const
	{
		return {{content.rect.x, content.rect.y - offset, content.rect.z, content.rect.w - offset}};
	}
	void reveal(Clip content)
	{
		const Clip placed = place(content);
		if (placed.rect.y < clip.rect.y)
			setOffset(offset + placed.rect.y - clip.rect.y);
		else if (placed.rect.w > clip.rect.w)
			setOffset(offset + placed.rect.w - clip.rect.w);
	}
};

// Content scale and framebuffer pixel density are distinct in SDL. The same
// logical bounds drive hit testing and the renderer's inward-rounded scissor.
inline glm::vec2 framebufferScale(glm::vec2 logical, glm::vec2 pixels)
{
	return logical.x > 0 && logical.y > 0 ? pixels / logical : glm::vec2(0);
}

inline Clip pixelClip(Clip logical, glm::vec2 scale, glm::vec2 pixels)
{
	const auto &r = logical.rect;
	return {{std::clamp(std::ceil(r.x * scale.x), 0.f, pixels.x),
		std::clamp(std::ceil(r.y * scale.y), 0.f, pixels.y),
		std::clamp(std::floor(r.z * scale.x), 0.f, pixels.x),
		std::clamp(std::floor(r.w * scale.y), 0.f, pixels.y)}};
}
} // namespace gameui
