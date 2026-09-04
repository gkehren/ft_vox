// MeshResultPool contract (issue #104): pointer-stable reusable mesh build
// results detached from Chunk lifetime. Covers acquire/release/reuse,
// capacity retention across jobs, beginBuild/detach identity bookkeeping,
// reserve and high-water behavior, concurrency (a block is never handed to
// two holders at once), wrong-pool/double release refusal (Release-safe),
// growth events and stats() aggregation.
#include <Chunk/ChunkMeshResult.hpp>
#include <Engine/WorkloadTelemetry.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

static int g_fails = 0;

#define CHECK(cond, msg)                                                       \
	do                                                                         \
	{                                                                          \
		if (!(cond))                                                           \
		{                                                                      \
			std::cerr << "FAIL: " << msg << " (" << __LINE__ << ")\n";         \
			++g_fails;                                                         \
		}                                                                      \
	} while (0)

static void fillJunk(MeshBuildResult &r)
{
	for (int i = 0; i < 1000; ++i)
	{
		Vertex v{};
		v.position = glm::vec3(float(i));
		r.opaqueVertices.push_back(v);
		r.opaqueIndices.push_back(uint32_t(i));
		if (i < 50)
		{
			r.waterVertices.push_back(v);
			r.waterIndices.push_back(uint32_t(i));
		}
	}
}

int main()
{
	// 1) Single acquire/release and reuse of the same block.
	{
		MeshResultPool pool;
		MeshBuildResult *first = pool.acquire();
		CHECK(first != nullptr, "acquire returns a block");
		CHECK(first->homePool == &pool, "acquire stamps the home pool");
		pool.release(first);
		CHECK(pool.capacity() == 1, "one block retained after release");
		CHECK(pool.activeCount() == 0, "no active blocks after release");
		CHECK(pool.freeCount() == 1, "released block is in the free list");
		MeshBuildResult *second = pool.acquire();
		CHECK(second == first, "reuse hands back the same block");
		CHECK(pool.capacity() == 1, "reuse does not grow the pool");
		pool.release(second);
	}

	// 2) Capacity retention: a released block keeps its vector capacities
	// (minus sizes) so the next borrower rebuilds without reallocation.
	{
		MeshResultPool pool;
		MeshBuildResult *r = pool.acquire();
		r->beginBuild(nullptr, 0);
		fillJunk(*r);
		const size_t opaqueCap = r->opaqueVertices.capacity();
		const size_t waterCap = r->waterVertices.capacity();
		CHECK(opaqueCap >= 1000, "junk fill grows opaque capacity");
		pool.release(r);
		CHECK(r->opaqueVertices.empty(), "detach drops opaque sizes");
		CHECK(r->waterIndices.empty(), "detach drops water sizes");
		CHECK(r->owner == nullptr && r->generation == 0, "detach clears identity");
		CHECK(r->opaqueVertices.capacity() == opaqueCap, "detach retains opaque capacity");
		CHECK(r->waterVertices.capacity() == waterCap, "detach retains water capacity");

		MeshResultPoolStats stats = pool.stats();
		CHECK(stats.capacity == 1 && stats.active == 0 && stats.free == 1,
			  "stats counts one free block");
		CHECK(stats.opaqueVertexCapacity >= opaqueCap * sizeof(Vertex),
			  "stats aggregates retained opaque vertex capacity");
		CHECK(stats.opaqueVertexSize == 0, "free blocks carry no live payload bytes");
		CHECK(stats.capacityBytes() ==
				  stats.opaqueVertexCapacity + stats.opaqueIndexCapacity +
					  stats.waterVertexCapacity + stats.waterIndexCapacity,
			  "capacityBytes is the sum of the four vector capacities");

		// Active blocks' sizes show up while the block is lent out.
		MeshBuildResult *r2 = pool.acquire();
		CHECK(r2 == r, "reuse after release");
		r2->beginBuild(nullptr, 7);
		fillJunk(*r2);
		stats = pool.stats();
		CHECK(stats.active == 1 && stats.free == 0, "active block leaves the free list");
		CHECK(stats.opaqueVertexSize >= 1000 * sizeof(Vertex),
			  "stats aggregates live payload of active block");
		CHECK(stats.opaqueVertexCapacity >= stats.opaqueVertexSize,
			  "capacity covers live payload");
		pool.release(r2);
	}

	// 3) beginBuild clears content but keeps capacity and stamps identity.
	{
		MeshResultPool pool;
		MeshBuildResult *r = pool.acquire();
		r->beginBuild(nullptr, 3);
		fillJunk(*r);
		const size_t capBefore = r->opaqueIndices.capacity();
		r->beginBuild(nullptr, 42);
		CHECK(r->opaqueVertices.empty() && r->opaqueIndices.empty() &&
				  r->waterVertices.empty() && r->waterIndices.empty(),
			  "beginBuild clears all four vectors");
		CHECK(r->opaqueIndices.capacity() == capBefore,
			  "beginBuild keeps capacity for reuse (no allocator churn)");
		CHECK(r->generation == 42, "beginBuild stamps generation");
		pool.release(r);
	}

	// 4) Reserve pre-allocates; burst then steady-state reuse shows the
	// high-water behavior with no growth after the burst.
	{
		MeshResultPool pool;
		pool.reserve(8);
		CHECK(pool.capacity() == 8, "reserve pre-allocates capacity");
		std::vector<MeshBuildResult *> held;
		for (int i = 0; i < 24; ++i)
			held.push_back(pool.acquire());
		CHECK(pool.capacity() == 24, "burst grows the pool to the working set");
		for (auto *r : held)
			pool.release(r);
		for (int i = 0; i < 1000; ++i)
		{
			MeshBuildResult *r = pool.acquire();
			pool.release(r);
		}
		CHECK(pool.capacity() == 24, "steady-state reuse does not grow the pool");
		CHECK(pool.activeCount() == 0, "steady-state reuse ends with 0 active");
		CHECK(pool.freeCount() == 24, "all blocks back in the free list");
	}

	// 5) Growth telemetry: allocation beyond the free list raises one grow
	// event; free-list reuse and reserve() preallocation do not.
	{
		auto &registry = telemetry::registry();
		if (registry.enabled)
		{
			const auto before = registry.snapshot();
			MeshResultPool pool;
			MeshBuildResult *a = pool.acquire();
			const auto afterFirst = registry.snapshot();
			CHECK(afterFirst.events[telemetry::MeshPoolGrow] ==
					  before.events[telemetry::MeshPoolGrow] + 1,
				  "first allocation is one grow event");
			pool.release(a);
			MeshBuildResult *b = pool.acquire();
			const auto afterReuse = registry.snapshot();
			CHECK(afterReuse.events[telemetry::MeshPoolGrow] ==
					  afterFirst.events[telemetry::MeshPoolGrow],
				  "free-list reuse is not growth");
			pool.release(b);
			pool.reserve(4);
			const auto afterReserve = registry.snapshot();
			CHECK(afterReserve.events[telemetry::MeshPoolGrow] ==
					  afterReuse.events[telemetry::MeshPoolGrow],
				  "reserve() preallocation is not growth");
			std::vector<MeshBuildResult *> held;
			while (pool.freeCount() > 0)
				held.push_back(pool.acquire()); // drains the free list
			MeshBuildResult *d = pool.acquire(); // beyond the free list
			const auto afterGrow = registry.snapshot();
			CHECK(afterGrow.events[telemetry::MeshPoolGrow] ==
					  afterReserve.events[telemetry::MeshPoolGrow] + 1,
				  "allocation beyond the free list raises one grow event");
			pool.release(d);
			for (auto *r : held)
				pool.release(r);
		}
	}

	// 6) Wrong-pool release must be refused without corrupting either pool.
	// Release-only: in Debug the ownership assert fires, making the
	// programming error immediately visible.
#ifdef NDEBUG
	{
		MeshResultPool poolA;
		MeshResultPool poolB;
		MeshBuildResult *r = poolA.acquire();
		poolB.release(r);
		CHECK(poolA.activeCount() == 1, "wrong-pool release leaves owner intact");
		CHECK(poolA.freeCount() == 0, "wrong-pool release does not touch owner free list");
		CHECK(poolB.activeCount() == 0, "wrong-pool release does not adopt the block");
		CHECK(poolB.freeCount() == 0, "wrong-pool release does not pollute borrower");
		poolA.release(r);
	}
#endif

	// 7) Double release is refused (Release-only; Debug asserts first).
#ifdef NDEBUG
	{
		MeshResultPool pool;
		MeshBuildResult *r = pool.acquire();
		pool.release(r);
		pool.release(r);
		CHECK(pool.freeCount() == 1, "double release does not duplicate the free entry");
		CHECK(pool.activeCount() == 0, "double release does not corrupt active count");
	}
#endif

	// 8) Concurrency: across hammering acquire/build/release, no block is
	// ever handed to two holders at the same time, and everything ends in
	// the free list.
	{
		MeshResultPool pool;
		pool.reserve(4);
		constexpr int kThreads = 6;
		constexpr int kIters = 2000;
		std::mutex guard; // protects the invariants below, not the pool
		std::set<MeshBuildResult *> currentlyHeld;
		std::vector<std::thread> threads;
		for (int t = 0; t < kThreads; ++t)
		{
			threads.emplace_back([&] {
				for (int i = 0; i < kIters; ++i)
				{
					MeshBuildResult *r = pool.acquire();
					{
						std::lock_guard<std::mutex> lk(guard);
						CHECK(currentlyHeld.insert(r).second,
							  "pool never double-hands a block");
					}
					r->beginBuild(nullptr, uint64_t(i));
					r->opaqueIndices.push_back(uint32_t(i));
					// Mark the block as no longer held BEFORE handing it
					// back: a later insert of the same pointer by another
					// thread is then legitimate reuse, while a block the
					// pool hands out twice while still held fails the
					// insert above.
					{
						std::lock_guard<std::mutex> lk(guard);
						currentlyHeld.erase(r);
					}
					pool.release(r);
				}
			});
		}
		for (auto &th : threads)
			th.join();
		CHECK(currentlyHeld.empty(), "no block is held after the stress run");
		CHECK(pool.activeCount() == 0, "active count returns to zero");
		CHECK(pool.freeCount() == pool.capacity(), "every block is back in the free list");
	}

	// 9) Stale-block reuse: a released block with stale identity is fully
	// re-initialized by beginBuild before the next build exposes it.
	{
		MeshResultPool pool;
		MeshBuildResult *r = pool.acquire();
		fillJunk(*r);
		pool.release(r);
		MeshBuildResult *reused = pool.acquire();
		CHECK(reused == r, "stale block is handed back for reuse");
		reused->beginBuild(nullptr, 9);
		bool clean = reused->opaqueVertices.empty() && reused->opaqueIndices.empty() &&
					 reused->waterVertices.empty() && reused->waterIndices.empty();
		CHECK(clean, "stale block contents cleared on next beginBuild");
		pool.release(reused);
	}

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: mesh result pool - reuse, retention, identity, "
				 "concurrency, telemetry\n";
	return 0;
}
