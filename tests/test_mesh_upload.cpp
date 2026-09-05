// Mesh upload lifecycle against a real (headless) Vulkan device
// (issue #104 acceptance criteria, review #114 items 25-28, reworked for the
// sectioned slot uploads of issue #107):
//   - a staging-ring-full frame defers the upload WITHOUT losing, copying
//     or rebuilding the completed build result;
//   - the retry succeeds from the very same result, in place (section slots
//     re-stage without swapping the chunk-level buffers);
//   - a partial staging failure (all opaque sections staged, the first
//     water section does not fit) keeps the CPU result attached and the old
//     GPU mesh untouched until the successful retry.
// Skips quietly when no Vulkan loader/device is available (CI containers).
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkMeshResult.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Vulkan/StagingRing.hpp>
#include <Vulkan/GpuResourceRetire.hpp>
#include <Vulkan/VkAllocator.hpp>
#include <volk.h>

#include <algorithm>
#include <cstring>
#include <iostream>

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

// Friend probe declared in Chunk.hpp.
struct ChunkStateProbe
{
	static MeshBuildResult *pending(const Chunk &c) { return c.m_pendingResult; }
	static uint64_t meshGeneration(const Chunk &c) { return c.m_meshGeneration; }
	static AllocatedBuffer opaqueBuffer(const Chunk &c) { return c.vertexBuffer; }
	static AllocatedBuffer waterBuffer(const Chunk &c) { return c.waterVertexBuffer; }
};

namespace
{

constexpr VkDeviceSize align256(VkDeviceSize v)
{
	return (v + StagingRing::kAlignment - 1) / StagingRing::kAlignment * StagingRing::kAlignment;
}

// Staging bytes the sectioned upload will request for one stream
// (per masked section with content: one vertex alloc + one index alloc).
template <typename PayloadSel>
VkDeviceSize streamStagingBytes(const MeshBuildResult &r, PayloadSel payloadSel)
{
	VkDeviceSize total = 0;
	for (int s = 0; s < kChunkSectionCount; ++s)
	{
		if (((r.sectionsBuilt >> s) & 1u) == 0)
			continue;
		const auto [verts, idxs] = payloadSel(r, s);
		const VkDeviceSize vBytes = verts->size() * sizeof(Vertex);
		const VkDeviceSize iBytes = idxs->size() * sizeof(uint32_t);
		if (vBytes == 0 && iBytes == 0)
			continue;
		total += align256(vBytes) + align256(iBytes);
	}
	return total;
}

VkDeviceSize opaqueStagingBytes(const MeshBuildResult &r)
{
	return streamStagingBytes(r, [](const MeshBuildResult &res, int s)
	{
		return std::make_pair(&res.sections[static_cast<size_t>(s)].opaqueVertices,
							  &res.sections[static_cast<size_t>(s)].opaqueIndices);
	});
}

size_t totalOpaqueIndices(const MeshBuildResult &r)
{
	size_t n = 0;
	for (const SectionMeshPayload &p : r.sections)
		n += p.opaqueIndices.size();
	return n;
}

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
	uint32_t queueFamily{0};
	VkQueue queue{VK_NULL_HANDLE};
	VkCommandPool pool{VK_NULL_HANDLE};
	VkCommandBuffer cmd{VK_NULL_HANDLE};
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
		bool found = false;
		for (uint32_t i = 0; i < familyCount; ++i)
		{
			if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				queueFamily = i;
				found = true;
				break;
			}
		}
		if (!found)
			return false;

		float priority = 1.0f;
		VkDeviceQueueCreateInfo qci{};
		qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		qci.queueFamilyIndex = queueFamily;
		qci.queueCount = 1;
		qci.pQueuePriorities = &priority;
		VkDeviceCreateInfo dci{};
		dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS)
			return false;
		volkLoadDevice(device);
		vkGetDeviceQueue(device, queueFamily, 0, &queue);

		try
		{
			allocator.init(instance, physical, device, VK_API_VERSION_1_2);
		}
		catch (...)
		{
			return false;
		}

		VkCommandPoolCreateInfo pci{};
		pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pci.queueFamilyIndex = queueFamily;
		if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS)
			return false;
		VkCommandBufferAllocateInfo cai{};
		cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cai.commandPool = pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(device, &cai, &cmd) != VK_SUCCESS)
			return false;
		VkCommandBufferBeginInfo bi{};
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		return vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
	}

	void shutdown()
	{
		if (device != VK_NULL_HANDLE)
			vkDeviceWaitIdle(device);
		allocator.shutdown();
		if (pool != VK_NULL_HANDLE)
			vkDestroyCommandPool(device, pool, nullptr);
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
		std::cout << "SKIP: mesh upload lifecycle (no Vulkan device available)\n";
		return 0;
	}

	// Small helper state for staging/retire.
	StagingRing staging;
	staging.init(vk.allocator.handle(), 1); // default 48 MiB - roomy ring
	staging.beginFrame(0);
	GpuResourceRetire retire;
	retire.init(vk.allocator.handle(), 2);
	retire.beginFrame(0);

	{
		ChunkPool chunkPool(16);
		TerrainGenerator gen(42);
		ChunkManager manager(&gen, nullptr, &chunkPool);
		Camera cam(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(64);
		Chunk *chunk = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "chunk registered by the load path");

		if (chunk)
		{
			CHECK(manager.prepareAndGenerateChunk(chunk, gen), "prepare+generate");
			CHECK(chunk->generateMesh(), "first mesh publishes");
			MeshBuildResult *first = ChunkStateProbe::pending(*chunk);
			CHECK(first != nullptr, "pending result attached");
			CHECK(first->sectionsBuilt == kAllSectionMask,
				  "full build stamps every section");

			// 25/26 baseline: a roomy ring uploads the first mesh.
			const AllocatedBuffer firstOpaque = ChunkStateProbe::opaqueBuffer(*chunk);
			CHECK(firstOpaque.buffer == VK_NULL_HANDLE, "no GPU mesh before upload");
			const size_t firstIndexCount = totalOpaqueIndices(*first);
			CHECK(firstIndexCount > 0, "full build emitted geometry");
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire),
				  "roomy staging uploads the mesh");
			const AllocatedBuffer uploadedOpaque = ChunkStateProbe::opaqueBuffer(*chunk);
			CHECK(uploadedOpaque.buffer != VK_NULL_HANDLE, "GPU mesh created");
			CHECK(!chunk->hasPendingMeshResult(), "result consumed by upload");
			CHECK(!chunk->needsGPUUpload(), "meshNeedsUpdate cleared by upload");
			// The drawn count covers the packed slot region: live indices
			// plus zeroed slot slack (issue #107), so it is >= the payload.
			CHECK(chunk->getOpaqueIndexCount() >= firstIndexCount,
				  "drawn index count covers the uploaded payload");

			// 25/28: a second mesh, then a staging slice drained to capacity
			// (StagingRing enforces a >=4 MiB per-frame minimum, so "full"
			// is simulated by consuming the slice with dummy allocations).
			CHECK(chunk->generateMesh(), "second mesh publishes");
			MeshBuildResult *second = ChunkStateProbe::pending(*chunk);
			CHECK(second != nullptr, "fresh result attached");
			staging.beginFrame(0);

			const AllocatedBuffer oldOpaque = ChunkStateProbe::opaqueBuffer(*chunk);
			const AllocatedBuffer oldWater = ChunkStateProbe::waterBuffer(*chunk);
			const uint64_t oldGen = ChunkStateProbe::meshGeneration(*chunk);
			const uint64_t oldRev = chunk->meshRevision();
			const size_t oldActive = chunk->getMeshResultPool()->activeCount();
			const uint32_t oldOpaqueCount = chunk->getOpaqueIndexCount();

			{
				// Drain the entire slice so ANY payload allocation fails.
				VkDeviceSize off = 0;
				void *sink = nullptr;
				const VkDeviceSize cap = staging.sliceCapacity();
				CHECK(staging.alloc(cap, off, sink), "drain allocation fills the slice");
			}

			CHECK(!chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire),
				  "staging-full defers the upload");
			CHECK(ChunkStateProbe::pending(*chunk) == second,
				  "the exact same result stays attached (no rebuild, no copy)");
			CHECK(chunk->meshGeneration() == oldGen && chunk->meshRevision() == oldRev,
				  "identity untouched by the deferred attempt");
			CHECK(chunk->getMeshResultPool()->activeCount() == oldActive,
				  "result stays active while deferred");
			CHECK(chunk->needsGPUUpload(), "meshNeedsUpdate stays armed while deferred");
			CHECK(ChunkStateProbe::opaqueBuffer(*chunk).buffer == oldOpaque.buffer &&
					  ChunkStateProbe::waterBuffer(*chunk).buffer == oldWater.buffer,
				  "old GPU mesh is not swapped on a deferred attempt");
			CHECK(chunk->getOpaqueIndexCount() == oldOpaqueCount,
				  "draw counts unchanged by the deferred attempt");

			// 26: retry with a fresh slice succeeds from the same result
			// without any rebuild. The payload may differ from the first
			// build (upload released the neighbor borders), so outgrown
			// slots append and may grow the buffer - the invariant is that
			// the retry publishes the deferred payload's counts.
			staging.beginFrame(0);
			const size_t secondIndexCount = totalOpaqueIndices(*second);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire),
				  "retry upload succeeds without a rebuild");
			CHECK(!chunk->hasPendingMeshResult(), "retry consumes the same result");
			CHECK(chunk->getOpaqueIndexCount() >= secondIndexCount,
				  "retry uploads the deferred payload");

			// 27: partial staging failure - all opaque section allocs fit
			// and the FIRST water section alloc does not. Craft a known
			// water payload in section 0 so the boundary is deterministic.
			CHECK(chunk->generateMesh(), "third mesh publishes");
			MeshBuildResult *third = ChunkStateProbe::pending(*chunk);
			if (third && third->sections[0].waterVertices.empty())
			{
				Vertex v{};
				third->sections[0].waterVertices.assign(64, v);
				third->sections[0].waterIndices.assign(96, 0u);
				third->sectionsBuilt |= 1u; // section 0 now carries water
				chunk->getMeshResultPool()->finishBuild(third); // delta accounting
			}
			const AllocatedBuffer prevOpaque = ChunkStateProbe::opaqueBuffer(*chunk);
			const VkDeviceSize opaqueNeeds = opaqueStagingBytes(*third);
			staging.beginFrame(0);
			{
				// Leave exactly room for the opaque stream's allocations;
				// the first water allocation cannot fit in what remains.
				VkDeviceSize off = 0;
				void *sink = nullptr;
				const VkDeviceSize drain = staging.sliceCapacity() - opaqueNeeds;
				CHECK(staging.alloc(drain, off, sink),
					  "drain leaves exactly the opaque sections of room");
			}

			CHECK(!chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire),
				  "partial staging failure defers the upload");
			CHECK(ChunkStateProbe::pending(*chunk) == third,
				  "CPU result survives the partial failure");
			CHECK(ChunkStateProbe::opaqueBuffer(*chunk).buffer == prevOpaque.buffer,
				  "old GPU mesh survives the partial failure");
			CHECK(chunk->needsGPUUpload(), "retry still pending after partial failure");

			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire),
				  "final retry succeeds");
			CHECK(chunk->hasWaterMesh(), "water mesh uploaded after the retry");

			vkDeviceWaitIdle(vk.device);
			retire.flush();
		}
	}

	staging.shutdown();
	retire.shutdown();
	vk.shutdown();

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: mesh upload lifecycle - defer, retry, partial rollback\n";
	return 0;
}
