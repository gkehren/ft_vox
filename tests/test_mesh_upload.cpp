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
	static AllocatedBuffer indexBufferOf(const Chunk &c) { return c.indexBuffer; }
	// Section slot layout + used extents (PR #117 review phases 25-27):
	// scalar getters only - SectionGpuSlot itself is private.
	static uint32_t slotVOff(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].vertexOffset; }
	static uint32_t slotVSz(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].vertexSlotBytes; }
	static uint32_t slotVUsed(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].vertexUsedBytes; }
	static uint32_t slotVBase(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].vertexBase; }
	static uint32_t slotIOff(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].indexOffset; }
	static uint32_t slotISz(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].indexSlotBytes; }
	static uint32_t slotIUsed(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].indexUsedBytes; }
	static uint32_t slotICount(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].indexCount; }
	static uint32_t slotWIOff(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].indexOffset; }
	static uint32_t slotWISz(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].indexSlotBytes; }
	static uint32_t slotWIUsed(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].indexUsedBytes; }
	static uint32_t slotWICount(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].indexCount; }
	static uint32_t usedV(const Chunk &c) { return c.m_vertexUsedBytes; }
	static uint32_t usedI(const Chunk &c) { return c.m_indexUsedBytes; }
	static uint32_t usedWV(const Chunk &c) { return c.m_waterVertexUsedBytes; }
	static uint32_t usedWI(const Chunk &c) { return c.m_waterIndexUsedBytes; }
	static uint16_t sectionNonAir(const Chunk &c, int s) { return c.m_sectionNonAir[static_cast<size_t>(s)]; }
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
		// Opt-in Khronos validation with synchronization validation (PR #117
		// review phase 28): set FT_VOX_TEST_VALIDATION=1 and point VK_LAYER_PATH
		// at the SDK's Bin directory.
		std::vector<const char *> layerNames;
		std::vector<VkValidationFeatureEnableEXT> featureEnables;
		VkValidationFeaturesEXT validationFeatures{};
		if (std::getenv("FT_VOX_TEST_VALIDATION") != nullptr)
		{
			uint32_t layerCount = 0;
			vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
			std::vector<VkLayerProperties> layerProps(layerCount);
			vkEnumerateInstanceLayerProperties(&layerCount, layerProps.data());
			for (const VkLayerProperties &lp : layerProps)
				if (std::strcmp(lp.layerName, "VK_LAYER_KHRONOS_validation") == 0)
				{
					layerNames.push_back("VK_LAYER_KHRONOS_validation");
					featureEnables.push_back(
					    VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
					featureEnables.push_back(
					    VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
					break;
				}
		}
		if (!layerNames.empty())
		{
			std::cout << "validation layer enabled (sync + best practices)\n";
			ici.enabledLayerCount = static_cast<uint32_t>(layerNames.size());
			ici.ppEnabledLayerNames = layerNames.data();
			validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
			validationFeatures.enabledValidationFeatureCount =
			    static_cast<uint32_t>(featureEnables.size());
			validationFeatures.pEnabledValidationFeatures = featureEnables.data();
			ici.pNext = &validationFeatures;
		}
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

	// Submit everything recorded so far, wait, and re-begin the buffer so
	// GPU buffer contents become observable (readback checks).
	bool flush()
	{
		if (vkEndCommandBuffer(cmd) != VK_SUCCESS)
			return false;
		VkSubmitInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
			return false;
		if (vkQueueWaitIdle(queue) != VK_SUCCESS)
			return false;
		// Reset before re-recording (never between end and submit: a reset
		// wipes the recorded commands and the submission would be empty).
		vkResetCommandBuffer(cmd, 0);
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
			// A full repack lays the sections out back-to-back exactly, so
			// the drawn count is the packed payload itself (PR #117 review).
			CHECK(chunk->getOpaqueIndexCount() == firstIndexCount,
				  "drawn index count equals the exact packed payload");

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

			// 25D: snapshot the whole section-slot state; a staging failure
			// must leave every slot, used extent and draw count untouched.
			uint32_t snap[16][8];
			for (int s = 0; s < 16; ++s)
			{
				snap[s][0] = ChunkStateProbe::slotVOff(*chunk, s);
				snap[s][1] = ChunkStateProbe::slotVSz(*chunk, s);
				snap[s][2] = ChunkStateProbe::slotVUsed(*chunk, s);
				snap[s][3] = ChunkStateProbe::slotVBase(*chunk, s);
				snap[s][4] = ChunkStateProbe::slotIOff(*chunk, s);
				snap[s][5] = ChunkStateProbe::slotISz(*chunk, s);
				snap[s][6] = ChunkStateProbe::slotIUsed(*chunk, s);
				snap[s][7] = ChunkStateProbe::slotICount(*chunk, s);
			}
			const uint32_t snapUsed[4] = {ChunkStateProbe::usedV(*chunk),
			                              ChunkStateProbe::usedI(*chunk),
			                              ChunkStateProbe::usedWV(*chunk),
			                              ChunkStateProbe::usedWI(*chunk)};
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
			{
				bool slotsIdentical = true;
				for (int s = 0; s < 16 && slotsIdentical; ++s)
					slotsIdentical =
					    ChunkStateProbe::slotVOff(*chunk, s) == snap[s][0] &&
					    ChunkStateProbe::slotVSz(*chunk, s) == snap[s][1] &&
					    ChunkStateProbe::slotVUsed(*chunk, s) == snap[s][2] &&
					    ChunkStateProbe::slotVBase(*chunk, s) == snap[s][3] &&
					    ChunkStateProbe::slotIOff(*chunk, s) == snap[s][4] &&
					    ChunkStateProbe::slotISz(*chunk, s) == snap[s][5] &&
					    ChunkStateProbe::slotIUsed(*chunk, s) == snap[s][6] &&
					    ChunkStateProbe::slotICount(*chunk, s) == snap[s][7];
				CHECK(slotsIdentical,
					  "water-failure attempt leaves every section slot untouched");
				CHECK(ChunkStateProbe::usedV(*chunk) == snapUsed[0] &&
				          ChunkStateProbe::usedI(*chunk) == snapUsed[1] &&
				          ChunkStateProbe::usedWV(*chunk) == snapUsed[2] &&
				          ChunkStateProbe::usedWI(*chunk) == snapUsed[3],
					  "water-failure attempt leaves the used extents untouched");
			}

			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire),
				  "final retry succeeds");
			CHECK(chunk->hasWaterMesh(), "water mesh uploaded after the retry");

			vkDeviceWaitIdle(vk.device);
			retire.flush();
		}
	}

	// PR #117 review phases 25-30: copy-on-write section upload scenarios,
	// with GPU content readback proving the composed byte image of both
	// streams (preserve copies, appended slots, zeroed slack/abandoned
	// ranges, triangle alignment).
	{
		ChunkPool chunkPool2(16);
		TerrainGenerator gen2(77);
		ChunkManager manager2(&gen2, nullptr, &chunkPool2);
		Camera cam2(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings2;
		manager2.updateStreaming(cam2, settings2);
		manager2.processChunkLoading(64);
		Chunk *chunk = manager2.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "cow: chunk registered");
		if (chunk)
		{
			CHECK(manager2.prepareAndGenerateChunk(chunk, gen2), "cow: prepare+generate");
			CHECK(chunk->generateMesh(), "cow: full mesh publishes");

			constexpr int kSections = 16;
			std::array<SectionMeshPayload, kSections> cpuView{};
			{
				MeshBuildResult *r = ChunkStateProbe::pending(*chunk);
				for (int s = 0; s < kSections; ++s)
					cpuView[static_cast<size_t>(s)] = r->sections[static_cast<size_t>(s)];
			}

			// GPU readback of [0, bytes) of a buffer (host-visible staging).
			// The host buffers stay alive until the end of the block: a
			// destroy/recreate would recycle the VkBuffer handle across
			// submissions and trip the sync layer's handle-based tracking.
			std::vector<AllocatedBuffer> readbackKeepAlive;
			auto readbackBuffer = [&](const AllocatedBuffer &src,
			                          VkDeviceSize bytes) -> std::vector<uint8_t>
			{
				std::vector<uint8_t> out(static_cast<size_t>(bytes), 0);
				if (bytes == 0)
					return out;
				AllocatedBuffer host =
				    createBuffer(vk.allocator.handle(), bytes,
				                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				                 VMA_MEMORY_USAGE_AUTO,
				                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
				                     VMA_ALLOCATION_CREATE_MAPPED_BIT);
				readbackKeepAlive.push_back(host);
				VkBufferCopy copy{};
				copy.size = bytes;
				vkCmdCopyBuffer(vk.cmd, src.buffer, host.buffer, 1, &copy);
				CHECK(vk.flush(), "readback submit");
				// The readback allocation may be non-coherent: invalidate the
				// mapped range so the CPU observes the GPU's writes.
				vmaInvalidateAllocation(vk.allocator.handle(), host.allocation, 0, VK_WHOLE_SIZE);
				void *p = mapBuffer(vk.allocator.handle(), host);
				std::memcpy(out.data(), p, static_cast<size_t>(bytes));
				unmapBuffer(vk.allocator.handle(), host);
				return out;
			};

			auto checkAlignments = [&](const char *what)
			{
				bool ok = (ChunkStateProbe::usedI(*chunk) % 12) == 0 &&
				          (ChunkStateProbe::usedWI(*chunk) % 12) == 0;
				for (int s = 0; s < kSections && ok; ++s)
					ok = (ChunkStateProbe::slotIOff(*chunk, s) % 12) == 0 &&
					     (ChunkStateProbe::slotISz(*chunk, s) % 12) == 0 &&
					     (ChunkStateProbe::slotWIOff(*chunk, s) % 12) == 0 &&
					     (ChunkStateProbe::slotWISz(*chunk, s) % 12) == 0;
				if (!ok)
					for (int s = 0; s < kSections; ++s)
						std::cerr << "  align s" << s
						          << " ioff=" << ChunkStateProbe::slotIOff(*chunk, s)
						          << " isz=" << ChunkStateProbe::slotISz(*chunk, s)
						          << " wioff=" << ChunkStateProbe::slotWIOff(*chunk, s)
						          << " wisz=" << ChunkStateProbe::slotWISz(*chunk, s)
						          << std::endl;
				CHECK(ok, what);
			};

			// The composed byte image of the index stream: live payloads
			// rebased to their slot's vertex base, gaps/slack/abandoned
			// ranges zero.
			auto compareIndexStream = [&](const char *what)
			{
				const VkDeviceSize bytes = ChunkStateProbe::usedI(*chunk);
				std::vector<uint8_t> expected(static_cast<size_t>(bytes), 0);
				bool shapesMatch = true;
				for (int s = 0; s < kSections; ++s)
				{
					const uint32_t iUsed = ChunkStateProbe::slotIUsed(*chunk, s);
					const size_t payload =
					    cpuView[static_cast<size_t>(s)].opaqueIndices.size() * sizeof(uint32_t);
					if (iUsed != payload)
						shapesMatch = false;
					if (iUsed == 0)
						continue;
					const size_t off = ChunkStateProbe::slotIOff(*chunk, s);
					const uint32_t base = ChunkStateProbe::slotVBase(*chunk, s);
					const auto &idx = cpuView[static_cast<size_t>(s)].opaqueIndices;
					for (size_t k = 0; k < idx.size(); ++k)
					{
						const uint32_t v = idx[k] + base;
						std::memcpy(expected.data() + off + k * sizeof(uint32_t), &v,
						            sizeof(uint32_t));
					}
				}
				CHECK(shapesMatch, what);
				const std::vector<uint8_t> got =
				    readbackBuffer(ChunkStateProbe::indexBufferOf(*chunk), bytes);
				if (got != expected)
				{
					for (size_t k = 0; k < got.size() && k < expected.size(); ++k)
						if (got[k] != expected[k])
						{
							std::cerr << "  index mismatch @" << k << "/" << got.size()
							          << " got";
							for (size_t j = k; j < k + 8 && j < got.size(); ++j)
								std::cerr << " " << static_cast<int>(got[j]);
							std::cerr << " want";
							for (size_t j = k; j < k + 8 && j < expected.size(); ++j)
								std::cerr << " " << static_cast<int>(expected[j]);
							std::cerr << " | slot0 off=" << ChunkStateProbe::slotIOff(*chunk, 0)
							          << " used=" << ChunkStateProbe::slotIUsed(*chunk, 0)
							          << " vbase=" << ChunkStateProbe::slotVBase(*chunk, 0)
							          << std::endl;
							break;
						}
				}
				CHECK(got == expected, what);
			};

			// Per-slot used ranges of the vertex stream (headroom slack is
			// unreferenced by construction and not zeroed).
			auto compareVertexStream = [&](const char *what)
			{
				const VkDeviceSize bytes = ChunkStateProbe::usedV(*chunk);
				const std::vector<uint8_t> got =
				    readbackBuffer(ChunkStateProbe::opaqueBuffer(*chunk), bytes);
				bool ok = got.size() == static_cast<size_t>(bytes);
				for (int s = 0; s < kSections && ok; ++s)
				{
					const uint32_t vUsed = ChunkStateProbe::slotVUsed(*chunk, s);
					if (vUsed == 0)
						continue;
					const size_t off = ChunkStateProbe::slotVOff(*chunk, s);
					const auto &verts = cpuView[static_cast<size_t>(s)].opaqueVertices;
					if (verts.size() * sizeof(Vertex) != vUsed ||
						    off + vUsed > got.size())
					{
						ok = false;
						break;
					}
					if (std::memcmp(got.data() + off, verts.data(),
					                static_cast<size_t>(vUsed)) != 0)
					{
						const uint8_t *a = got.data() + off;
						const uint8_t *b =
						    reinterpret_cast<const uint8_t *>(verts.data());
						for (size_t k = 0; k < vUsed; ++k)
							if (a[k] != b[k])
							{
								std::cerr << "  vertex mismatch section " << s
								          << " byte " << k << "/" << vUsed
								          << " got " << static_cast<int>(a[k])
								          << " want " << static_cast<int>(b[k])
								          << std::endl;
								break;
							}
						ok = false;
					}
				}
				CHECK(ok, what);
			};

			auto uploadPending = [&](const char *what)
			{
				staging.beginFrame(0);
				CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire),
				      what);
				// Submit the recorded copies; the retired buffers stay alive
				// until the end of the block (destroying them here would let
				// VMA recycle their handles across submissions, which trips
				// the sync layer's handle-based hazard tracking).
				CHECK(vk.flush(), "cow: upload submit");
			};

			// (A) Full repack baseline: the GPU byte image equals the packed
			// CPU payloads exactly (no gaps, no headroom).
			{
				uploadPending("cow: full repack upload");
				CHECK(vk.flush(), "cow: repack submit");
			}
			checkAlignments("cow: repacked index slots are triangle-aligned");
			compareIndexStream("cow: repacked index stream matches the composed payloads");
			compareVertexStream("cow: repacked vertex payloads match per slot");
			{
				uint32_t payloadSum = 0;
				for (int s = 0; s < kSections; ++s)
					payloadSum += static_cast<uint32_t>(
					    cpuView[static_cast<size_t>(s)].opaqueIndices.size() *
					    sizeof(uint32_t));
				CHECK(ChunkStateProbe::usedI(*chunk) == payloadSum,
				      "cow: repacked extent is the exact payload sum");
			}

			// (B) Outgrow: floating bricks inside section 5 grow its payload
			// beyond the exact slot => appended slot, buffer growth, and the
			// abandoned slot range zeroed inside the drawn region.
			{
				const VkDeviceSize oldCap = ChunkStateProbe::opaqueBuffer(*chunk).size;
				const uint32_t oldUsedI = ChunkStateProbe::usedI(*chunk);
				int placed = 0;
				for (int x = 0; x < 16 && placed < 8; ++x)
					for (int z = 0; z < 16 && placed < 8; ++z)
						if (chunk->getVoxel(static_cast<uint32_t>(x), 88,
						                    static_cast<uint32_t>(z))
						        .type == static_cast<uint8_t>(AIR))
						{
							chunk->setVoxel(x, 88, z, BRICKS);
							++placed;
						}
				CHECK(placed >= 4, "cow: grew section 5 with floating bricks");
				const uint16_t mask = chunk->takeDirtySections();
				CHECK((mask & (1u << 5)) != 0, "cow: growth edit dirties section 5");
				{
					MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
					chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), mask);
					chunk->getMeshResultPool()->finishBuild(r);
					for (int s = 0; s < kSections; ++s)
						if ((mask >> s) & 1u)
							cpuView[static_cast<size_t>(s)] = r->sections[static_cast<size_t>(s)];
					CHECK(chunk->publishMeshResult(r), "cow: growth result published");
				}
				uploadPending("cow: growth upload (append path)");
				checkAlignments("cow: appended slots stay triangle-aligned");
				CHECK(ChunkStateProbe::usedI(*chunk) != oldUsedI,
				      "cow: growth changed the packed extent");
				CHECK(ChunkStateProbe::opaqueBuffer(*chunk).size > oldCap,
				      "cow: buffer grew for the appended slot");
				CHECK(vk.flush(), "cow: growth submit");
				compareIndexStream("cow: index stream after append matches the composed payloads");
			}

			// (C) Emptied section: zero payload keeps its reserved slot,
			// the range reads as zeros, and the draw stays valid.
			{
				int target = -1;
				uint16_t best = 0xFFFF;
				for (int s = 0; s < kSections; ++s)
				{
					const uint16_t n = ChunkStateProbe::sectionNonAir(*chunk, s);
					if (n != 0 && n < best)
					{
						best = n;
						target = s;
					}
				}
				CHECK(target >= 0 && best <= 128, "cow: found a small section to empty");
				const int y0 = target * 16;
				for (int y = y0; y < y0 + 16; ++y)
					for (int x = 0; x < 16; ++x)
						for (int z = 0; z < 16; ++z)
							if (chunk->getVoxel(static_cast<uint32_t>(x),
							                    static_cast<uint32_t>(y),
							                    static_cast<uint32_t>(z))
							        .type != static_cast<uint8_t>(AIR))
								chunk->setVoxel(x, y, z, AIR);
				const uint16_t mask = chunk->takeDirtySections();
				CHECK((mask & (1u << target)) != 0, "cow: emptying dirties the section");
				{
					MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
					chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), mask);
					chunk->getMeshResultPool()->finishBuild(r);
					for (int s = 0; s < kSections; ++s)
						if ((mask >> s) & 1u)
							cpuView[static_cast<size_t>(s)] = r->sections[static_cast<size_t>(s)];
					CHECK(chunk->publishMeshResult(r), "cow: emptied result published");
				}
				uploadPending("cow: emptied-section upload");
				checkAlignments("cow: cleared slot keeps triangle alignment");
				CHECK(ChunkStateProbe::slotICount(*chunk, target) == 0,
				      "cow: emptied slot carries no indices");
				CHECK(ChunkStateProbe::slotISz(*chunk, target) > 0,
				      "cow: emptied slot keeps its reservation");
				CHECK(vk.flush(), "cow: emptied submit");
				compareIndexStream("cow: index stream after clear matches the composed payloads");
			}

			// (D) Full rebuild compacts the layout: exact slots,
			// back-to-back, no abandoned ranges.
			{
				CHECK(chunk->generateMesh(), "cow: full rebuild publishes");
				{
					MeshBuildResult *r = ChunkStateProbe::pending(*chunk);
					for (int s = 0; s < kSections; ++s)
						cpuView[static_cast<size_t>(s)] = r->sections[static_cast<size_t>(s)];
				}
				uploadPending("cow: compaction repack upload");
				checkAlignments("cow: compacted slots triangle-aligned");
				uint32_t liveSum = 0;
				bool exact = true;
				std::vector<uint32_t> ends;
				for (int s = 0; s < kSections; ++s)
				{
					const uint32_t iUsed = ChunkStateProbe::slotIUsed(*chunk, s);
					if (iUsed == 0)
						continue;
					exact = exact && ChunkStateProbe::slotISz(*chunk, s) == iUsed;
					liveSum += iUsed;
					ends.push_back(ChunkStateProbe::slotIOff(*chunk, s) + iUsed);
				}
				CHECK(exact, "cow: compacted slots are exact-sized");
				CHECK(ChunkStateProbe::usedI(*chunk) == liveSum,
				      "cow: compacted extent holds live bytes only");
				std::sort(ends.begin(), ends.end());
				CHECK(ends.empty() || ends.back() == ChunkStateProbe::usedI(*chunk),
				      "cow: compacted layout is back-to-back");
				CHECK(vk.flush(), "cow: compaction submit");
				compareIndexStream("cow: compacted index stream matches the composed payloads");
			}

			// (E) LOD <-> full transitions reset the slot state cleanly.
			{
				CHECK(chunk->generateLODMesh(), "cow: LOD publishes");
				uploadPending("cow: LOD upload");
				CHECK(chunk->isLODMesh(), "cow: LOD mesh active");
				bool anySlot = false;
				for (int s = 0; s < kSections; ++s)
					anySlot = anySlot || ChunkStateProbe::slotICount(*chunk, s) != 0 ||
					          ChunkStateProbe::slotIUsed(*chunk, s) != 0;
				CHECK(!anySlot, "cow: LOD upload clears the section slots");
				CHECK(ChunkStateProbe::usedI(*chunk) == 0 &&
				          ChunkStateProbe::usedV(*chunk) == 0,
				      "cow: LOD upload zeroes the used extents");
				// This seed's terrain is water-covered: the LOD carries water
				// quads only, so the opaque draw count legitimately reads zero.
				CHECK(chunk->getOpaqueIndexCount() > 0 || chunk->hasWaterMesh(),
				      "cow: LOD draw state set");

				CHECK(chunk->generateMesh(), "cow: full after LOD publishes");
				{
					MeshBuildResult *r = ChunkStateProbe::pending(*chunk);
					for (int s = 0; s < kSections; ++s)
						cpuView[static_cast<size_t>(s)] = r->sections[static_cast<size_t>(s)];
				}
				uploadPending("cow: full upload after LOD");
				checkAlignments("cow: slots realigned after LOD->full");
				CHECK(ChunkStateProbe::usedI(*chunk) > 0,
				      "cow: slots repopulated after LOD->full");
				CHECK(vk.flush(), "cow: LOD->full submit");
				compareIndexStream("cow: index stream after LOD->full matches the payloads");
			}

			// (F) A parked partial result forces the next dispatch to a full
			// build - partial CPU results are never composed together
			// (issue #107 dispatch contract, PR #117 review phase 30).
			{
				MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
				chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(),
								 static_cast<uint16_t>(1u << 9));
				chunk->getMeshResultPool()->finishBuild(r);
				CHECK(chunk->publishMeshResult(r), "cow: partial result parked");
				CHECK(chunk->hasUnuploadedFullMesh(),
				      "cow: parked partial blocks partial composition");
				chunk->setVoxel(7, 90, 7, BRICKS);
				CHECK(chunk->hasUnuploadedFullMesh(),
				      "cow: still parked after a further edit");
				CHECK(chunk->generateMesh(),
				      "cow: full build replaces the parked partial");
				CHECK(ChunkStateProbe::pending(*chunk) != nullptr &&
				              ChunkStateProbe::pending(*chunk)->sectionsBuilt ==
				                  kAllSectionMask,
				          "cow: parked partial replaced by the full build");
				uploadPending("cow: full upload after parked partial");
				CHECK(vk.flush(), "cow: parked-partial submit");
				CHECK(chunk->getOpaqueIndexCount() > 0, "cow: draw count valid after the swap");
			}

			// All submissions completed: safe to release every retired buffer
			// and the readback staging kept alive above.
			vkDeviceWaitIdle(vk.device);
			retire.flush();
			for (AllocatedBuffer &b : readbackKeepAlive)
				destroyBuffer(vk.allocator.handle(), b);
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
