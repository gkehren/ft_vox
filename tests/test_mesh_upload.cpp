// Mesh upload lifecycle against a real (headless) Vulkan device, through the
// shared mesh arenas (issue #109; #104 acceptance criteria, #117 review
// invariants):
//   - a staging-ring-full frame defers the upload WITHOUT losing, copying or
//     rebuilding the completed build result, and without consuming any arena
//     range;
//   - the retry succeeds from the very same CPU result;
//   - a partial staging failure (opaque fits, the first water range does not)
//     leaves every range, slot and draw count untouched (transactional);
//   - an emptied section keeps its reservation on a partial upload and a
//     full repack re-allocates compactly;
//   - indirect draw command collection groups by arena page pair.
// Skips quietly when no Vulkan loader/device is available (CI containers).
#include <Chunk/Chunk.hpp>
#include <Chunk/ChunkManager.hpp>
#include <Chunk/ChunkMeshResult.hpp>
#include <Chunk/ChunkPool.hpp>
#include <Chunk/TerrainGenerator.hpp>
#include <Vulkan/MeshArena.hpp>
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
	// Section range layout (scalar getters only: SectionGpuSlot is private).
	static uint32_t slotVPage(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].vertexPage; }
	static uint32_t slotVOff(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].vertexOffset; }
	static uint32_t slotVSz(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].vertexSlotBytes; }
	static uint32_t slotVUsed(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].vertexUsedBytes; }
	static uint32_t slotVBase(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].vertexBase; }
	static uint32_t slotIPage(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].indexPage; }
	static uint32_t slotIOff(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].indexOffset; }
	static uint32_t slotISz(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].indexSlotBytes; }
	static uint32_t slotIUsed(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].indexUsedBytes; }
	static uint32_t slotICount(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].indexCount; }
	static uint32_t slotWICount(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].indexCount; }
	static bool slotEmpty(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].empty(); }
	static uint16_t sectionNonAir(const Chunk &c, int s) { return c.m_sectionNonAir[static_cast<size_t>(s)]; }
};

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

size_t totalOpaqueIndices(const MeshBuildResult &r)
{
	size_t n = 0;
	for (const SectionMeshPayload &p : r.sections)
		n += p.opaqueIndices.size();
	return n;
}

} // namespace

int main()
{
	HeadlessDevice vk;
	if (!vk.init())
	{
		std::cout << "SKIP: mesh upload lifecycle (no Vulkan device available)\n";
		return 0;
	}

	StagingRing staging;
	staging.init(vk.allocator.handle(), 1); // default 48 MiB - roomy ring
	staging.beginFrame(0);
	GpuResourceRetire retire;
	retire.init(vk.allocator.handle(), 2);
	retire.beginFrame(0);
	MeshArenas arenas;
	arenas.init(vk.allocator.handle(), retire, sizeof(Vertex));

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
			CHECK(first->sectionsBuilt == kAllSectionMask, "full build stamps every section");

			const size_t firstIndexCount = totalOpaqueIndices(*first);
			CHECK(firstIndexCount > 0, "full build emitted geometry");
			CHECK(arenas.opaqueVertex.metrics().pages == 0, "no arena page before upload");

			// Baseline: a roomy ring uploads the first mesh into the arenas.
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "roomy staging uploads the mesh");
			CHECK(!chunk->hasPendingMeshResult(), "result consumed by upload");
			CHECK(!chunk->needsGPUUpload(), "meshNeedsUpdate cleared by upload");
			CHECK(chunk->getOpaqueIndexCount() == firstIndexCount,
				  "drawn index count equals the exact packed payload");
			const uint32_t pagesAfterFull = arenas.opaqueIndex.metrics().pages;
			CHECK(pagesAfterFull >= 1, "full upload creates arena pages");
			CHECK(vk.flush(), "first upload submit");
			retire.flush();

			// Staging-full deferral: the completed result is neither lost nor
			// copied, no arena range is consumed, and the retry succeeds from
			// the very same result (issue #104 items 25-26, issue #109).
			CHECK(chunk->generateMesh(), "second mesh publishes");
			MeshBuildResult *second = ChunkStateProbe::pending(*chunk);
			CHECK(second != nullptr, "fresh result attached");
			staging.beginFrame(0);

			const uint32_t oldCount = chunk->getOpaqueIndexCount();
			const uint64_t oldGen = ChunkStateProbe::meshGeneration(*chunk);
			const size_t oldActive = chunk->getMeshResultPool()->activeCount();
			{
				VkDeviceSize off = 0;
				void *sink = nullptr;
				CHECK(staging.alloc(staging.sliceCapacity(), off, sink),
					  "drain allocation fills the slice");
			}

			CHECK(!chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "staging-full defers the upload");
			CHECK(ChunkStateProbe::pending(*chunk) == second,
				  "the exact same result stays attached (no rebuild, no copy)");
			CHECK(chunk->meshGeneration() == oldGen, "identity untouched");
			CHECK(chunk->getMeshResultPool()->activeCount() == oldActive,
				  "result stays active while deferred");
			CHECK(chunk->needsGPUUpload(), "meshNeedsUpdate stays armed while deferred");
			CHECK(chunk->getOpaqueIndexCount() == oldCount,
				  "draw counts unchanged by the deferred attempt");
			CHECK(arenas.opaqueIndex.metrics().pages == pagesAfterFull,
				  "deferred attempt consumes no arena page");

			staging.beginFrame(0);
			const size_t secondIndexCount = totalOpaqueIndices(*second);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "retry upload succeeds without a rebuild");
			CHECK(!chunk->hasPendingMeshResult(), "retry consumes the same result");
			CHECK(chunk->getOpaqueIndexCount() >= secondIndexCount,
				  "retry uploads the deferred payload");
			CHECK(vk.flush(), "retry submit");
			retire.flush();

			// Partial staging failure (issue #104 item 28): opaque ranges
			// would fit, the first water range does not. The whole upload
			// must be transactional: no range, slot or draw count moves.
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
			// Snapshot the whole section-range state.
			uint32_t snap[16][10];
			for (int s = 0; s < 16; ++s)
			{
				snap[s][0] = ChunkStateProbe::slotVPage(*chunk, s);
				snap[s][1] = ChunkStateProbe::slotVOff(*chunk, s);
				snap[s][2] = ChunkStateProbe::slotVSz(*chunk, s);
				snap[s][3] = ChunkStateProbe::slotVBase(*chunk, s);
				snap[s][4] = ChunkStateProbe::slotIPage(*chunk, s);
				snap[s][5] = ChunkStateProbe::slotIOff(*chunk, s);
				snap[s][6] = ChunkStateProbe::slotISz(*chunk, s);
				snap[s][7] = ChunkStateProbe::slotIUsed(*chunk, s);
				snap[s][8] = ChunkStateProbe::slotICount(*chunk, s);
				snap[s][9] = ChunkStateProbe::slotWICount(*chunk, s);
			}
			const uint32_t snapDraw = chunk->getOpaqueIndexCount();
			const uint32_t pagesBefore = arenas.opaqueVertex.metrics().pages +
			                             arenas.opaqueIndex.metrics().pages +
			                             arenas.waterVertex.metrics().pages +
			                             arenas.waterIndex.metrics().pages;

			staging.beginFrame(0);
			{
				// Leave exactly room for the opaque stream; the first water
				// allocation cannot fit in what remains.
				VkDeviceSize off = 0;
				void *sink = nullptr;
				VkDeviceSize opaqueNeeds = 0;
				for (int s = 0; s < 16; ++s)
				{
					opaqueNeeds += static_cast<VkDeviceSize>(
						((third->sections[static_cast<size_t>(s)].opaqueVertices.size() *
				          sizeof(Vertex)) +
				         StagingRing::kAlignment - 1) /
				        StagingRing::kAlignment * StagingRing::kAlignment);
					opaqueNeeds += static_cast<VkDeviceSize>(
						((third->sections[static_cast<size_t>(s)].opaqueIndices.size() *
				          sizeof(uint32_t)) +
				         StagingRing::kAlignment - 1) /
				        StagingRing::kAlignment * StagingRing::kAlignment);
				}
				const VkDeviceSize drain = staging.sliceCapacity() - opaqueNeeds;
				CHECK(staging.alloc(drain, off, sink),
					  "drain leaves exactly the opaque sections of room");
			}

			CHECK(!chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "partial staging failure defers the upload");
			CHECK(ChunkStateProbe::pending(*chunk) == third,
				  "CPU result survives the partial failure");
			CHECK(chunk->needsGPUUpload(), "retry still pending after partial failure");
			{
				bool identical = true;
				for (int s = 0; s < 16 && identical; ++s)
					identical = ChunkStateProbe::slotVPage(*chunk, s) == snap[s][0] &&
					            ChunkStateProbe::slotVOff(*chunk, s) == snap[s][1] &&
					            ChunkStateProbe::slotVSz(*chunk, s) == snap[s][2] &&
					            ChunkStateProbe::slotVBase(*chunk, s) == snap[s][3] &&
					            ChunkStateProbe::slotIPage(*chunk, s) == snap[s][4] &&
					            ChunkStateProbe::slotIOff(*chunk, s) == snap[s][5] &&
					            ChunkStateProbe::slotISz(*chunk, s) == snap[s][6] &&
					            ChunkStateProbe::slotIUsed(*chunk, s) == snap[s][7] &&
					            ChunkStateProbe::slotICount(*chunk, s) == snap[s][8] &&
					            ChunkStateProbe::slotWICount(*chunk, s) == snap[s][9];
				CHECK(identical, "water-failure attempt leaves every section slot untouched");
				const uint32_t pagesAfter =
					arenas.opaqueVertex.metrics().pages + arenas.opaqueIndex.metrics().pages +
					arenas.waterVertex.metrics().pages + arenas.waterIndex.metrics().pages;
				CHECK(pagesAfter == pagesBefore,
					  "water-failure attempt consumes no new arena page");
				CHECK(chunk->getOpaqueIndexCount() == snapDraw,
					  "water-failure attempt leaves draw counts untouched");
			}

			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "final retry succeeds");
			CHECK(chunk->hasWaterMesh(), "water mesh uploaded after the retry");
			CHECK(vk.flush(), "final retry submit");
		}
	}

	// Arena lifecycle block: emptied section keeps its reservation on a
	// partial upload; a full repack re-allocates compactly; indirect draw
	// collection groups by page pair.
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
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "cow: full upload");
			CHECK(vk.flush(), "cow: full submit");

			// Indirect collection: every live section yields exactly one
			// command, each carrying valid arena pages.
			{
				std::vector<Chunk::IndirectDraw> cmds;
				uint32_t liveSections = 0, indexSum = 0;
				for (int s = 0; s < 16; ++s)
				{
					liveSections += ChunkStateProbe::slotICount(*chunk, s) != 0 ? 1u : 0u;
					indexSum += ChunkStateProbe::slotICount(*chunk, s);
				}
				chunk->collectOpaqueDraws(cmds);
				// Contiguous sections merge into one command, so the count is
				// at most the live-section count; the drawn indices are the
				// same set either way.
				CHECK(cmds.size() <= liveSections,
					  "cow: collection yields at most one command per live section");
				uint32_t cmdSum = 0;
				for (const auto &d : cmds)
					cmdSum += d.cmd.indexCount;
				CHECK(cmdSum == indexSum,
					  "cow: merged commands still cover every live index");
				bool pagesValid = true;
				for (const auto &d : cmds)
					pagesValid = pagesValid &&
					             d.vertexPage != MeshArena::kNoPage &&
					             d.indexPage != MeshArena::kNoPage &&
					             d.cmd.indexCount > 0 && d.cmd.instanceCount == 1;
				CHECK(pagesValid, "cow: collected commands carry valid arena pages");
			}

			// Empty the smallest section, partial upload: the reservation is
			// kept (used extents drop to zero) so reactivation stays cheap.
			int target = -1;
			uint16_t best = 0xFFFF;
			for (int s = 0; s < 16; ++s)
			{
				const uint16_t n = ChunkStateProbe::sectionNonAir(*chunk, s);
				// Only sections that own a reserved range exercise the
				// keep-reservation path (a fully buried section may have no
				// visible faces and therefore no range at all).
				if (n != 0 && ChunkStateProbe::slotISz(*chunk, s) > 0 && n < best)
				{
					best = n;
					target = s;
				}
			}
			CHECK(target >= 0, "cow: found a reserved section to empty");
			const int y0 = target * 16;
			// Collect the section's voxels; drain all but the last first so
			// the partial upload exercises the keep-reservation path.
			std::vector<std::pair<glm::ivec3, uint8_t>> voxels;
			for (int y = y0; y < y0 + 16; ++y)
				for (int x = 0; x < 16; ++x)
					for (int z = 0; z < 16; ++z)
					{
						const uint8_t t = chunk->getVoxel(static_cast<uint32_t>(x),
						                                  static_cast<uint32_t>(y),
						                                  static_cast<uint32_t>(z))
						                      .type;
						if (t != static_cast<uint8_t>(AIR))
							voxels.emplace_back(glm::ivec3(x, y, z), t);
					}
			CHECK(voxels.size() >= 2, "cow: section holds voxels to drain");
			for (size_t k = 0; k + 1 < voxels.size(); ++k)
				chunk->setVoxel(static_cast<uint32_t>(voxels[k].first.x),
				                static_cast<uint32_t>(voxels[k].first.y),
				                static_cast<uint32_t>(voxels[k].first.z), AIR);
			const uint16_t mask = chunk->takeDirtySections();
			CHECK((mask & (1u << target)) != 0, "cow: draining dirties the section");
			{
				MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
				chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), mask);
				chunk->getMeshResultPool()->finishBuild(r);
				CHECK(chunk->publishMeshResult(r), "cow: drained result published");
			}
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "cow: drained-section upload");
			CHECK(ChunkStateProbe::slotISz(*chunk, target) > 0,
				  "cow: in-place rebuild keeps the section's reservation");

			// Now remove the last voxel: a small partial upload. The emptied
			// section keeps its reservation (used extents drop to zero).
			chunk->setVoxel(static_cast<uint32_t>(voxels.back().first.x),
			                static_cast<uint32_t>(voxels.back().first.y),
			                static_cast<uint32_t>(voxels.back().first.z), AIR);
			const uint16_t mask2 = chunk->takeDirtySections();
			CHECK((mask2 & (1u << target)) != 0, "cow: final removal dirties the section");
			{
				MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
				chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), mask2);
				chunk->getMeshResultPool()->finishBuild(r);
				CHECK(chunk->publishMeshResult(r), "cow: emptied result published");
			}
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "cow: emptied-section upload");
			CHECK(ChunkStateProbe::slotICount(*chunk, target) == 0,
				  "cow: emptied slot carries no indices");
			CHECK(ChunkStateProbe::slotISz(*chunk, target) > 0,
				  "cow: emptied slot keeps its reservation");
			CHECK(vk.flush(), "cow: emptied submit");

			// Full rebuild: the repack re-allocates every section; the
			// emptied section's stale reservation must be gone.
			CHECK(chunk->generateMesh(), "cow: post-clear full rebuild publishes");
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "cow: post-clear repack upload");
			CHECK(ChunkStateProbe::slotEmpty(*chunk, target),
				  "cow: full repack drops the reservation of an emptied section");
			CHECK(vk.flush(), "cow: post-clear repack submit");

			// Reactivation: a fresh range is allocated (reservation was
			// dropped) and the draw count reflects the new section.
			chunk->setVoxel(8, y0 + 8, 8, BRICKS);
			const uint16_t reactivateMask = chunk->takeDirtySections();
			CHECK((reactivateMask & (1u << target)) != 0, "cow: reactivation dirties the section");
			{
				MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
				chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), reactivateMask);
				chunk->getMeshResultPool()->finishBuild(r);
				CHECK(chunk->publishMeshResult(r), "cow: reactivation published");
			}
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "cow: reactivation upload");
			CHECK(!ChunkStateProbe::slotEmpty(*chunk, target),
				  "cow: reactivated section owns a real range");
			CHECK(ChunkStateProbe::slotICount(*chunk, target) > 0,
				  "cow: reactivated section carries indices");
			CHECK(vk.flush(), "cow: reactivation submit");
		}
	}

	arenas.shutdown();
	staging.shutdown();
	retire.shutdown();
	vk.shutdown();

	if (g_fails != 0)
	{
		std::cerr << g_fails << " check(s) failed\n";
		return 1;
	}
	std::cout << "PASS: mesh upload lifecycle - arenas, defer, retry, atomicity\n";
	return 0;
}
