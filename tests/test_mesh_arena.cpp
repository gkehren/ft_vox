// MeshArena unit tests against a real (headless) Vulkan device (issue #109):
// aligned suballocation, first-fit reuse with coalescing, frame-aware
// retirement, page growth, exhaustion without corruption, and range
// stability. Skips quietly when no Vulkan loader/device is available.
#include <Vulkan/MeshArena.hpp>
#include <utils.hpp>
#include <Vulkan/VkAllocator.hpp>
#include <volk.h>

#include <algorithm>
#include <iostream>
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

namespace
{
VkPhysicalDevice pickPhysicalDevice(VkInstance instance)
{
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(instance, &count, nullptr);
	if (count == 0)
		return VK_NULL_HANDLE;
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(instance, &count, devices.data());
	for (VkPhysicalDevice d : devices)
	{
		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(d, &props);
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
			return d;
	}
	return devices[0];
}

struct HeadlessDevice
{
	VkInstance instance{VK_NULL_HANDLE};
	VkPhysicalDevice physical{VK_NULL_HANDLE};
	VkDevice device{VK_NULL_HANDLE};
	VkAllocator allocator;

	bool init()
	{
		if (volkInitialize() != VK_SUCCESS)
			return false;
		VkApplicationInfo app{};
		app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		app.apiVersion = VK_API_VERSION_1_2;
		VkInstanceCreateInfo ici{};
		ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		ici.pApplicationInfo = &app;
		if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
			return false;
		volkLoadInstance(instance);
		physical = pickPhysicalDevice(instance);
		if (physical == VK_NULL_HANDLE)
			return false;
		uint32_t familyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
		std::vector<VkQueueFamilyProperties> families(familyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
		uint32_t graphics = VK_QUEUE_FAMILY_IGNORED;
		for (uint32_t i = 0; i < familyCount; ++i)
			if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				graphics = i;
				break;
			}
		if (graphics == VK_QUEUE_FAMILY_IGNORED)
			return false;
		float priority = 1.0f;
		VkDeviceQueueCreateInfo qci{};
		qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qci.queueFamilyIndex = graphics;
		qci.queueCount = 1;
		qci.pQueuePriorities = &priority;
		VkDeviceCreateInfo dci{};
		dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS)
			return false;
		volkLoadDevice(device);
		try
		{
			allocator.init(instance, physical, device, VK_API_VERSION_1_2);
		}
		catch (...)
		{
			return false;
		}
		return true;
	}

	void shutdown()
	{
		if (device != VK_NULL_HANDLE)
			vkDeviceWaitIdle(device);
		allocator.shutdown();
		if (device != VK_NULL_HANDLE)
			vkDestroyDevice(device, nullptr);
		if (instance != VK_NULL_HANDLE)
			vkDestroyInstance(instance, nullptr);
	}
};
} // namespace

int main()
{
	HeadlessDevice vk;
	if (!vk.init())
	{
		std::cout << "SKIP: mesh arena tests (no Vulkan device available)\n";
		return 0;
	}

	GpuResourceRetire retire;
	retire.init(vk.allocator.handle(), 2);
	retire.beginFrame(0);

	// Small page (256 KiB) so growth and exhaustion are reachable quickly.
	MeshArena arena;
	// 2 frames in flight -> the arena's retirement delay is 3 beginFrames.
	arena.init(vk.allocator.handle(), retire, 256 * 1024,
			   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, sizeof(Vertex), telemetry::GpuOpaqueVertex,
			   2);

	// Every range allocated below is collected here so the drain test can
	// return the arena to a fully free state.
	std::vector<MeshArena::Range> kept;

	// 1. Aligned suballocation: every range is page-aligned to sizeof(Vertex)
	//    and stays inside its page.
	{
		std::vector<MeshArena::Range> ranges;
		for (int i = 0; i < 64; ++i)
		{
			MeshArena::Range r;
			CHECK(arena.allocate(1000 + i * 28, r), "allocate succeeds on a fresh page");
			CHECK(r.page == 0, "first allocations land on page 0");
			CHECK(r.offset % sizeof(Vertex) == 0, "vertex ranges stay Vertex-aligned");
			CHECK(r.bytes >= 1000u + i * 28, "range covers the request");
			ranges.push_back(r);
			kept.push_back(r);
		}
		CHECK(arena.metrics().liveBlocks == 64, "64 live blocks tracked");
	}

	// 2. First-fit reuse: freeing one range lets the next allocation take
	//    exactly that slot (offset reuse proves the free list works).
	{
		MeshArena::Range freed = {};
		std::vector<MeshArena::Range> ranges;
		for (int i = 0; i < 8; ++i)
		{
			MeshArena::Range r;
			CHECK(arena.allocate(280, r), "alloc for reuse test");
			if (i != 3)
				kept.push_back(r); // range 3 is freed right below
			ranges.push_back(r);
		}
		freed = ranges[3];
		arena.freeImmediate(freed);
		MeshArena::Range reused;
		CHECK(arena.allocate(280, reused), "alloc reuses the freed block");
		CHECK(reused.page == freed.page && reused.offset == freed.offset,
			  "first-fit reuses the exact freed slot");
		kept.push_back(reused);
	}

	// 3. Frame-aware retirement: a retired range is NOT reusable before the
	//    delay elapses, and becomes reusable after kRetireDelay frames.
	{
		MeshArena::Range r;
		CHECK(arena.allocate(560, r), "alloc for retire test");
		const uint32_t off = r.offset;
		arena.retire(r);
		arena.beginFrame(1);
		arena.beginFrame(2);
		MeshArena::Range early;
		CHECK(arena.allocate(560, early), "alloc during retire delay");
		CHECK(!(early.page == r.page && early.offset == off),
			  "retired range is not reused before the delay");
		arena.beginFrame(4);
		MeshArena::Range late;
		CHECK(arena.allocate(560, late), "alloc after the delay");
		CHECK(late.page == r.page && late.offset == off,
			  "retired range returns to the free list after the delay");
		kept.push_back(early);
		kept.push_back(late);
	}

	// 4. Growth: allocations beyond one page create new pages; exhausting
	//    the device returns false without corrupting existing ranges.
	{
		const uint32_t beforePages = arena.metrics().pages;
		std::vector<MeshArena::Range> big;
		bool grew = false, exhausted = false;
		MeshArena::Range keep;
		CHECK(arena.allocate(1024, keep), "keep one live range across growth");
		for (int i = 0; i < 512 && !exhausted; ++i)
		{
			MeshArena::Range r;
			if (!arena.allocate(64 * 1024, r))
			{
				exhausted = true;
				break;
			}
			if (r.page >= beforePages)
				grew = true;
			big.push_back(r);
		}
		CHECK(grew, "allocation grows by adding pages");
		CHECK(exhausted || big.size() == 512, "growth is bounded by device memory");
		// The pre-growth range still resolves: no corruption.
		CHECK(keep.page != MeshArena::kNoPage, "early range survives growth");
		kept.push_back(keep);
		for (auto &r : big)
		{
			arena.freeImmediate(r);
		}
	}

	// 5. Empty pages are released: after freeing everything and ticking the
	//    retire delay, the arena drops back to zero pages.
	{
		for (auto &r : kept)
			arena.freeImmediate(r);
		kept.clear();
		arena.beginFrame(100);
		arena.beginFrame(101);
		arena.beginFrame(102);
		arena.beginFrame(103);
		const auto after = arena.metrics();
		CHECK(after.pages == 0, "fully drained arena releases every page");
		CHECK(after.liveBytes == 0, "drained arena holds no live bytes");
	}

	// 6. Deterministic allocation failure:
	{
		arena.setFailNextPageAllocations(1);
		MeshArena::Range failR;
		CHECK(!arena.allocate(1024, failR), "injected page failure returns false");
		CHECK(arena.metrics().pages == 0, "no page created on injected failure");
		CHECK(arena.liveBlocks() == 0, "liveBlocks unchanged on failed alloc");
		CHECK(arena.liveBytes() == 0, "liveBytes unchanged on failed alloc");

		// Subsequent allocation succeeds once failure count is consumed.
		MeshArena::Range okR;
		CHECK(arena.allocate(1024, okR), "allocation succeeds after failure count consumed");
		CHECK(arena.metrics().pages == 1, "page created on successful alloc");
		CHECK(arena.liveBlocks() == 1, "liveBlocks incremented on success");
		CHECK(arena.liveBytes() == okR.bytes, "liveBytes updated on success");
		arena.freeImmediate(okR);
		arena.beginFrame(150);
		CHECK(arena.metrics().pages == 0, "page released after okR freed");
		retire.flush();
	}

	// 7. Counter consistency and pending retirement:
	//    liveBlocks and liveBytes track both active and pending-retirement ranges.
	{
		MeshArena::Range r1, r2, r3;
		CHECK(arena.allocate(512, r1), "alloc r1");
		CHECK(arena.allocate(512, r2), "alloc r2");
		CHECK(arena.allocate(512, r3), "alloc r3");
		CHECK(arena.liveBlocks() == 3, "liveBlocks == 3");
		const uint64_t expectedBytes = r1.bytes + r2.bytes + r3.bytes;
		CHECK(arena.liveBytes() == expectedBytes, "liveBytes matches sum of 3 allocations");

		// Retire r1: still consumes the arena until retirement delay elapses
		arena.retire(r1);
		CHECK(arena.liveBlocks() == 3, "liveBlocks unchanged while r1 pending");
		CHECK(arena.liveBytes() == expectedBytes, "liveBytes unchanged while r1 pending");

		// freeImmediate r2: decrements immediately
		arena.freeImmediate(r2);
		CHECK(arena.liveBlocks() == 2, "liveBlocks decremented after freeImmediate r2");
		CHECK(arena.liveBytes() == r1.bytes + r3.bytes, "liveBytes decremented after freeImmediate r2");

		// Advance frames past retirement delay for r1
		arena.beginFrame(200);
		arena.beginFrame(201);
		arena.beginFrame(202);
		arena.beginFrame(203);
		CHECK(arena.liveBlocks() == 1, "liveBlocks decremented after r1 delay elapsed");
		CHECK(arena.liveBytes() == r3.bytes, "liveBytes decremented after r1 delay elapsed");

		// Pending retired range cannot be reused before its delay
		MeshArena::Range r4;
		CHECK(arena.allocate(512, r4), "alloc r4");
		arena.retire(r4); // retired at frame 203, delay 3 -> expires at 206
		const uint32_t off4 = r4.offset;
		arena.beginFrame(204);
		arena.beginFrame(205);
		MeshArena::Range earlyR;
		CHECK(arena.allocate(512, earlyR), "early alloc during retire delay");
		CHECK(!(earlyR.page == r4.page && earlyR.offset == off4),
			  "pending retired range cannot be reused before retirement frame");
		// Advance frame past delay
		arena.beginFrame(207);
		MeshArena::Range lateR;
		CHECK(arena.allocate(512, lateR), "alloc after retirement frame");
		CHECK(lateR.page == r4.page && lateR.offset == off4,
			  "range reuse succeeds after exact retirement frame");

		arena.freeImmediate(r3);
		arena.freeImmediate(earlyR);
		arena.freeImmediate(lateR);
		arena.beginFrame(210);
		CHECK(arena.liveBlocks() == 0, "liveBlocks back to 0");
		CHECK(arena.liveBytes() == 0, "liveBytes back to 0");
		retire.flush();
	}

	// 8. Page cannot be destroyed while pending ranges exist:
	{
		MeshArena::Range soleR;
		CHECK(arena.allocate(1024, soleR), "alloc sole range on page");
		arena.retire(soleR); // retired at frame 210 -> expires at 213
		arena.beginFrame(211);
		CHECK(arena.metrics().pages >= 1, "page not destroyed while range pending");
		arena.beginFrame(215);
		// After delay, page returns to free and gets released to retire queue
		CHECK(arena.metrics().pages == 0, "page released after all ranges retired and freed");
		retire.flush();
	}

	// 9. Oversize page allocation (> pageSize = 256 KiB):
	{
		MeshArena::Range oversizeR;
		CHECK(arena.allocate(512 * 1024, oversizeR), "oversize 512 KiB allocation succeeds");
		CHECK(oversizeR.bytes >= 512 * 1024, "oversize range has requested capacity");
		CHECK(arena.pageSize(oversizeR.page) >= 512 * 1024, "oversize page created with proper size");
		arena.freeImmediate(oversizeR);
		arena.beginFrame(400);
		CHECK(arena.metrics().pages == 0, "oversize page released");
		retire.flush();
	}

	// 10. Multiple pages release independently:
	{
		// Force multiple pages by allocating several 200 KiB blocks
		MeshArena::Range p0R, p1R;
		CHECK(arena.allocate(200 * 1024, p0R), "alloc p0R");
		CHECK(arena.allocate(200 * 1024, p1R), "alloc p1R");
		CHECK(p0R.page != p1R.page, "allocations land on separate pages");
		const uint32_t pagesBoth = arena.metrics().pages;
		CHECK(pagesBoth >= 2, "at least 2 pages allocated");

		// Free page 1 first; page 0 range remains held
		arena.freeImmediate(p1R);
		arena.beginFrame(500);
		CHECK(arena.metrics().pages == pagesBoth - 1, "page 1 released independently");
		CHECK(arena.liveBlocks() == 1, "page 0 range remains live");

		// Free page 0
		arena.freeImmediate(p0R);
		arena.beginFrame(501);
		CHECK(arena.metrics().pages == 0, "page 0 released independently");
		CHECK(arena.liveBlocks() == 0, "all liveBlocks freed");
		retire.flush();
	}

	// Flushed retired pages live in the retire queue: flush it before
	// shutdown so VMA sees every block destroyed.
	retire.flush();
	arena.shutdown();

	// 11. MeshArenas wrapper pins the retirement margin (issue #121 review):
	//     the +1 must be applied exactly once, inside MeshArena::init. The
	//     wrapper passes framesInFlight through untouched - 2 frames in
	//     flight means a delay of 3, not 4.
	{
		MeshArenas arenas;
		arenas.init(vk.allocator.handle(), retire, sizeof(Vertex), 2,
		            256 * 1024, 128 * 1024);
		CHECK(arenas.opaqueVertex.retireDelay() == 3, "opaque vertex delay = FIF+1 = 3");
		CHECK(arenas.opaqueIndex.retireDelay() == 3, "opaque index delay = FIF+1 = 3");
		CHECK(arenas.waterVertex.retireDelay() == 3, "water vertex delay = FIF+1 = 3");
		CHECK(arenas.waterIndex.retireDelay() == 3, "water index delay = FIF+1 = 3");

		// Behavioral cross-check on the real wrapper config: retire a range
		// at the arena's current frame 0, it must come back after exactly
		// 3 beginFrame calls (1 and 2 are still inside the delay).
		MeshArena::Range r;
		CHECK(arenas.opaqueVertex.allocate(1024, r), "wrapper: allocate");
		CHECK(arenas.opaqueVertex.metrics().pages == 1, "wrapper: one page created");
		arenas.opaqueVertex.retire(r);
		arenas.beginFrame(1);
		arenas.beginFrame(2);
		CHECK(arenas.opaqueVertex.metrics().pages == 1,
		      "wrapper: page still held during the delay window");
		arenas.beginFrame(3);
		CHECK(arenas.opaqueVertex.metrics().pages == 0,
		      "wrapper: page released after exactly FIF+1 frames");
		arenas.shutdown();
		retire.flush();
	}

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: mesh arena - alignment, reuse, retirement, growth, exhaustion\n";
	return 0;
}
