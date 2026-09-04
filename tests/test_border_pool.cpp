// BorderPool contract (issue #113 review): pointer-stable reusable compact
// border blocks whose content the CONSUMER initializes. Covers
// acquire/release/reuse, reserve and high-water behavior, concurrency,
// wrong-pool/double release refusal (Release-safe), growth events and stale
// block reuse.
#include <Chunk/ChunkBorders.hpp>
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

int main()
{
	// 1) Single acquire/release and reuse of the same block.
	{
		BorderPool pool;
		ChunkNeighborBorders *first = pool.acquire();
		CHECK(first != nullptr, "acquire returns a block");
		pool.release(first);
		CHECK(pool.capacity() == 1, "one block retained after release");
		CHECK(pool.activeCount() == 0, "no active blocks after release");
		CHECK(pool.freeCount() == 1, "released block is in the free list");
		ChunkNeighborBorders *second = pool.acquire();
		CHECK(second == first, "reuse hands back the same block");
		CHECK(pool.capacity() == 1, "reuse does not grow the pool");
		pool.release(second);
	}

	// 2) Reserve pre-allocates; burst then steady-state reuse shows the
	// high-water behavior with no growth after the burst.
	{
		BorderPool pool;
		pool.reserve(10);
		CHECK(pool.capacity() == 10, "reserve pre-allocates capacity");
		std::vector<ChunkNeighborBorders *> held;
		for (int i = 0; i < 32; ++i)
			held.push_back(pool.acquire());
		CHECK(pool.capacity() == 32, "burst grows the pool to the working set");
		for (auto *b : held)
			pool.release(b);
		for (int i = 0; i < 1000; ++i)
		{
			ChunkNeighborBorders *b = pool.acquire();
			pool.release(b);
		}
		CHECK(pool.capacity() == 32, "steady-state reuse does not grow the pool");
		CHECK(pool.activeCount() == 0, "steady-state reuse ends with 0 active");
		CHECK(pool.freeCount() == 32, "all blocks back in the free list");
	}

	// 3) Wrong-pool release must be refused without corrupting either pool.
	// Release-only: in Debug the ownership assert fires, making the
	// programming error immediately visible.
#ifdef NDEBUG
	{
		BorderPool poolA;
		BorderPool poolB;
		ChunkNeighborBorders *s = poolA.acquire();
		poolB.release(s);
		CHECK(poolA.activeCount() == 1, "wrong-pool release leaves owner intact");
		CHECK(poolA.freeCount() == 0, "wrong-pool release does not touch owner free list");
		CHECK(poolB.activeCount() == 0, "wrong-pool release does not adopt the block");
		CHECK(poolB.freeCount() == 0, "wrong-pool release does not pollute borrower");
		poolA.release(s);
		CHECK(poolA.freeCount() == 1, "owner release still works after refused release");
	}

	// 4) Double release must be refused without duplicating the free entry.
	{
		BorderPool pool;
		ChunkNeighborBorders *s = pool.acquire();
		pool.release(s);
		pool.release(s);
		CHECK(pool.freeCount() == 1, "double release does not duplicate the free entry");
		CHECK(pool.activeCount() == 0, "double release does not drive active negative");
		ChunkNeighborBorders *t = pool.acquire();
		CHECK(t == s, "pool still hands out the block exactly once");
		pool.release(t);
	}
#endif

	// 5) Concurrent acquire/release: 4 threads x 2000 ops; a mutex-protected
	// active set verifies no block is handed out twice at the same time.
	{
		BorderPool pool;
		constexpr int kThreads = 4;
		constexpr int kOpsPerThread = 2000;
		std::mutex activeMutex;
		std::set<ChunkNeighborBorders *> active;
		std::vector<std::thread> workers;
		for (int t = 0; t < kThreads; ++t)
		{
			workers.emplace_back([&pool, &active, &activeMutex]() {
				for (int i = 0; i < kOpsPerThread; ++i)
				{
					ChunkNeighborBorders *b = pool.acquire();
					{
						std::lock_guard<std::mutex> lock(activeMutex);
						CHECK(active.insert(b).second, "block handed out while still active");
					}
					b->mutableAt(-1, 7, 9) = 1;
					{
						std::lock_guard<std::mutex> lock(activeMutex);
						active.erase(b);
					}
					pool.release(b);
				}
			});
		}
		for (auto &w : workers)
			w.join();
		CHECK(pool.activeCount() == 0, "concurrent stress ends with 0 active");
		CHECK(pool.capacity() == pool.freeCount(), "concurrent stress keeps free list consistent");
	}

	// 6) Growth events: allocation beyond the free list is one grow event,
	// free-list reuse is not, and beginCapture() clears the interval.
	{
		auto &registry = telemetry::registry();
		registry.beginCapture();
		BorderPool pool;
		ChunkNeighborBorders *a = pool.acquire();
		ChunkNeighborBorders *b = pool.acquire();
		const auto afterGrowth = registry.snapshot();
		pool.release(a);
		ChunkNeighborBorders *reuse = pool.acquire();
		const auto afterReuse = registry.snapshot();
		CHECK(afterGrowth.events[telemetry::BorderPoolGrow] == 2,
			  "each allocation beyond the free list is one grow event");
		CHECK(afterReuse.events[telemetry::BorderPoolGrow] == 2,
			  "free-list reuse is not counted as growth");
		registry.beginCapture();
		CHECK(registry.snapshot().events[telemetry::BorderPoolGrow] == 0,
			  "beginCapture clears border grow events");
		pool.release(b);
		ChunkNeighborBorders *secondReuse = pool.acquire();
		CHECK(registry.snapshot().events[telemetry::BorderPoolGrow] == 0,
			  "reuse after the capture boundary is still not growth");
		pool.release(reuse);
		pool.release(secondReuse);
	}

	// 7) Stale block reuse: acquired blocks hold stale bytes - the pool does
	// not clear them - and the consumer must initialize before sampling.
	{
		BorderPool pool;
		ChunkNeighborBorders *b = pool.acquire();
		b->resetToAir();
		b->mutableAt(-1, 3, 4) = static_cast<uint8_t>(15);
		pool.release(b);

		ChunkNeighborBorders *stale = pool.acquire();
		CHECK(stale == b, "reuse returns the same block");
		// Stale content is observable until the consumer initializes.
		const size_t staleIdx = 3 * CHUNK_SIZE + 4;
		const bool staleIsVisible = stale->west[staleIdx] == 15;
		CHECK(staleIsVisible, "acquired block keeps stale bytes (pool does not clear)");
		// Consumer-side initialization makes every cell AIR.
		stale->resetToAir();
		CHECK(stale->at(-1, 3, 4) == static_cast<uint8_t>(AIR),
			  "consumer initialization yields AIR");
		pool.release(stale);
	}

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: border pool - acquire/release/reuse, reserve high-water, wrong-pool and "
			  << "double release refusal, concurrency, growth events, stale reuse\n";
	return 0;
}
