// VoxelPool contract (issue #112 review): pointer-stable reusable voxel
// backing with strict ownership. Covers acquire/release/reuse, reserve and
// high-water behavior, concurrency, foreign release and double release
// refusal (Release-safe), and pool growth events.
#include <Chunk/VoxelPool.hpp>
#include <Engine/WorkloadTelemetry.hpp>

#include <algorithm>
#include <atomic>
#include <iostream>
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
		VoxelPool pool;
		VoxelStorage *first = pool.acquire();
		CHECK(first != nullptr, "acquire returns a block");
		pool.release(first);
		CHECK(pool.capacity() == 1, "one block retained after release");
		CHECK(pool.activeCount() == 0, "no active blocks after release");
		CHECK(pool.freeCount() == 1, "released block is in the free list");
		VoxelStorage *second = pool.acquire();
		CHECK(second == first, "reuse hands back the same block");
		CHECK(pool.capacity() == 1, "reuse does not grow the pool");
		pool.release(second);
	}

	// 2) Reserve pre-allocates and the pool behaves as a high-water cache:
	// capacity never shrinks and steady-state reuse does not grow it.
	{
		VoxelPool pool;
		pool.reserve(10);
		CHECK(pool.capacity() == 10, "reserve pre-allocates capacity");
		CHECK(pool.freeCount() == 10, "reserved blocks are free");
		std::vector<VoxelStorage *> held;
		held.reserve(10);
		for (int i = 0; i < 10; ++i)
			held.push_back(pool.acquire());
		CHECK(pool.freeCount() == 0, "all reserved blocks in use");
		CHECK(pool.capacity() == 10, "acquiring reserved blocks does not grow the pool");
		for (VoxelStorage *s : held)
			pool.release(s);
		CHECK(pool.capacity() == 10 && pool.freeCount() == 10,
			  "high-water capacity retained after release");
		// Reuse order hands back recently released blocks without growth.
		VoxelStorage *again = pool.acquire();
		CHECK(std::find(held.begin(), held.end(), again) != held.end(),
			  "reacquired block comes from the free list");
		pool.release(again);
	}

	// 3) Foreign release must be refused without corrupting either pool.
	{
		VoxelPool poolA;
		VoxelPool poolB;
		VoxelStorage *s = poolA.acquire();
		poolB.release(s);
		CHECK(poolA.activeCount() == 1, "foreign release leaves owner active count intact");
		CHECK(poolA.freeCount() == 0, "foreign release does not touch owner free list");
		CHECK(poolB.activeCount() == 0, "foreign release does not adopt the block");
		CHECK(poolB.freeCount() == 0, "foreign release does not pollute borrower free list");
		poolA.release(s);
		CHECK(poolA.freeCount() == 1, "owner release still works after refused foreign release");
	}

	// 4) Double release must be refused without duplicating the free entry.
	{
		VoxelPool pool;
		VoxelStorage *s = pool.acquire();
		pool.release(s);
		pool.release(s);
		CHECK(pool.freeCount() == 1, "double release does not duplicate the free entry");
		CHECK(pool.activeCount() == 0, "double release does not drive active negative");
		VoxelStorage *t = pool.acquire();
		CHECK(t == s, "pool still hands out the block exactly once");
		pool.release(t);
	}

	// 5) Concurrent acquire/release stays balanced (used from the streaming
	// workers historically; now also from edit paths).
	{
		VoxelPool pool;
		constexpr int kThreads = 4;
		constexpr int kOpsPerThread = 2000;
		std::vector<std::thread> workers;
		for (int t = 0; t < kThreads; ++t)
		{
			workers.emplace_back([&pool]() {
				for (int i = 0; i < kOpsPerThread; ++i)
				{
					VoxelStorage *s = pool.acquire();
					s->voxels[0].type = 1;
					pool.release(s);
				}
			});
		}
		for (auto &w : workers)
			w.join();
		CHECK(pool.activeCount() == 0, "concurrent stress ends with 0 active");
		CHECK(pool.capacity() == pool.freeCount(), "concurrent stress keeps free list consistent");
	}

	// 6) Growth events: growth beyond the free list is observable, reuse is
	// not counted as growth.
	{
		auto &registry = telemetry::registry();
		registry.beginCapture();
		VoxelPool pool;
		VoxelStorage *a = pool.acquire();
		VoxelStorage *b = pool.acquire();
		const auto afterGrowth = registry.snapshot();
		pool.release(a);
		VoxelStorage *reuse = pool.acquire();
		const auto afterReuse = registry.snapshot();
		CHECK(afterGrowth.events[telemetry::VoxelPoolGrow] == 2,
			  "each allocation beyond the free list is one grow event");
		CHECK(afterReuse.events[telemetry::VoxelPoolGrow] == 2,
			  "free-list reuse is not counted as growth");
		pool.release(b);
		pool.release(reuse);
		(void)registry;
	}

	// 7) Pointer stability: retained blocks keep their address for the pool
	// lifetime (chunks hold raw VoxelStorage pointers).
	{
		VoxelPool pool;
		std::set<VoxelStorage *> seen;
		std::vector<VoxelStorage *> held;
		for (int i = 0; i < 8; ++i)
		{
			VoxelStorage *s = pool.acquire();
			CHECK(seen.insert(s).second, "no duplicate live block handed out");
			held.push_back(s);
		}
		for (VoxelStorage *s : held)
			pool.release(s);
	}

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: voxel pool - acquire/release/reuse, reserve high-water, foreign and "
			  << "double release refusal, concurrency, growth events\n";
	return 0;
}
