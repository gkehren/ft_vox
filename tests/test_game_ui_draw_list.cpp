#include <Renderer/GameUiDrawList.hpp>

#include "TestMain.hpp"

int main()
{
	using gameui::Clip;
	using gameui::DrawList;
	static_assert(DrawList::kMaxBatches + 1 <= 32);
	DrawList draws;
	const glm::vec4 area{0, 0, 100, 100};
	const glm::vec4 white(1.f);
	draws.reset({100, 100}, 2);
	const auto full = draws.bounds();
	const Clip parent{{-10, 5, 80, 110}};
	const Clip child{{10, 0, 90, 60}};
	const Clip nested = parent.intersect(child);
	CHECK(nested.rect == glm::vec4(10, 5, 80, 60), "nested clips intersect");
	draws.panel(area, white, parent);
	draws.glyph(area, {0, 0, 1, 1}, white, nested);
	draws.icon(area, {2, 7}, white, nested);
	draws.panel(area, white, parent);
	draws.panel({80, 0, 100, 10}, white, nested);
	draws.panel(area, white, child.intersect({{90, 0, 100, 100}}));
	draws.icon(area, {1, 9}, white, full);
	const auto batches = draws.batches();
	const auto quads = draws.quads();
	CHECK(batches.size() == 3 && quads.size() == 4 && draws.dropped() == 0,
		"only consecutive clips batch, and clipped or stale primitives leave capacity available");
	CHECK(batches.size() == 3 && batches[0].clip.rect == glm::vec4(0, 5, 80, 100) &&
		batches[1].clip == nested && batches[1].first == 1 && batches[1].count == 2 &&
		batches[2].clip == batches[0].clip && batches[2].first == 3, "clipped batches preserve paint order");
	CHECK(quads.size() == 4 && quads[0].layer == -1 && quads[1].layer == -2 && quads[2].layer == 7 &&
		quads[3].layer == -1, "primitive order is stable");

	draws.reset({100, 100}, 2);
	for (uint32_t i = 0; i < DrawList::kMaxQuads; ++i)
		draws.icon(area, {2, i}, white, full);
	draws.panel(area, white, full);
	CHECK(draws.quads().size() == DrawList::kMaxQuads && draws.dropped() == 1 &&
		draws.batches().size() == 1 && draws.batches()[0].count == DrawList::kMaxQuads,
		"quad overflow counts only the rejected submission");
	CHECK(!draws.quads().empty() && draws.quads().back().layer == int32_t(DrawList::kMaxQuads - 1),
		"quad overflow preserves the last accepted primitive");

	draws.reset({100, 100}, 2);
	const Clip inset{{1, 1, 99, 99}};
	for (uint32_t i = 0; i < DrawList::kMaxBatches; ++i)
		draws.panel(area, white, i % 2 == 0 ? full : inset);
	draws.panel(area, white, inset);
	draws.panel(area, white, full);
	CHECK(draws.batches().size() == DrawList::kMaxBatches && draws.dropped() == 1 &&
		draws.quads().size() == DrawList::kMaxBatches + 1 &&
		draws.batches().back().first == DrawList::kMaxBatches - 1 && draws.batches().back().count == 2,
		"batch overflow rejects a new clip and still accepts the final batch's clip");
	draws.reset({100, 100}, 3);
	CHECK(draws.quads().empty() && draws.batches().empty() && draws.dropped() == 0,
		"reset clears the previous frame's work and counters");
	return testmain::exitCode();
}
