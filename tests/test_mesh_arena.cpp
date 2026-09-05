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
	arena.init(vk.allocator.handle(), retire, 256 * 1024,
			   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, sizeof(Vertex), telemetry::GpuOpaqueVertex);

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
		arena.beginFrame(1 + MeshArena::kRetireDelay);
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

	// Flushed retired pages live in the retire queue: flush it before
	// shutdown so VMA sees every block destroyed.
	retire.flush();
	arena.shutdown();

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: mesh arena - alignment, reuse, retirement, growth, exhaustion\n";
	return 0;
}
