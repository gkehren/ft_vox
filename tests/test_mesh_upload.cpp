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
#include <Vulkan/VkCommands.hpp>
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
	static uint32_t slotWVPage(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].vertexPage; }
	static uint32_t slotWVOff(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].vertexOffset; }
	static uint32_t slotWVSz(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].vertexSlotBytes; }
	static uint32_t slotWIPage(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].indexPage; }
	static uint32_t slotWIOff(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].indexOffset; }
	static uint32_t slotWISz(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].indexSlotBytes; }
	static bool slotEmpty(const Chunk &c, int s) { return c.m_sectionGpu[static_cast<size_t>(s)].empty(); }
	static bool slotWaterEmpty(const Chunk &c, int s) { return c.m_sectionGpuWater[static_cast<size_t>(s)].empty(); }
	static uint16_t sectionNonAir(const Chunk &c, int s) { return c.m_sectionNonAir[static_cast<size_t>(s)]; }
	static MeshArena::Range lodOpaqueV(const Chunk &c) { return c.m_lodOpaqueVertices; }
	static MeshArena::Range lodOpaqueI(const Chunk &c) { return c.m_lodOpaqueIndices; }
	static MeshArena::Range lodWaterV(const Chunk &c) { return c.m_lodWaterVertices; }
	static MeshArena::Range lodWaterI(const Chunk &c) { return c.m_lodWaterIndices; }
};

// Friend probe declared in ChunkManager.hpp: exposes the geometric commit
// group bookkeeping so group lifetime, lifecycle state, fusion and member
// sets are observable without public API.
struct ChunkManagerProbe
{
	static size_t commitGroups(const ChunkManager &m) { return m.m_commitGroups.size(); }
	static CommitGroupState groupState(const ChunkManager &m, size_t index)
	{
		return m.m_commitGroups[index].state;
	}
	static std::vector<Chunk *> groupChunks(const ChunkManager &m, size_t index)
	{
		std::vector<Chunk *> out;
		for (const PendingGroupMember &member : m.m_commitGroups[index].members)
			out.push_back(member.chunk);
		return out;
	}
	static bool replacementRecorded(const ChunkManager &m, size_t index, const Chunk *chunk)
	{
		for (const PendingGroupMember &member : m.m_commitGroups[index].members)
			if (member.chunk == chunk)
				return member.replacement.recorded;
		return false;
	}
	static bool replacementValid(const ChunkManager &m, size_t index, const Chunk *chunk)
	{
		for (const PendingGroupMember &member : m.m_commitGroups[index].members)
			if (member.chunk == chunk)
				return member.replacement.valid;
		return false;
	}
	static void registerGroup(ChunkManager &m, std::vector<Chunk *> members)
	{
		std::vector<PendingGroupMember> converted;
		for (Chunk *chunk : members)
			converted.push_back({chunk, {}});
		m.registerCommitGroup(0, std::move(converted));
	}
	static void removeFromGroups(ChunkManager &m, Chunk *chunk)
	{
		m.removeChunkFromCommitGroups(chunk);
	}
};

namespace
{

bool groupContains(const std::vector<Chunk *> &chunks, Chunk *chunk)
{
	for (Chunk *member : chunks)
		if (member == chunk)
			return true;
	return false;
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

	// Submit everything recorded so far, wait, and re-begin the buffer so
	// GPU buffer contents become observable (readback checks).
	bool flush()
	{
		VkResult endR = vkEndCommandBuffer(cmd);
		if (endR != VK_SUCCESS)
		{
			std::cerr << "flush: end=" << int(endR) << std::endl;
			return false;
		}
		VkSubmitInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		VkResult subR = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
		if (subR != VK_SUCCESS)
		{
			std::cerr << "flush: submit=" << int(subR) << std::endl;
			return false;
		}
		VkResult waitR = vkQueueWaitIdle(queue);
		if (waitR != VK_SUCCESS)
		{
			std::cerr << "flush: wait=" << int(waitR) << std::endl;
			return false;
		}
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
	// Small pages: the spawn e2e block below then spans MULTIPLE pages per
	// stream, exercising the page-pair grouping of the indirect path.
	arenas.init(vk.allocator.handle(), retire, sizeof(Vertex), 2,
	            2ull * 1024ull * 1024ull, 1ull * 1024ull * 1024ull);
	ImmediateCommands imm;
	imm.init(vk.device, vk.queue, vk.queueFamily);

	// lastFrameUsed() (issue #179 review): beginFrame() resets the slice
	// cursor, but must first capture the completed frame's consumption so
	// mid-frame debug/UI reads (after the reset, before this frame's copies)
	// still see meaningful staging traffic.
	{
		staging.beginFrame(0);
		CHECK(staging.usedThisFrame() == 0, "slice cursor resets at beginFrame");
		VkDeviceSize off = 0;
		void *sink = nullptr;
		CHECK(staging.alloc(4096, off, sink), "staging alloc recorded in frame A");
		CHECK(staging.usedThisFrame() >= 4096, "consumption visible within the frame");
		staging.beginFrame(0);
		CHECK(staging.usedThisFrame() == 0, "cursor reset for the next frame");
		CHECK(staging.lastFrameUsed() >= 4096, "lastFrameUsed holds the completed frame's usage");
	}

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

	// Arena lifecycle block: emptied section drops its reservation immediately
	// on a partial upload; reactivation allocates fresh ranges; indirect draw
	// collection yields one command per live section grouped by page pair.
	{
		ChunkPool chunkPool2(16);
		TerrainGenerator gen2(77);
		ChunkManager manager2(&gen2, nullptr, &chunkPool2);
		Camera cam2(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings2;
		manager2.updateStreaming(cam2, settings2);
		manager2.processChunkLoading(64);
		Chunk *chunk = manager2.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "arena: chunk registered");
		if (chunk)
		{
			CHECK(manager2.prepareAndGenerateChunk(chunk, gen2), "arena: prepare+generate");
			CHECK(chunk->generateMesh(), "arena: full mesh publishes");
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "arena: full upload");
			CHECK(vk.flush(), "arena: full submit");

			// Indirect collection: every live section yields exactly one
			// command carrying its own vertexOffset (issue #109).
			{
				std::vector<Chunk::IndirectDraw> cmds;
				uint32_t liveSections = 0, indexSum = 0;
				for (int s = 0; s < 16; ++s)
				{
					liveSections += ChunkStateProbe::slotICount(*chunk, s) != 0 ? 1u : 0u;
					indexSum += ChunkStateProbe::slotICount(*chunk, s);
				}
				chunk->collectOpaqueDraws(cmds);
				CHECK(cmds.size() == liveSections,
					  "arena: collection yields exactly one command per live section");
				uint32_t cmdSum = 0;
				for (const auto &d : cmds)
					cmdSum += d.cmd.indexCount;
				CHECK(cmdSum == indexSum,
					  "arena: section commands cover every live index");
				bool pagesValid = true;
				for (const auto &d : cmds)
					pagesValid = pagesValid &&
					             d.vertexPage != MeshArena::kNoPage &&
					             d.indexPage != MeshArena::kNoPage &&
					             d.cmd.indexCount > 0 && d.cmd.instanceCount == 1;
				CHECK(pagesValid, "arena: collected commands carry valid arena pages");
			}

			// Empty the smallest section: partial upload must drop its reservation
			// immediately (slot becomes empty, old ranges retired; Phase 2 item 4).
			int target = -1;
			uint16_t best = 0xFFFF;
			for (int s = 0; s < 16; ++s)
			{
				const uint16_t n = ChunkStateProbe::sectionNonAir(*chunk, s);
				if (n != 0 && ChunkStateProbe::slotISz(*chunk, s) > 0 && n < best)
				{
					best = n;
					target = s;
				}
			}
			CHECK(target >= 0, "arena: found a reserved section to empty");
			const int y0 = target * 16;
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
			CHECK(voxels.size() >= 2, "arena: section holds voxels to drain");
			for (size_t k = 0; k + 1 < voxels.size(); ++k)
				chunk->setVoxel(static_cast<uint32_t>(voxels[k].first.x),
				                static_cast<uint32_t>(voxels[k].first.y),
				                static_cast<uint32_t>(voxels[k].first.z), AIR);
			const uint16_t mask = chunk->takeDirtySections();
			CHECK((mask & (1u << target)) != 0, "arena: draining dirties the section");
			{
				MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
				chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), mask);
				chunk->getMeshResultPool()->finishBuild(r);
				CHECK(chunk->publishMeshResult(r), "arena: drained result published");
			}
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "arena: drained-section upload");

			// Remove the last voxel: partial upload.
			// Emptied section MUST retire its reservation and become empty.
			chunk->setVoxel(static_cast<uint32_t>(voxels.back().first.x),
			                static_cast<uint32_t>(voxels.back().first.y),
			                static_cast<uint32_t>(voxels.back().first.z), AIR);
			const uint16_t mask2 = chunk->takeDirtySections();
			CHECK((mask2 & (1u << target)) != 0, "arena: final removal dirties the section");
			{
				MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
				chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), mask2);
				chunk->getMeshResultPool()->finishBuild(r);
				CHECK(chunk->publishMeshResult(r), "arena: emptied result published");
			}
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "arena: emptied-section upload");
			CHECK(ChunkStateProbe::slotICount(*chunk, target) == 0,
				  "arena: emptied slot carries no indices");
			CHECK(ChunkStateProbe::slotEmpty(*chunk, target),
				  "arena: emptied slot drops its reservation immediately");
			CHECK(vk.flush(), "arena: emptied submit");

			// Full rebuild: every section is rebuilt; emptied section remains empty.
			CHECK(chunk->generateMesh(), "arena: post-clear full rebuild publishes");
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "arena: post-clear repack upload");
			CHECK(ChunkStateProbe::slotEmpty(*chunk, target),
				  "arena: full repack keeps emptied section empty");
			CHECK(vk.flush(), "arena: post-clear repack submit");

			// Reactivation: a fresh range is allocated and the draw count reflects the new section.
			chunk->setVoxel(8, y0 + 8, 8, BRICKS);
			const uint16_t reactivateMask = chunk->takeDirtySections();
			CHECK((reactivateMask & (1u << target)) != 0, "arena: reactivation dirties the section");
			{
				MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
				chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), reactivateMask);
				chunk->getMeshResultPool()->finishBuild(r);
				CHECK(chunk->publishMeshResult(r), "arena: reactivation published");
			}
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				  "arena: reactivation upload");
			CHECK(!ChunkStateProbe::slotEmpty(*chunk, target),
				  "arena: reactivated section owns a real range");
			CHECK(ChunkStateProbe::slotICount(*chunk, target) > 0,
				  "arena: reactivated section carries indices");
			CHECK(vk.flush(), "arena: reactivation submit");
		}
	}

	// Imm-path (spawn/bootstrap) content verification: uploadToGPU with
	// ImmediateCommands must land byte-exact section payloads in the arenas.
	{
		ChunkPool chunkPool3(8);
		TerrainGenerator gen3(42);
		ChunkManager manager3(&gen3, nullptr, &chunkPool3);
		Camera cam3(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings3;
		manager3.updateStreaming(cam3, settings3);
		manager3.processChunkLoading(64);
		Chunk *chunk = manager3.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "imm: chunk registered");
		if (chunk)
		{
			CHECK(manager3.prepareAndGenerateChunk(chunk, gen3), "imm: prepare+generate");
			CHECK(chunk->generateMesh(), "imm: mesh publishes");
			MeshBuildResult *r = ChunkStateProbe::pending(*chunk);
			std::vector<SectionMeshPayload> cpu(r->sections.begin(), r->sections.end());
			chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);

			// Read back every section's used vertex/index bytes from the
			// arena pages and compare against the CPU payloads.
			int badSections = 0;
			for (int sec = 0; sec < 16; ++sec)
			{
				const uint32_t vUsed = ChunkStateProbe::slotVUsed(*chunk, sec);
				const uint32_t iUsed = ChunkStateProbe::slotIUsed(*chunk, sec);
				if (vUsed == 0 && iUsed == 0)
					continue;
				if (vUsed != cpu[static_cast<size_t>(sec)].opaqueVertices.size() * sizeof(Vertex) ||
				    iUsed != cpu[static_cast<size_t>(sec)].opaqueIndices.size() * sizeof(uint32_t))
				{
					++badSections;
					continue;
				}
				// vertex readback
				{
					AllocatedBuffer &page =
					    arenas.opaqueVertex.pageBufferRef(ChunkStateProbe::slotVPage(*chunk, sec));
					std::vector<uint8_t> got = [&]
					{
						std::vector<uint8_t> out(static_cast<size_t>(vUsed), 0);
						AllocatedBuffer host = createBuffer(
						    vk.allocator.handle(), vUsed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
						    VMA_MEMORY_USAGE_AUTO,
						    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
						        VMA_ALLOCATION_CREATE_MAPPED_BIT);
						VkBufferCopy c{};
						c.srcOffset = ChunkStateProbe::slotVOff(*chunk, sec);
						c.size = vUsed;
						vkCmdCopyBuffer(vk.cmd, page.buffer, host.buffer, 1, &c);
						CHECK(vk.flush(), "imm readback submit");
						vmaInvalidateAllocation(vk.allocator.handle(), host.allocation, 0,
						                        VK_WHOLE_SIZE);
						void *p2 = mapBuffer(vk.allocator.handle(), host);
						std::memcpy(out.data(), p2, static_cast<size_t>(vUsed));
						unmapBuffer(vk.allocator.handle(), host);
						destroyBuffer(vk.allocator.handle(), host);
						return out;
					}();
					if (got != std::vector<uint8_t>(
					               reinterpret_cast<const uint8_t *>(
					                   cpu[static_cast<size_t>(sec)].opaqueVertices.data()),
					               reinterpret_cast<const uint8_t *>(
					                   cpu[static_cast<size_t>(sec)].opaqueVertices.data()) +
					                   vUsed))
						++badSections;
				}
				// index readback
				{
					AllocatedBuffer &page =
					    arenas.opaqueIndex.pageBufferRef(ChunkStateProbe::slotIPage(*chunk, sec));
					std::vector<uint8_t> out(static_cast<size_t>(iUsed), 0);
					AllocatedBuffer host = createBuffer(
					    vk.allocator.handle(), iUsed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
					    VMA_MEMORY_USAGE_AUTO,
					    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
					        VMA_ALLOCATION_CREATE_MAPPED_BIT);
					VkBufferCopy c{};
					c.srcOffset = ChunkStateProbe::slotIOff(*chunk, sec);
					c.size = iUsed;
					vkCmdCopyBuffer(vk.cmd, page.buffer, host.buffer, 1, &c);
					CHECK(vk.flush(), "imm readback submit");
					vmaInvalidateAllocation(vk.allocator.handle(), host.allocation, 0,
					                        VK_WHOLE_SIZE);
					void *p2 = mapBuffer(vk.allocator.handle(), host);
					std::memcpy(out.data(), p2, static_cast<size_t>(iUsed));
					unmapBuffer(vk.allocator.handle(), host);
					destroyBuffer(vk.allocator.handle(), host);
					if (out != std::vector<uint8_t>(
					               reinterpret_cast<const uint8_t *>(
					                   cpu[static_cast<size_t>(sec)].opaqueIndices.data()),
					               reinterpret_cast<const uint8_t *>(
					                   cpu[static_cast<size_t>(sec)].opaqueIndices.data()) +
					                   iUsed))
						++badSections;
				}
			}
			CHECK(badSections == 0,
			      "imm: every section's arena bytes match the CPU payloads");

			// Command validity on the bootstrap chunk: every collected
			// command must start at a slot offset, never cross slack, and
			// cover the whole used range.
			{
				struct Range
				{
					uint32_t off, sz, used;
				};
				std::vector<Range> slots;
				for (int sec = 0; sec < 16; ++sec)
					if (ChunkStateProbe::slotIUsed(*chunk, sec) != 0)
						slots.push_back({ChunkStateProbe::slotIOff(*chunk, sec),
						                 ChunkStateProbe::slotISz(*chunk, sec),
						                 ChunkStateProbe::slotIUsed(*chunk, sec)});
				std::sort(slots.begin(), slots.end(),
				          [](const Range &a, const Range &b) { return a.off < b.off; });
				std::vector<Chunk::IndirectDraw> cmds;
				chunk->collectOpaqueDraws(cmds);
				bool valid = !cmds.empty();
				uint32_t covered = 0;
				for (const auto &d : cmds)
				{
					const uint32_t start = d.cmd.firstIndex * sizeof(uint32_t);
					const uint32_t end = start + d.cmd.indexCount * sizeof(uint32_t);
					// Walk the sorted slots: the command must start at a slot
					// offset, cross only exact (used==reserved) slots, and
					// end exactly at a slot's used end.
					uint32_t pos = start;
					bool ok = false;
					for (size_t k = 0; k < slots.size() && pos < end; ++k)
					{
						if (slots[k].off != pos)
							continue;
						if (pos + slots[k].used >= end)
						{
							ok = pos + slots[k].used == end;
							break;
						}
						if (slots[k].used != slots[k].sz)
							break; // would draw slack: invalid
						pos += slots[k].used;
					}
					if (!ok)
						valid = false;
					covered += d.cmd.indexCount * sizeof(uint32_t);
				}
				uint32_t usedTotal = 0;
				for (const auto &sl : slots)
					usedTotal += sl.used;
				CHECK(covered == usedTotal,
				      "imm: commands cover every live index exactly once");
				CHECK(valid, "imm: commands map exactly to each section's used range");
			}
		}
	}

	// Spawn-path end-to-end (issue #109 regression): generateInitialArea
	// uploads every bootstrap chunk through the imm path; after simulated
	// frames the collected indirect commands must still cover each chunk's
	// live indices exactly (no ranges lost, no slack drawn).
	{
		ChunkPool chunkPool4(64);
		TerrainGenerator gen4(42);
		ChunkManager manager4(&gen4, nullptr, &chunkPool4);
		manager4.generateInitialArea(glm::vec3(0.0f), 2, vk.allocator.handle(), imm, arenas);

		uint32_t totalChunks = 0, totalCmds = 0, totalIndices = 0;
		bool allValid = true;
		for (int iteration = 0; iteration < 2; ++iteration)
		{
			// Simulate frames: retirement of freed ranges must never touch
			// live chunk ranges.
			for (uint64_t f = 1; f <= 5; ++f)
				arenas.beginFrame(f * 10 + iteration);

			allValid = true;
			totalChunks = 0;
			totalCmds = 0;
			totalIndices = 0;
			for (Chunk *chunk : manager4.getActiveChunks())
			{
				if (!chunk || chunk->getOpaqueIndexCount() == 0)
					continue;
				++totalChunks;
				struct Range
				{
					uint32_t off, sz, used;
				};
				std::vector<Range> slots;
				std::vector<Chunk::IndirectDraw> cmds;
				chunk->collectOpaqueDraws(cmds);
				totalCmds += static_cast<uint32_t>(cmds.size());
				for (int sec = 0; sec < 16; ++sec)
					if (ChunkStateProbe::slotIUsed(*chunk, sec) != 0)
					{
						slots.push_back({ChunkStateProbe::slotIOff(*chunk, sec),
						                 ChunkStateProbe::slotISz(*chunk, sec),
						                 ChunkStateProbe::slotIUsed(*chunk, sec)});
						totalIndices += ChunkStateProbe::slotIUsed(*chunk, sec);
					}
				std::sort(slots.begin(), slots.end(),
				          [](const Range &a, const Range &b) { return a.off < b.off; });
				for (const auto &d : cmds)
				{
					uint32_t pos = d.cmd.firstIndex * sizeof(uint32_t);
					const uint32_t end = pos + d.cmd.indexCount * sizeof(uint32_t);
					for (size_t k = 0; k < slots.size() && pos < end; ++k)
					{
						if (slots[k].off != pos)
							continue;
						if (pos + slots[k].used >= end)
						{
							pos = end;
							break;
						}
						if (slots[k].used != slots[k].sz)
							allValid = false;
						pos += slots[k].used;
					}
					if (pos != end)
						allValid = false;
				}
			}
			CHECK(allValid, "spawn: every bootstrap chunk's commands stay valid");
		}
		std::cerr << "  dbg spawn: chunks=" << totalChunks << " cmds=" << totalCmds
		          << " indexBytes=" << totalIndices << std::endl;
		CHECK(totalChunks > 0, "spawn: bootstrap chunks uploaded");

		// Second phase (PR review): neighbours' mirror edits re-mesh the
		// bootstrap chunks PARTIALLY (fresh ranges per touched section,
		// atomic slot swap). After those uploads the collected commands
		// must STILL be valid - no slack drawn, full coverage.
		for (int e = 0; e < 24; ++e)
		{
			const int cx = (e % 3) * CHUNK_SIZE;
			const int cz = ((e / 3) % 3) * CHUNK_SIZE;
			for (Chunk *chunk : manager4.getActiveChunks())
			{
				const glm::vec3 p = chunk->getPosition();
				if (std::abs(static_cast<int>(p.x) - cx) > 1 ||
				    std::abs(static_cast<int>(p.z) - cz) > 1)
					continue;
				const int lx = (e * 7) % CHUNK_SIZE;
				const int lz = (e * 11) % CHUNK_SIZE;
				int top = -1;
				for (int y = static_cast<int>(CHUNK_HEIGHT) - 1; y >= 0; --y)
					if (chunk->getVoxel(static_cast<uint32_t>(lx),
					                    static_cast<uint32_t>(y),
					                    static_cast<uint32_t>(lz))
					        .type != static_cast<uint8_t>(AIR))
					{
						top = y;
						break;
					}
				if (top > 1)
					chunk->setVoxel(lx, top, lz, BRICKS);
			}
		}
		for (Chunk *chunk : manager4.getActiveChunks())
		{
			if (chunk->dirtySections() == 0)
				continue;
			const uint16_t mask = chunk->takeDirtySections();
			MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
			chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), mask);
			chunk->getMeshResultPool()->finishBuild(r);
			CHECK(chunk->publishMeshResult(r), "phase2: partial result published");
		}
		staging.beginFrame(0);
		int uploaded = 0;
		for (Chunk *chunk : manager4.getActiveChunks())
			if (chunk->needsGPUUpload())
			{
				CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd,
				                              retire, arenas),
				      "phase2: partial upload");
				++uploaded;
			}
		CHECK(vk.flush(), "phase2 submit");
		std::cerr << "  dbg phase2: uploaded=" << uploaded << std::endl;

		allValid = true;
		uint32_t phase2Cmds = 0;
		for (Chunk *chunk : manager4.getActiveChunks())
		{
			if (!chunk || chunk->getOpaqueIndexCount() == 0)
				continue;
			struct Range2
			{
				uint32_t off, sz, used;
			};
			std::vector<Range2> slots;
			std::vector<Chunk::IndirectDraw> cmds;
			chunk->collectOpaqueDraws(cmds);
			phase2Cmds += static_cast<uint32_t>(cmds.size());
			for (int sec = 0; sec < 16; ++sec)
				if (ChunkStateProbe::slotIUsed(*chunk, sec) != 0)
					slots.push_back({ChunkStateProbe::slotIOff(*chunk, sec),
					                 ChunkStateProbe::slotISz(*chunk, sec),
					                 ChunkStateProbe::slotIUsed(*chunk, sec)});
			std::sort(slots.begin(), slots.end(),
			          [](const Range2 &a, const Range2 &b) { return a.off < b.off; });
			for (const auto &d : cmds)
			{
				uint32_t pos = d.cmd.firstIndex * sizeof(uint32_t);
				const uint32_t end = pos + d.cmd.indexCount * sizeof(uint32_t);
				for (size_t k = 0; k < slots.size() && pos < end; ++k)
				{
					if (slots[k].off != pos)
						continue;
					if (pos + slots[k].used >= end)
					{
						pos = end;
						break;
					}
					if (slots[k].used != slots[k].sz)
						allValid = false;
					pos += slots[k].used;
				}
				if (pos != end)
					allValid = false;
			}
		}
		CHECK(allValid, "phase2: commands valid after partial rebuilds");
		std::cerr << "  dbg phase2: cmds=" << phase2Cmds << std::endl;
	}

	// =========================================================================
	// Phase 11: In-flight partial remesh without queueWaitIdle (item 27)
	// =========================================================================
	{
		ChunkPool chunkPool(8);
		TerrainGenerator gen(101);
		ChunkManager manager(&gen, nullptr, &chunkPool);
		Camera cam(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(16);
		Chunk *chunk = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "inflight: chunk loaded");
		if (chunk)
		{
			CHECK(manager.prepareAndGenerateChunk(chunk, gen), "inflight: prep+gen");
			CHECK(chunk->generateMesh(), "inflight: initial mesh");
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
			      "inflight: initial upload");
			CHECK(vk.flush(), "inflight: submit frame 0");

			// Frame 0: find a section with non-empty mesh
			int sec = -1;
			for (int s = 0; s < 16; ++s)
				if (ChunkStateProbe::slotICount(*chunk, s) > 0)
				{
					sec = s;
					break;
				}
			CHECK(sec >= 0, "inflight: found section to edit");
			const uint32_t vPage0 = ChunkStateProbe::slotVPage(*chunk, sec);
			const uint32_t vOff0 = ChunkStateProbe::slotVOff(*chunk, sec);
			const uint32_t iPage0 = ChunkStateProbe::slotIPage(*chunk, sec);
			const uint32_t iOff0 = ChunkStateProbe::slotIOff(*chunk, sec);

			// Frame 1: edit same section, partial remesh, upload, submit (DO NOT WAIT IDLE)
			const int y0 = sec * 16;
			chunk->setVoxel(4, y0 + 4, 4, STONE);
			const uint16_t mask1 = chunk->takeDirtySections();
			CHECK((mask1 & (1u << sec)) != 0, "inflight: frame 1 dirty section");
			{
				MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
				chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), mask1);
				chunk->getMeshResultPool()->finishBuild(r);
				CHECK(chunk->publishMeshResult(r), "inflight: frame 1 mesh published");
			}
			staging.beginFrame(1);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
			      "inflight: frame 1 upload");
			CHECK(vk.flush(), "inflight: submit frame 1 (no wait idle)");

			const uint32_t vPage1 = ChunkStateProbe::slotVPage(*chunk, sec);
			const uint32_t vOff1 = ChunkStateProbe::slotVOff(*chunk, sec);
			const uint32_t iPage1 = ChunkStateProbe::slotIPage(*chunk, sec);
			const uint32_t iOff1 = ChunkStateProbe::slotIOff(*chunk, sec);
			CHECK(vPage1 != vPage0 || vOff1 != vOff0, "inflight: frame 1 fresh vertex range");
			CHECK(iPage1 != iPage0 || iOff1 != iOff0, "inflight: frame 1 fresh index range");

			// Frame 2: another edit in the same section, upload, submit (DO NOT WAIT IDLE)
			chunk->setVoxel(5, y0 + 5, 5, DIRT);
			const uint16_t mask2 = chunk->takeDirtySections();
			CHECK((mask2 & (1u << sec)) != 0, "inflight: frame 2 dirty section");
			{
				MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
				chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), mask2);
				chunk->getMeshResultPool()->finishBuild(r);
				CHECK(chunk->publishMeshResult(r), "inflight: frame 2 mesh published");
			}
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
			      "inflight: frame 2 upload");
			CHECK(vk.flush(), "inflight: submit frame 2 (no wait idle)");

			const uint32_t vPage2 = ChunkStateProbe::slotVPage(*chunk, sec);
			const uint32_t vOff2 = ChunkStateProbe::slotVOff(*chunk, sec);
			const uint32_t iPage2 = ChunkStateProbe::slotIPage(*chunk, sec);
			const uint32_t iOff2 = ChunkStateProbe::slotIOff(*chunk, sec);
			CHECK(vPage2 != vPage0 || vOff2 != vOff0, "inflight: frame 2 does not reuse frame 0 V");
			CHECK(iPage2 != iPage0 || iOff2 != iOff0, "inflight: frame 2 does not reuse frame 0 I");
			CHECK(vPage2 != vPage1 || vOff2 != vOff1, "inflight: frame 2 does not reuse frame 1 V");
			CHECK(iPage2 != iPage1 || iOff2 != iOff1, "inflight: frame 2 does not reuse frame 1 I");

			// Wait idle and tick retirement frames to verify reuse
			vkQueueWaitIdle(vk.queue);
			for (uint64_t f = 1; f <= 10; ++f)
			{
				arenas.beginFrame(f * 10);
				retire.beginFrame(f * 10);
			}
			retire.flush();
		}
	}

	// =========================================================================
	// Phase 12: LOD Transitions (items 28, 29, 30, 31)
	// =========================================================================
	{
		ChunkPool chunkPool(8);
		TerrainGenerator gen(202);
		ChunkManager manager(&gen, nullptr, &chunkPool);
		Camera cam(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(16);
		Chunk *chunk = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "lod: chunk loaded");
		if (chunk)
		{
			CHECK(manager.prepareAndGenerateChunk(chunk, gen), "lod: prep+gen");
			CHECK(chunk->generateMesh(), "lod: full mesh gen");
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
			      "lod: full upload");
			CHECK(vk.flush(), "lod: full submit");
			CHECK(!chunk->isLODMesh(), "lod: chunk starts in full-quality mesh");
			CHECK(chunk->getOpaqueIndexCount() > 0, "lod: full mesh has indices");

			// 28. Full -> LOD transition:
			{
				MeshBuildResult *lod = chunk->getMeshResultPool()->acquire();
				lod->isLOD = true;
				lod->owner = chunk;
				lod->generation = chunk->meshGeneration();
				lod->revision = chunk->meshRevision();
				Vertex v{};
				lod->opaqueVertices.assign(128, v);
				lod->opaqueIndices.assign(192, 0u);
				chunk->getMeshResultPool()->finishBuild(lod);
				CHECK(chunk->publishMeshResult(lod), "lod: publish LOD mesh");
				staging.beginFrame(0);
				CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				      "lod: upload LOD mesh");
				CHECK(vk.flush(), "lod: LOD submit");
				CHECK(chunk->isLODMesh(), "lod: chunk is now LOD mesh");
				CHECK(chunk->getOpaqueIndexCount() == 192, "lod: LOD index count matches");
				// Section slot table must be completely empty!
				bool allEmpty = true;
				for (int s = 0; s < 16; ++s)
					if (!ChunkStateProbe::slotEmpty(*chunk, s))
						allEmpty = false;
				CHECK(allEmpty, "lod: Full->LOD cleared all section slots");
				std::vector<Chunk::IndirectDraw> draws;
				CHECK(chunk->collectOpaqueDraws(draws) == 1, "lod: LOD yields exactly 1 draw command");
			}

			// 29. LOD -> Full transition:
			{
				MeshBuildResult *full = chunk->getMeshResultPool()->acquire();
				full->isLOD = false;
				chunk->buildMesh(*full, chunk->meshGeneration(), chunk->meshRevision(), kAllSectionMask);
				chunk->getMeshResultPool()->finishBuild(full);
				CHECK(chunk->publishMeshResult(full), "lod: publish full mesh after LOD");
				staging.beginFrame(0);
				CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				      "lod: upload full mesh after LOD");
				CHECK(vk.flush(), "lod: full submit after LOD");
				CHECK(!chunk->isLODMesh(), "lod: chunk transitioned back to full quality");
				CHECK(ChunkStateProbe::lodOpaqueV(*chunk).empty(), "lod: old LOD vertex range retired/cleared");
				CHECK(ChunkStateProbe::lodOpaqueI(*chunk).empty(), "lod: old LOD index range retired/cleared");
				std::vector<Chunk::IndirectDraw> draws;
				CHECK(chunk->collectOpaqueDraws(draws) > 1, "lod: full mesh yields multiple section draws");
			}

			// 30. LOD nonempty -> empty:
			{
				// First upload nonempty LOD with both opaque and water
				MeshBuildResult *lodNonEmpty = chunk->getMeshResultPool()->acquire();
				lodNonEmpty->isLOD = true;
				lodNonEmpty->owner = chunk;
				lodNonEmpty->generation = chunk->meshGeneration();
				lodNonEmpty->revision = chunk->meshRevision();
				Vertex v{};
				lodNonEmpty->opaqueVertices.assign(64, v);
				lodNonEmpty->opaqueIndices.assign(96, 0u);
				lodNonEmpty->waterVertices.assign(64, v);
				lodNonEmpty->waterIndices.assign(96, 0u);
				chunk->getMeshResultPool()->finishBuild(lodNonEmpty);
				CHECK(chunk->publishMeshResult(lodNonEmpty), "lod: publish nonempty LOD");
				staging.beginFrame(0);
				CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				      "lod: upload nonempty opaque+water LOD");
				CHECK(vk.flush(), "lod: nonempty submit");
				CHECK(chunk->getOpaqueIndexCount() == 96, "lod: opaque 96");
				CHECK(chunk->getWaterIndexCount() == 96, "lod: water 96");

				// Now upload empty LOD (opaque empty, water empty)
				MeshBuildResult *lodEmpty = chunk->getMeshResultPool()->acquire();
				lodEmpty->isLOD = true; // empty vertices & indices
				lodEmpty->owner = chunk;
				lodEmpty->generation = chunk->meshGeneration();
				lodEmpty->revision = chunk->meshRevision();
				chunk->getMeshResultPool()->finishBuild(lodEmpty);
				CHECK(chunk->publishMeshResult(lodEmpty), "lod: publish empty LOD");
				staging.beginFrame(0);
				CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				      "lod: upload empty LOD");
				CHECK(vk.flush(), "lod: empty LOD submit");
				CHECK(chunk->getOpaqueIndexCount() == 0, "lod: opaque empty -> 0");
				CHECK(chunk->getWaterIndexCount() == 0, "lod: water empty -> 0");
				CHECK(ChunkStateProbe::lodOpaqueV(*chunk).empty(), "lod: opaque V cleared");
				CHECK(ChunkStateProbe::lodOpaqueI(*chunk).empty(), "lod: opaque I cleared");
				CHECK(ChunkStateProbe::lodWaterV(*chunk).empty(), "lod: water V cleared");
				CHECK(ChunkStateProbe::lodWaterI(*chunk).empty(), "lod: water I cleared");
				std::vector<Chunk::IndirectDraw> draws;
				CHECK(chunk->collectOpaqueDraws(draws) == 0, "lod: collectOpaqueDraws is 0");
				CHECK(chunk->collectWaterDraws(draws) == 0, "lod: collectWaterDraws is 0");
			}

			// 31. Stress loop: 100x Full <-> LOD alternating
			{
				for (int iter = 0; iter < 100; ++iter)
				{
					// LOD upload
					MeshBuildResult *lod = chunk->getMeshResultPool()->acquire();
					lod->isLOD = true;
					lod->owner = chunk;
					lod->generation = chunk->meshGeneration();
					lod->revision = chunk->meshRevision();
					Vertex v{};
					lod->opaqueVertices.assign(128, v);
					lod->opaqueIndices.assign(192, 0u);
					chunk->getMeshResultPool()->finishBuild(lod);
					chunk->publishMeshResult(lod);
					staging.beginFrame(0);
					chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas);

					// Full upload
					MeshBuildResult *full = chunk->getMeshResultPool()->acquire();
					full->isLOD = false;
					chunk->buildMesh(*full, chunk->meshGeneration(), chunk->meshRevision(), kAllSectionMask);
					chunk->getMeshResultPool()->finishBuild(full);
					chunk->publishMeshResult(full);
					staging.beginFrame(0);
					chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas);

					// Advance frame periodically to retire expired ranges
					arenas.beginFrame(1000 + iter);
					retire.beginFrame(1000 + iter);
				}
				CHECK(vk.flush(), "lod: stress loop flush");
				vkQueueWaitIdle(vk.queue);
				for (uint64_t f = 1; f <= 10; ++f)
				{
					arenas.beginFrame(2000 + f);
					retire.beginFrame(2000 + f);
				}
				retire.flush();
				// Ensure no runaway memory growth
				CHECK(arenas.opaqueVertex.metrics().pages <= 4, "lod: pages stabilized after 100x stress loop");
				CHECK(arenas.opaqueIndex.metrics().pages <= 4, "lod: index pages stabilized after 100x stress loop");
			}
		}
	}

	// =========================================================================
	// Phase 13: Transaction rollback on water failure (item 32)
	// =========================================================================
	{
		ChunkPool chunkPool(8);
		TerrainGenerator gen(303);
		ChunkManager manager(&gen, nullptr, &chunkPool);
		Camera cam(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(16);
		Chunk *chunk = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "trans: chunk loaded");
		if (chunk)
		{
			CHECK(manager.prepareAndGenerateChunk(chunk, gen), "trans: prep+gen");
			CHECK(chunk->generateMesh(), "trans: initial mesh");
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
			      "trans: initial upload");
			CHECK(vk.flush(), "trans: submit initial");

			// Create a fresh mesh result with BOTH opaque and water
			MeshBuildResult *res = chunk->getMeshResultPool()->acquire();
			chunk->buildMesh(*res, chunk->meshGeneration(), chunk->meshRevision(), kAllSectionMask);
			Vertex v{};
			res->sections[0].waterVertices.assign(64, v);
			res->sections[0].waterIndices.assign(96, 0u);
			res->sectionsBuilt |= 1u;
			chunk->getMeshResultPool()->finishBuild(res);
			CHECK(chunk->publishMeshResult(res), "trans: mesh with water published");

			// Snapshot current chunk state
			const uint32_t snapOpaqueCount = chunk->getOpaqueIndexCount();
			const uint32_t snapWaterCount = chunk->getWaterIndexCount();
			const bool snapIsLOD = chunk->isLODMesh();
			uint32_t snapOpaqueSlots[16][4];
			uint32_t snapWaterSlots[16][4];
			for (int s = 0; s < 16; ++s)
			{
				snapOpaqueSlots[s][0] = ChunkStateProbe::slotVPage(*chunk, s);
				snapOpaqueSlots[s][1] = ChunkStateProbe::slotVOff(*chunk, s);
				snapOpaqueSlots[s][2] = ChunkStateProbe::slotIPage(*chunk, s);
				snapOpaqueSlots[s][3] = ChunkStateProbe::slotIOff(*chunk, s);

				snapWaterSlots[s][0] = ChunkStateProbe::slotWVPage(*chunk, s);
				snapWaterSlots[s][1] = ChunkStateProbe::slotWVOff(*chunk, s);
				snapWaterSlots[s][2] = ChunkStateProbe::slotWIPage(*chunk, s);
				snapWaterSlots[s][3] = ChunkStateProbe::slotWIOff(*chunk, s);
			}
			const auto metricsOVBefore = arenas.opaqueVertex.metrics();
			const auto metricsOIBefore = arenas.opaqueIndex.metrics();
			const auto metricsWVBefore = arenas.waterVertex.metrics();
			const auto metricsWIBefore = arenas.waterIndex.metrics();

			// Inject failure on waterIndex arena allocation!
			// Opaque vertex and index allocations will succeed, then waterIndex fails.
			arenas.waterIndex.setFailNextAllocations(1);

			staging.beginFrame(0);
			const bool uploaded = chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas);
			CHECK(!uploaded, "trans: upload failed when water allocation failed");
			CHECK(ChunkStateProbe::pending(*chunk) == res, "trans: pending result attached (survives rollback)");
			CHECK(chunk->needsGPUUpload(), "trans: meshNeedsUpdate remains armed");
			CHECK(chunk->getOpaqueIndexCount() == snapOpaqueCount, "trans: opaqueIndexCount identical");
			CHECK(chunk->getWaterIndexCount() == snapWaterCount, "trans: waterIndexCount identical");
			CHECK(chunk->isLODMesh() == snapIsLOD, "trans: isLODMesh identical");

			// Verify every section slot in both streams is byte-for-byte identical
			bool slotsMatch = true;
			for (int s = 0; s < 16; ++s)
			{
				if (ChunkStateProbe::slotVPage(*chunk, s) != snapOpaqueSlots[s][0] ||
				    ChunkStateProbe::slotVOff(*chunk, s) != snapOpaqueSlots[s][1] ||
				    ChunkStateProbe::slotIPage(*chunk, s) != snapOpaqueSlots[s][2] ||
				    ChunkStateProbe::slotIOff(*chunk, s) != snapOpaqueSlots[s][3] ||
				    ChunkStateProbe::slotWVPage(*chunk, s) != snapWaterSlots[s][0] ||
				    ChunkStateProbe::slotWVOff(*chunk, s) != snapWaterSlots[s][1] ||
				    ChunkStateProbe::slotWIPage(*chunk, s) != snapWaterSlots[s][2] ||
				    ChunkStateProbe::slotWIOff(*chunk, s) != snapWaterSlots[s][3])
					slotsMatch = false;
			}
			CHECK(slotsMatch, "trans: every section slot is byte-for-byte identical after rollback");

			// Verify arena metrics are identical (all rolled back ranges freed)
			CHECK(arenas.opaqueVertex.metrics().liveBlocks == metricsOVBefore.liveBlocks,
			      "trans: opaque vertex live blocks rolled back");
			CHECK(arenas.opaqueIndex.metrics().liveBlocks == metricsOIBefore.liveBlocks,
			      "trans: opaque index live blocks rolled back");
			CHECK(arenas.waterVertex.metrics().liveBlocks == metricsWVBefore.liveBlocks,
			      "trans: water vertex live blocks rolled back");
			CHECK(arenas.waterIndex.metrics().liveBlocks == metricsWIBefore.liveBlocks,
			      "trans: water index live blocks rolled back");

			// Retry without failure injection -> succeeds!
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
			      "trans: retry after failure succeeds");
			CHECK(vk.flush(), "trans: retry submit");
		}
	}

	// =========================================================================
	// Phase 14: synchronous bootstrap failure is loud and clean
	//
	// The bootstrap path (uploadToGPU) cannot retry silently: an arena OOM
	// must throw WITHOUT leaking partially-allocated ranges, WITHOUT
	// publishing slots or draw counts, and WITHOUT consuming the pending CPU
	// result. Covered scenarios:
	//   A. full-quality bootstrap: first allocation fails
	//   B. LOD bootstrap: opaque index fails after the vertex range succeeded
	//   C. LOD bootstrap: water fails AFTER opaque uploaded fully (the whole
	//      LOD transaction rolls back)
	// =========================================================================
	{
		ChunkPool chunkPool(8);
		TerrainGenerator gen(404);
		ChunkManager manager(&gen, nullptr, &chunkPool);
		Camera cam(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(16);
		Chunk *chunk = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "bootfail A: chunk loaded");
		if (chunk)
		{
			CHECK(manager.prepareAndGenerateChunk(chunk, gen), "bootfail A: prep+gen");
			CHECK(chunk->generateMesh(), "bootfail A: initial mesh");
			CHECK(ChunkStateProbe::pending(*chunk) != nullptr, "bootfail A: pending attached");

			const auto ovM = arenas.opaqueVertex.metrics();
			const auto oiM = arenas.opaqueIndex.metrics();
			const auto wvM = arenas.waterVertex.metrics();
			const auto wiM = arenas.waterIndex.metrics();

			arenas.opaqueVertex.setFailNextAllocations(1);
			bool threw = false;
			try
			{
				chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);
			}
			catch (const std::runtime_error &)
			{
				threw = true;
			}
			CHECK(threw, "bootfail A: full-quality OOM throws (never silently ignored)");
			CHECK(ChunkStateProbe::pending(*chunk) != nullptr,
			      "bootfail A: pending result NOT consumed by the failure");
			CHECK(chunk->needsGPUUpload(), "bootfail A: meshNeedsUpdate stays armed");
			CHECK(chunk->getOpaqueIndexCount() == 0 && chunk->getWaterIndexCount() == 0,
			      "bootfail A: no phantom draw count");
			bool allEmpty = true;
			for (int s = 0; s < 16; ++s)
				allEmpty = allEmpty && ChunkStateProbe::slotEmpty(*chunk, s) &&
				           ChunkStateProbe::slotWaterEmpty(*chunk, s);
			CHECK(allEmpty, "bootfail A: no section slot published");
			CHECK(arenas.opaqueVertex.metrics().liveBlocks == ovM.liveBlocks &&
			      arenas.opaqueVertex.metrics().liveBytes == ovM.liveBytes &&
			      arenas.opaqueIndex.metrics().liveBlocks == oiM.liveBlocks &&
			      arenas.opaqueIndex.metrics().liveBytes == oiM.liveBytes &&
			      arenas.waterVertex.metrics().liveBlocks == wvM.liveBlocks &&
			      arenas.waterIndex.metrics().liveBytes == wiM.liveBytes,
			      "bootfail A: arena counters untouched (nothing leaked)");

			// Recovery: the same bootstrap succeeds once the hook is consumed.
			threw = false;
			try
			{
				chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);
			}
			catch (const std::runtime_error &)
			{
				threw = true;
			}
			CHECK(!threw, "bootfail A: retry succeeds");
			CHECK(ChunkStateProbe::pending(*chunk) == nullptr,
			      "bootfail A: retry consumed the result");
			CHECK(chunk->getOpaqueIndexCount() > 0, "bootfail A: retry published opaque geometry");
		}
	}

	{
		ChunkPool chunkPool(8);
		TerrainGenerator gen(505);
		ChunkManager manager(&gen, nullptr, &chunkPool);
		Camera cam(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(16);
		Chunk *chunk = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "bootfail B: chunk loaded");
		if (chunk)
		{
			// Pure LOD bootstrap: the chunk was generated but never uploaded;
			// the LOD result replaces the never-uploaded full result.
			CHECK(manager.prepareAndGenerateChunk(chunk, gen), "bootfail B: prep+gen");
			MeshBuildResult *lod = chunk->getMeshResultPool()->acquire();
			lod->isLOD = true;
			lod->owner = chunk;
			lod->generation = chunk->meshGeneration();
			lod->revision = chunk->meshRevision();
			Vertex v{};
			lod->opaqueVertices.assign(128, v);
			lod->opaqueIndices.assign(192, 0u);
			lod->waterVertices.assign(32, v);
			lod->waterIndices.assign(48, 0u);
			chunk->getMeshResultPool()->finishBuild(lod);
			CHECK(chunk->publishMeshResult(lod), "bootfail B: LOD result published");

			const auto ovM = arenas.opaqueVertex.metrics();
			const auto oiM = arenas.opaqueIndex.metrics();
			const auto wvM = arenas.waterVertex.metrics();
			const auto wiM = arenas.waterIndex.metrics();

			arenas.opaqueIndex.setFailNextAllocations(1);
			bool threw = false;
			try
			{
				chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);
			}
			catch (const std::runtime_error &)
			{
				threw = true;
			}
			CHECK(threw, "bootfail B: LOD opaque-index OOM throws");
			CHECK(ChunkStateProbe::pending(*chunk) == lod,
			      "bootfail B: pending LOD result retained");
			CHECK(ChunkStateProbe::lodOpaqueV(*chunk).empty() &&
			      ChunkStateProbe::lodOpaqueI(*chunk).empty(),
			      "bootfail B: partial opaque pair rolled back (no leaked vertex range)");
			CHECK(ChunkStateProbe::lodWaterV(*chunk).empty() &&
			      ChunkStateProbe::lodWaterI(*chunk).empty(),
			      "bootfail B: water ranges untouched");
			CHECK(chunk->getOpaqueIndexCount() == 0 && chunk->getWaterIndexCount() == 0,
			      "bootfail B: no phantom LOD counts");
			CHECK(arenas.opaqueVertex.metrics().liveBlocks == ovM.liveBlocks &&
			      arenas.opaqueVertex.metrics().liveBytes == ovM.liveBytes &&
			      arenas.opaqueIndex.metrics().liveBlocks == oiM.liveBlocks &&
			      arenas.opaqueIndex.metrics().liveBytes == oiM.liveBytes,
			      "bootfail B: opaque counters untouched after pair rollback");

			// Recovery.
			threw = false;
			try
			{
				chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);
			}
			catch (const std::runtime_error &)
			{
				threw = true;
			}
			CHECK(!threw, "bootfail B: retry succeeds");
			CHECK(chunk->isLODMesh(), "bootfail B: retry flagged LOD");
			CHECK(chunk->getOpaqueIndexCount() == 192 && chunk->getWaterIndexCount() == 48,
			      "bootfail B: retry published both streams");
			CHECK(!ChunkStateProbe::lodOpaqueV(*chunk).empty() &&
			      !ChunkStateProbe::lodWaterI(*chunk).empty(),
			      "bootfail B: retry holds LOD ranges");
			CHECK(ChunkStateProbe::pending(*chunk) == nullptr,
			      "bootfail B: retry consumed the result");
		}
	}

	{
		ChunkPool chunkPool(8);
		TerrainGenerator gen(606);
		ChunkManager manager(&gen, nullptr, &chunkPool);
		Camera cam(glm::vec3(0.0f, 100.0f, 0.0f));
		RenderSettings settings;
		manager.updateStreaming(cam, settings);
		manager.processChunkLoading(16);
		Chunk *chunk = manager.getChunkAtWorldPos(glm::vec3(4.0f, 40.0f, 4.0f));
		CHECK(chunk != nullptr, "bootfail C: chunk loaded");
		if (chunk)
		{
			CHECK(manager.prepareAndGenerateChunk(chunk, gen), "bootfail C: prep+gen");
			MeshBuildResult *lod = chunk->getMeshResultPool()->acquire();
			lod->isLOD = true;
			lod->owner = chunk;
			lod->generation = chunk->meshGeneration();
			lod->revision = chunk->meshRevision();
			Vertex v{};
			lod->opaqueVertices.assign(96, v);
			lod->opaqueIndices.assign(144, 0u);
			lod->waterVertices.assign(64, v);
			lod->waterIndices.assign(96, 0u);
			chunk->getMeshResultPool()->finishBuild(lod);
			CHECK(chunk->publishMeshResult(lod), "bootfail C: LOD result published");

			const auto ovM = arenas.opaqueVertex.metrics();
			const auto oiM = arenas.opaqueIndex.metrics();
			const auto wvM = arenas.waterVertex.metrics();
			const auto wiM = arenas.waterIndex.metrics();

			// Water fails AFTER opaque allocated AND uploaded: the whole LOD
			// upload is one transaction, so the opaque success must not
			// survive the throw.
			arenas.waterVertex.setFailNextAllocations(1);
			bool threw = false;
			try
			{
				chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);
			}
			catch (const std::runtime_error &)
			{
				threw = true;
			}
			CHECK(threw, "bootfail C: LOD water OOM throws after opaque success");
			CHECK(ChunkStateProbe::pending(*chunk) == lod,
			      "bootfail C: pending LOD result retained");
			CHECK(ChunkStateProbe::lodOpaqueV(*chunk).empty() &&
			      ChunkStateProbe::lodOpaqueI(*chunk).empty() &&
			      ChunkStateProbe::lodWaterV(*chunk).empty() &&
			      ChunkStateProbe::lodWaterI(*chunk).empty(),
			      "bootfail C: whole transaction rolled back (uploaded opaque pair too)");
			CHECK(chunk->getOpaqueIndexCount() == 0 && chunk->getWaterIndexCount() == 0,
			      "bootfail C: no stale draw counts after rollback");
			CHECK(arenas.opaqueVertex.metrics().liveBlocks == ovM.liveBlocks &&
			      arenas.opaqueVertex.metrics().liveBytes == ovM.liveBytes &&
			      arenas.opaqueIndex.metrics().liveBlocks == oiM.liveBlocks &&
			      arenas.opaqueIndex.metrics().liveBytes == oiM.liveBytes &&
			      arenas.waterVertex.metrics().liveBlocks == wvM.liveBlocks &&
			      arenas.waterIndex.metrics().liveBytes == wiM.liveBytes,
			      "bootfail C: no leaked range despite opaque upload success");

			// Recovery.
			threw = false;
			try
			{
				chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);
			}
			catch (const std::runtime_error &)
			{
				threw = true;
			}
			CHECK(!threw, "bootfail C: retry succeeds");
			CHECK(chunk->getOpaqueIndexCount() == 144 && chunk->getWaterIndexCount() == 96,
			      "bootfail C: retry published both streams");
		}

		// -----------------------------------------------------------------
		// 15. Indirect Draw Descriptor Cache (issue #109 / issue #122)
		// -----------------------------------------------------------------
		{
			Chunk *chunk = chunkPool.acquire(glm::vec3(0.0f, 0.0f, 0.0f));
			CHECK(chunk->getCachedOpaqueDrawCount() == 0, "cache: fresh chunk has 0 cached opaque draws");
			CHECK(chunk->getCachedWaterDrawCount() == 0, "cache: fresh chunk has 0 cached water draws");

			CHECK(manager.prepareAndGenerateChunk(chunk, gen), "cache: prepare+generate");
			CHECK(chunk->generateMesh(), "cache: mesh publishes");
			chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);

			const uint32_t opCount = chunk->getCachedOpaqueDrawCount();
			const uint32_t wtCount = chunk->getCachedWaterDrawCount();
			CHECK(opCount > 0, "cache: full mesh populates cached opaque draws");

			std::vector<Chunk::IndirectDraw> opDraws1, opDraws2;
			size_t collected1 = chunk->collectOpaqueDraws(opDraws1);
			CHECK(collected1 == opCount, "cache: collectOpaqueDraws matches cached count");
			CHECK(opDraws1.size() == opCount, "cache: collectOpaqueDraws output size matches");

			// Multiple collections without upload yield identical results
			size_t collected2 = chunk->collectOpaqueDraws(opDraws2);
			CHECK(collected2 == opCount && opDraws2.size() == opCount, "cache: second collection matches");
			for (size_t i = 0; i < opCount; ++i)
			{
				CHECK(opDraws1[i].cmd.indexCount == opDraws2[i].cmd.indexCount, "cache: draw indexCount identical");
				CHECK(opDraws1[i].cmd.firstIndex == opDraws2[i].cmd.firstIndex, "cache: draw firstIndex identical");
				CHECK(opDraws1[i].cmd.vertexOffset == opDraws2[i].cmd.vertexOffset, "cache: draw vertexOffset identical");
				CHECK(opDraws1[i].vertexPage == opDraws2[i].vertexPage, "cache: draw vertexPage identical");
				CHECK(opDraws1[i].indexPage == opDraws2[i].indexPage, "cache: draw indexPage identical");
			}

			// Move constructor preserves cache
			Chunk moved(std::move(*chunk));
			CHECK(moved.getCachedOpaqueDrawCount() == opCount, "cache: move ctor transfers cached opaque count");
			CHECK(moved.getCachedWaterDrawCount() == wtCount, "cache: move ctor transfers cached water count");
			CHECK(chunk->getCachedOpaqueDrawCount() == 0, "cache: move ctor zeroes source opaque count");
			CHECK(chunk->getCachedWaterDrawCount() == 0, "cache: move ctor zeroes source water count");

			std::vector<Chunk::IndirectDraw> movedDraws;
			CHECK(moved.collectOpaqueDraws(movedDraws) == opCount, "cache: moved chunk collects correctly");

			// Move assignment preserves cache
			Chunk assigned(glm::vec3(16.0f, 0.0f, 0.0f));
			assigned = std::move(moved);
			CHECK(assigned.getCachedOpaqueDrawCount() == opCount, "cache: move assign transfers cached count");
			CHECK(moved.getCachedOpaqueDrawCount() == 0, "cache: move assign zeroes source count");

			// Release resets cache
			assigned.releaseGPU();
			CHECK(assigned.getCachedOpaqueDrawCount() == 0, "cache: releaseGPU zeroes cached opaque count");
			CHECK(assigned.getCachedWaterDrawCount() == 0, "cache: releaseGPU zeroes cached water count");
			std::vector<Chunk::IndirectDraw> emptyDraws;
			CHECK(assigned.collectOpaqueDraws(emptyDraws) == 0, "cache: collection on released chunk is empty");

			chunkPool.release(chunk);
		}

		const auto sameDraws = [](const std::vector<Chunk::IndirectDraw> &a,
		                          const std::vector<Chunk::IndirectDraw> &b) {
			if (a.size() != b.size())
				return false;
			for (size_t i = 0; i < a.size(); ++i)
				if (a[i].cmd.indexCount != b[i].cmd.indexCount ||
				    a[i].cmd.firstIndex != b[i].cmd.firstIndex ||
				    a[i].cmd.vertexOffset != b[i].cmd.vertexOffset ||
				    a[i].vertexPage != b[i].vertexPage ||
				    a[i].indexPage != b[i].indexPage)
					return false;
			return true;
		};

		// -----------------------------------------------------------------
		// 16. Renderable-cache invariant across pending uploads (issue
		// #177): stale-until-replaced. A committed GPU mesh stays
		// renderable while meshNeedsUpdate only marks a pending
		// replacement — hiding it would blink the whole chunk for the
		// remesh/upload window. Only a successful commit swaps the drawn
		// mesh, upload back-pressure keeps the old mesh drawable, and a
		// chunk with no committed mesh yet (first upload pending) stays
		// non-renderable.
		// -----------------------------------------------------------------
		{
			// First-upload lifecycle: meshed but never uploaded - cache
			// and index counters are still zero, so the committed-mesh
			// predicate must not report renderability.
			{
				Chunk *fresh = chunkPool.acquire(glm::vec3(0.0f, 0.0f, 0.0f));
				CHECK(manager.prepareAndGenerateChunk(fresh, gen), "first-upload: prepare+generate");
				CHECK(fresh->generateMesh(), "first-upload: mesh publishes");
				CHECK(fresh->needsGPUUpload(), "first-upload: mesh awaits its GPU commit");
				CHECK(fresh->getCachedOpaqueDrawCount() == 0 && fresh->getOpaqueIndexCount() == 0,
				      "first-upload: no committed opaque mesh yet");
				CHECK(fresh->getCachedWaterDrawCount() == 0 && fresh->getWaterIndexCount() == 0,
				      "first-upload: no committed water mesh yet");
				CHECK(!fresh->hasRenderableOpaqueDraws(),
				      "first-upload: no committed mesh is not renderable (opaque)");
				CHECK(!fresh->hasRenderableWaterDraws(),
				      "first-upload: no committed mesh is not renderable (water)");
				std::vector<Chunk::IndirectDraw> freshDraws;
				CHECK(fresh->collectOpaqueDraws(freshDraws) == 0,
				      "first-upload: no opaque draws collected");
				CHECK(fresh->collectWaterDraws(freshDraws) == 0,
				      "first-upload: no water draws collected");
				chunkPool.release(fresh);
			}

			Chunk *chunk = chunkPool.acquire(glm::vec3(0.0f, 0.0f, 0.0f));
			CHECK(manager.prepareAndGenerateChunk(chunk, gen), "invariant: prepare+generate");
			CHECK(chunk->generateMesh(), "invariant: mesh publishes");
			chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);
			CHECK(chunk->hasRenderableOpaqueDraws(), "invariant: uploaded chunk is renderable");

			std::vector<Chunk::IndirectDraw> committed;
			const size_t before = chunk->collectOpaqueDraws(committed);
			CHECK(before > 0, "invariant: committed draws collectable");

			// Edit the chunk: meshNeedsUpdate arms, but the committed GPU
			// mesh stays fully renderable (stale-until-replaced).
			CHECK(chunk->placeVoxel(glm::vec3(5.f, 140.f, 5.f), STONE),
			      "invariant: edit accepted above surface");
			CHECK(chunk->needsGPUUpload(), "invariant: edit raises needsGPUUpload");
			CHECK(chunk->hasRenderableOpaqueDraws(),
			      "invariant: committed opaque mesh stays renderable while replacement is pending");
			{
				std::vector<Chunk::IndirectDraw> pending;
				CHECK(chunk->collectOpaqueDraws(pending) == before,
				      "invariant: old opaque draws collectable while pending");
				CHECK(sameDraws(pending, committed),
				      "invariant: pending collection still describes the committed mesh");
			}
			// Raw counters stay cached (the data is stale, not gone).
			CHECK(chunk->getCachedOpaqueDrawCount() == before,
			      "invariant: stale cache retained while pending");

			// Replacement ready + staging full: back-pressure keeps the
			// old mesh renderable for as many frames as it takes.
			CHECK(chunk->generateMesh(), "invariant: remesh publishes");
			CHECK(chunk->needsGPUUpload(), "invariant: replacement result awaits its commit");
			{
				staging.beginFrame(0);
				VkDeviceSize off = 0;
				void *sink = nullptr;
				CHECK(staging.alloc(staging.sliceCapacity(), off, sink),
				      "invariant: staging ring drained");
				std::vector<Chunk::IndirectDraw> deferred;
				CHECK(!chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				      "invariant: staging-full defers the replacement upload");
				CHECK(chunk->needsGPUUpload(), "invariant: replacement still pending after deferral");
				CHECK(chunk->hasRenderableOpaqueDraws(),
				      "invariant: old mesh renderable during upload back-pressure");
				CHECK(chunk->collectOpaqueDraws(deferred) == before && sameDraws(deferred, committed),
				      "invariant: old draws collectable during upload back-pressure");
			}

			// Successful retry commits the replacement atomically.
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
			      "invariant: retry commits the replacement");
			CHECK(!chunk->needsGPUUpload(), "invariant: commit clears needsGPUUpload");
			CHECK(chunk->hasRenderableOpaqueDraws(), "invariant: opaque renderable after commit");
			{
				std::vector<Chunk::IndirectDraw> replaced;
				CHECK(chunk->collectOpaqueDraws(replaced) > 0,
				      "invariant: opaque draws collectable after commit");
				CHECK(!sameDraws(replaced, committed),
				      "invariant: committed descriptors switched to the replacement mesh");
			}
			CHECK(vk.flush(), "invariant: replacement submit");
			retire.flush();

			chunkPool.release(chunk);
		}

		// -----------------------------------------------------------------
		// 17. Deterministic water stale-renderable (issue #177 review): the
		// water contract must not depend on the bootstrap terrain holding
		// water. Water geometry is built explicitly, then a second edit
		// proves the committed water mesh stays renderable with descriptor
		// stability through a pending remesh AND a staging-full deferral,
		// and that the retry commits replacement water descriptors.
		// -----------------------------------------------------------------
		{
			Chunk *chunk = chunkPool.acquire(glm::vec3(0.0f, 0.0f, 0.0f));
			CHECK(manager.prepareAndGenerateChunk(chunk, gen), "water: prepare+generate");
			CHECK(chunk->generateMesh(), "water: mesh publishes");
			chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);

			// Build an explicit floating water voxel and commit it: this is
			// the committed "water mesh A" the rest of the section uses (the
			// seed's terrain may also carry natural water - it is part of
			// the same committed snapshot either way).
			CHECK(chunk->placeVoxel(glm::vec3(5.0f, 140.0f, 5.0f), WATER),
			      "water: initial water voxel placed");
			CHECK(chunk->generateMesh(), "water: water mesh A publishes");
			chunk->uploadToGPU(vk.allocator.handle(), imm, arenas);
			CHECK(chunk->hasRenderableWaterDraws(), "water: committed water mesh is renderable");
			std::vector<Chunk::IndirectDraw> waterA;
			const size_t waterBefore = chunk->collectWaterDraws(waterA);
			CHECK(waterBefore > 0, "water: committed water draws collectable");
			std::vector<Chunk::IndirectDraw> opaqueA;
			CHECK(chunk->collectOpaqueDraws(opaqueA) > 0, "water: committed opaque draws collectable");

			// Second, adjacent water voxel: remesh pending, and the
			// committed water mesh must stay renderable with the exact old
			// descriptors (the shared water face only changes at commit).
			CHECK(chunk->placeVoxel(glm::vec3(6.0f, 140.0f, 5.0f), WATER),
			      "water: adjacent water voxel placed");
			CHECK(chunk->needsGPUUpload(), "water: edit raises needsGPUUpload");
			CHECK(chunk->hasRenderableWaterDraws(),
			      "water: committed water mesh stays renderable while replacement is pending");
			CHECK(chunk->hasRenderableOpaqueDraws(),
			      "water: committed opaque mesh stays renderable while replacement is pending");
			{
				std::vector<Chunk::IndirectDraw> pendingWater;
				CHECK(chunk->collectWaterDraws(pendingWater) == waterBefore,
				      "water: old water draws collectable while pending");
				CHECK(sameDraws(pendingWater, waterA),
				      "water: pending collection still describes the committed water mesh");
			}

			// Publish the replacement remesh, then starve the staging ring:
			// back-pressure keeps the committed water mesh renderable and
			// descriptor-stable for as long as the upload cannot commit.
			CHECK(chunk->generateMesh(), "water: replacement remesh publishes");
			{
				staging.beginFrame(0);
				VkDeviceSize off = 0;
				void *sink = nullptr;
				CHECK(staging.alloc(staging.sliceCapacity(), off, sink),
				      "water: staging ring drained");
				std::vector<Chunk::IndirectDraw> deferredWater;
				CHECK(!chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
				      "water: staging-full defers the replacement upload");
				CHECK(chunk->hasRenderableWaterDraws(),
				      "water: committed water mesh renderable during upload back-pressure");
				CHECK(chunk->collectWaterDraws(deferredWater) == waterBefore &&
				          sameDraws(deferredWater, waterA),
				      "water: old water descriptors intact during back-pressure");
			}

			// Retry: the replacement commits and replaces the water mesh.
			staging.beginFrame(0);
			CHECK(chunk->uploadToGPUAsync(vk.allocator.handle(), staging, vk.cmd, retire, arenas),
			      "water: retry commits the replacement");
			CHECK(!chunk->needsGPUUpload(), "water: commit clears needsGPUUpload");
			CHECK(chunk->hasRenderableWaterDraws(), "water: water renderable after commit");
			{
				std::vector<Chunk::IndirectDraw> waterB;
				CHECK(chunk->collectWaterDraws(waterB) > 0, "water: water draws collectable after commit");
				CHECK(!sameDraws(waterB, waterA),
				      "water: committed descriptors switched to the replacement water mesh");
			}
			CHECK(vk.flush(), "water: replacement submit");
			retire.flush();

			chunkPool.release(chunk);
		}

		// -----------------------------------------------------------------
		// 18. Cross-chunk border edit with geometric commit groups (issue
		// #177 review round 2): a border edit re-arms BOTH the target and
		// the shell-mirror neighbor, and their GPU publications now commit
		// ATOMICALLY - the group prepares all members, then commits all.
		// Each member COPY consumes one budget unit. "new A + old B" (the seam the
		// round-1 test demonstrated) is therefore impossible. The harness
		// simulates real frames: every upload call is preceded by
		// beginFrame(slot % framesInFlight) and followed by a submit, so
		// staged data is never overwritten before its command buffer runs.
		// -----------------------------------------------------------------
		{
			ChunkPool poolX(32);
			TerrainGenerator genX(42);
			ChunkManager managerX(&genX, nullptr, &poolX);
			managerX.generateInitialArea(glm::vec3(0.0f, 0.0f, 0.0f), 1,
			                             vk.allocator.handle(), imm, arenas);
			StagingRing stagingX;
			stagingX.init(vk.allocator.handle(), 2); // frames-in-flight slices
			Chunk *A = managerX.getChunk(glm::ivec3(0, 0, 0));
			Chunk *B = managerX.getChunk(glm::ivec3(1, 0, 0));
			Chunk *S = managerX.getChunk(glm::ivec3(0, 0, 1)); // south: corner-edit member
			CHECK(A != nullptr && B != nullptr && S != nullptr,
			      "xchunk: adjacent chunks bootstrapped");
			if (A && B && S)
			{
				CHECK(A->hasRenderableOpaqueDraws() && B->hasRenderableOpaqueDraws(),
				      "xchunk: bootstrap meshes renderable");

				// Surface height of the deepest involved column; the
				// fixture floats above it so every target is air on both
				// sides of the border.
				const auto columnSurface = [](Chunk *chunk, int lx, int lz) {
					for (int y = static_cast<int>(CHUNK_HEIGHT) - 1; y >= 0; --y)
						if (chunk->getVoxel(static_cast<uint32_t>(lx),
						                    static_cast<uint32_t>(y),
						                    static_cast<uint32_t>(lz))
						        .type != static_cast<uint8_t>(AIR))
							return y;
					return -1;
				};
				int hMax = -1;
				for (int lz = 7; lz <= 9; ++lz)
					hMax = std::max(hMax, columnSurface(A, 15, lz));
				hMax = std::max(hMax, columnSurface(B, 0, 8));
				CHECK(hMax > 1 && hMax + 5 < static_cast<int>(CHUNK_HEIGHT),
				      "xchunk: border columns surfaced");
				const int y0 = hMax + 3;

				const auto worldOf = [](Chunk *chunk, int lx, int ly, int lz) {
					const glm::vec3 p = chunk->getPosition();
					return glm::vec3(p.x + static_cast<float>(lx) + 0.5f,
					                 static_cast<float>(ly) + 0.5f,
					                 p.z + static_cast<float>(lz) + 0.5f);
				};
				const auto remeshOne = [](Chunk *chunk) {
					const uint16_t mask = chunk->takeDirtySections();
					if (mask == 0)
						return;
					MeshBuildResult *r = chunk->getMeshResultPool()->acquire();
					chunk->buildMesh(*r, chunk->meshGeneration(), chunk->meshRevision(), mask);
					chunk->getMeshResultPool()->finishBuild(r);
					CHECK(chunk->publishMeshResult(r), "xchunk: remesh published");
				};
				uint32_t frameSlot = 0;
				const auto beginFrame = [&]() {
					const uint64_t frameNumber = 10000u + frameSlot;
					retire.beginFrame(frameNumber);
					arenas.beginFrame(frameNumber);
					stagingX.beginFrame(frameSlot % 2);
				};
				const auto submitFrame = [&]() {
					if (!vk.flush())
					{
						std::cerr << "FLUSHFAIL slot=" << frameSlot << std::endl;
					}
					retire.flush();
					++frameSlot;
				};

				// Fixture (placed through the manager so both border shells
				// update): the tested center voxel on A's x=15 column, the
				// solid BRICKS across the border on B's x=0 column whose -x
				// face the removal will expose, and four isolation stones
				// culling every OTHER candidate face in B's border plane so
				// the later commit delta is exactly one quad (greedy
				// merge-proof). Every placement is a border edit: each
				// registers its own commit group.
				CHECK(managerX.placeVoxel(worldOf(B, 0, y0, 8), BRICKS), "xchunk: B border brick");
				CHECK(managerX.placeVoxel(worldOf(A, 15, y0 - 1, 8), BRICKS), "xchunk: stone -y");
				CHECK(managerX.placeVoxel(worldOf(A, 15, y0 + 1, 8), BRICKS), "xchunk: stone +y");
				CHECK(managerX.placeVoxel(worldOf(A, 15, y0, 7), BRICKS), "xchunk: stone -z");
				CHECK(managerX.placeVoxel(worldOf(A, 15, y0, 9), BRICKS), "xchunk: stone +z");
				CHECK(managerX.placeVoxel(worldOf(A, 15, y0, 8), BRICKS),
				      "xchunk: center voxel placed");

				// A border edit re-arms both the target and the mirror
				// chunk; the committed meshes stay renderable meanwhile.
				CHECK(A->getState() == ChunkState::GENERATED &&
				          B->getState() == ChunkState::GENERATED,
				      "xchunk: target and mirror neighbor re-armed");
				{
					std::vector<Chunk::IndirectDraw> aKeep, bKeep;
					CHECK(A->collectOpaqueDraws(aKeep) > 0 && B->collectOpaqueDraws(bKeep) > 0,
					      "xchunk: committed meshes collectable while re-armed");
				}

				// Both members published -> the group is ready, but one budget
				// unit records exactly one member even when both fit staging.
				remeshOne(A);
				remeshOne(B);
				const Camera camX(glm::vec3(8.0f, static_cast<float>(y0), 8.0f));
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 1);
				CHECK(A->needsGPUUpload() && B->needsGPUUpload() &&
				          ChunkManagerProbe::groupState(managerX, 0) == CommitGroupState::Uploading,
				      "xchunk: budget=1 records only one member COPY");
				submitFrame();
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 1);
				CHECK(!A->needsGPUUpload() && !B->needsGPUUpload(),
				      "xchunk: second budget unit completes the atomic border commit");
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 0,
				      "xchunk: fixture groups consumed by the commit");
				submitFrame();

				// The light-halo neighbors the same edits re-armed are still
				// un-remeshed (GENERATED): they never joined the group and
				// did not block it.
				{
					size_t generated = 0;
					for (Chunk *chunk : managerX.getActiveChunks())
						if (chunk && chunk->getState() == ChunkState::GENERATED)
							++generated;
					CHECK(generated > 0, "xchunk: light-only neighbors stay out of the group");
				}

				std::vector<Chunk::IndirectDraw> committedA, committedB;
				const size_t aCount = A->collectOpaqueDraws(committedA);
				const size_t bCount = B->collectOpaqueDraws(committedB);
				const uint32_t bIndices = B->getOpaqueIndexCount();
				CHECK(aCount > 0 && bCount > 0, "xchunk: fixture meshes committed");

				// --- Removal: A ready / B not ready -> NEITHER commits ---
				CHECK(managerX.deleteVoxel(worldOf(A, 15, y0, 8)),
				      "xchunk: border removal accepted");
				remeshOne(A); // A publishes its replacement; B is still remeshing
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 4);
				CHECK(A->needsGPUUpload(),
				      "xchunk: blocked group does not commit the ready member");
				CHECK(B->getState() == ChunkState::GENERATED && B->dirtySections() != 0,
				      "xchunk: blocked group waits for the remeshing member");
				{
					std::vector<Chunk::IndirectDraw> aPending, bPending;
					CHECK(A->collectOpaqueDraws(aPending) == aCount &&
					          sameDraws(aPending, committedA),
					      "xchunk: blocked group keeps old A drawn");
					CHECK(B->collectOpaqueDraws(bPending) == bCount &&
					          sameDraws(bPending, committedB),
					      "xchunk: blocked group keeps old B drawn");
				}
				submitFrame();

				// --- Round 4: ALLOCATE failure at the Waiting->Uploading
				// transition --- the group rolls back and stays
				// WaitingForMeshes with every arena counter exactly as
				// before (no leaked suballocation).
				remeshOne(B);
				const auto stagingNeedOf = [](Chunk *chunk) {
					VkDeviceSize need = 0;
					MeshBuildResult *r = ChunkStateProbe::pending(*chunk);
					CHECK(r != nullptr, "xchunk: result attached for staging accounting");
					if (r)
						for (int s = 0; s < 16; ++s)
							if ((r->sectionsBuilt >> s) & 1u)
							{
								const auto align = [](size_t bytes) {
									return static_cast<VkDeviceSize>(
										(bytes + StagingRing::kAlignment - 1) /
										StagingRing::kAlignment * StagingRing::kAlignment);
								};
								need += align(r->sections[static_cast<size_t>(s)].opaqueVertices.size() *
								              sizeof(Vertex));
								need += align(r->sections[static_cast<size_t>(s)].opaqueIndices.size() *
								              sizeof(uint32_t));
								need += align(r->sections[static_cast<size_t>(s)].waterVertices.size() *
								              sizeof(Vertex));
								need += align(r->sections[static_cast<size_t>(s)].waterIndices.size() *
								              sizeof(uint32_t));
							}
					return need;
				};
				const VkDeviceSize needA = stagingNeedOf(A);
				const VkDeviceSize needB = stagingNeedOf(B);
				CHECK(needA > 0 && needB > 0, "xchunk: both members need staging");
				CHECK(needA < stagingX.sliceCapacity() && needB < stagingX.sliceCapacity(),
				      "xchunk: each member individually fits one staging slice");

				const auto metricsEqual = [](const MeshArena::Metrics &a,
				                             const MeshArena::Metrics &b) {
					return a.pages == b.pages && a.liveBlocks == b.liveBlocks &&
					       a.liveBytes == b.liveBytes && a.freeBytes == b.freeBytes;
				};
				const auto arenaSnapshot = [&]() {
					return std::make_tuple(arenas.opaqueVertex.metrics(),
					                       arenas.opaqueIndex.metrics(),
					                       arenas.waterVertex.metrics(),
					                       arenas.waterIndex.metrics());
				};
				const auto beforeFailedAllocate = arenaSnapshot();
				// Opaque vertex allocation succeeds first; opaque index then
				// fails, exercising rollback of an already-owned range.
				arenas.opaqueIndex.setFailNextAllocations(1);
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 2);
				arenas.opaqueIndex.setFailNextAllocations(0);
				{
					const auto [ov, oi, wv, wi] = beforeFailedAllocate;
					CHECK(metricsEqual(ov, arenas.opaqueVertex.metrics()) &&
					          metricsEqual(oi, arenas.opaqueIndex.metrics()) &&
					          metricsEqual(wv, arenas.waterVertex.metrics()) &&
					          metricsEqual(wi, arenas.waterIndex.metrics()),
					      "xchunk: failed ALLOCATE leaks nothing (live blocks/bytes/pages)");
				}
				CHECK(A->needsGPUUpload() && B->needsGPUUpload(),
				      "xchunk: failed ALLOCATE defers the group");
				{
					std::vector<Chunk::IndirectDraw> aPending, bPending;
					CHECK(A->collectOpaqueDraws(aPending) == aCount &&
					          sameDraws(aPending, committedA),
					      "xchunk: failed ALLOCATE keeps old A drawn");
					CHECK(B->collectOpaqueDraws(bPending) == bCount &&
					          sameDraws(bPending, committedB),
					      "xchunk: failed ALLOCATE keeps old B drawn");
				}
				submitFrame();

				// --- Liveness (round 4, items 6-7/13): the group's combined
				// staging need exceeds one frame's available room, so the
				// members record PROGRESSIVELY across frames while both old
				// meshes keep rendering; publication happens atomically in
				// the frame the last member lands.
				beginFrame();
				{
					VkDeviceSize off = 0;
					void *sink = nullptr;
					CHECK(stagingX.alloc(stagingX.sliceCapacity() - needA, off, sink),
					      "xchunk: frame 1 leaves room for exactly A");
				}
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 1);
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 1 &&
				          ChunkManagerProbe::groupState(managerX, 0) == CommitGroupState::Uploading,
				      "xchunk: group uploads progressively across frames");
				CHECK(A->needsGPUUpload() && B->needsGPUUpload(),
				      "xchunk: no member publishes while the group is incomplete");
				{
					std::vector<Chunk::IndirectDraw> aPending, bPending;
					CHECK(A->collectOpaqueDraws(aPending) == aCount &&
					          sameDraws(aPending, committedA),
					      "xchunk: frame N keeps old A drawn");
					CHECK(B->collectOpaqueDraws(bPending) == bCount &&
					          sameDraws(bPending, committedB),
					      "xchunk: frame N keeps old B drawn");
				}
				submitFrame();

				// Frame 2: room for exactly B -> B records -> the whole group
				// publishes atomically this frame.
				beginFrame();
				{
					VkDeviceSize off = 0;
					void *sink = nullptr;
					CHECK(stagingX.alloc(stagingX.sliceCapacity() - needB, off, sink),
					      "xchunk: frame 2 leaves room for exactly B");
				}
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 1);
				CHECK(!A->needsGPUUpload() && !B->needsGPUUpload(),
				      "xchunk: the group publishes atomically when the last member lands");
				{
					std::vector<Chunk::IndirectDraw> aNew, bNew;
					CHECK(A->collectOpaqueDraws(aNew) > 0 && !sameDraws(aNew, committedA),
					      "xchunk: A committed the removal");
					CHECK(B->collectOpaqueDraws(bNew) > 0 && !sameDraws(bNew, committedB),
					      "xchunk: B committed in the SAME slot as A");
					CHECK(B->getOpaqueIndexCount() == bIndices + 6,
					      "xchunk: B's replacement adds exactly the exposed border face");
				}
				submitFrame();

				// --- Supersession before commit: stale result rejected ---
				CHECK(managerX.placeVoxel(worldOf(A, 15, y0 + 2, 8), BRICKS),
				      "supersession: border edit #1 accepted");
				MeshBuildResult *stale = A->getMeshResultPool()->acquire();
				{
					const uint16_t mask = A->takeDirtySections();
					A->buildMesh(*stale, A->meshGeneration(), A->meshRevision(), mask);
					A->getMeshResultPool()->finishBuild(stale);
				}
				CHECK(managerX.placeVoxel(worldOf(A, 10, y0 + 2, 8), BRICKS),
				      "supersession: later edit supersedes the in-flight result");
				CHECK(!A->publishMeshResult(stale),
				      "supersession: stale result rejected by revision validation");
				remeshOne(A);
				remeshOne(B);
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 2);
				CHECK(!A->needsGPUUpload() && !B->needsGPUUpload(),
				      "supersession: the newest results commit together");
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 0,
				      "supersession: no group leaks after commit");
				submitFrame();

				// --- Corner edit: target + BOTH side neighbors (3 chunks)
				// commit atomically after three COPY budget units ---
				int hCorner = std::max(std::max(columnSurface(A, 15, 15),
				                                columnSurface(B, 0, 15)),
				                       columnSurface(S, 15, 0));
				CHECK(hCorner > 1 && hCorner + 5 < static_cast<int>(CHUNK_HEIGHT),
				      "corner: border columns surfaced");
				const int yC = hCorner + 3;
				CHECK(managerX.placeVoxel(worldOf(A, 15, yC, 15), BRICKS),
				      "corner: corner voxel placed");
				remeshOne(A);
				remeshOne(B);
				remeshOne(S);
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 3);
				CHECK(!A->needsGPUUpload() && !B->needsGPUUpload() && !S->needsGPUUpload(),
				      "corner: three COPY units commit all linked chunks together");
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 0,
				      "corner: group consumed");
				submitFrame();

				// --- Overlapping edits fuse into one group (PR #178 review
				// round 3): an east border edit and a south border edit of
				// the SAME target chunk share A, so {A,B} + {A,S} must fuse
				// into {A,B,S} BEFORE the scheduler can see them - otherwise
				// committing {A,B} alone could publish "new A + new B + old
				// S" or "new A + old B + new S".
				int hOverlap = std::max(columnSurface(A, 15, 3),
				                        columnSurface(A, 8, 15));
				CHECK(hOverlap > 1 && hOverlap + 5 < static_cast<int>(CHUNK_HEIGHT),
				      "overlap: border columns surfaced");
				const int y1 = hOverlap + 3;

				// East edit: {A,B} registers; A publishes its result, B is
				// still remeshing.
				CHECK(managerX.placeVoxel(worldOf(A, 15, y1, 3), BRICKS),
				      "overlap: east border edit accepted");
				MeshBuildResult *eastOnly = A->getMeshResultPool()->acquire();
				{
					const uint16_t mask = A->takeDirtySections();
					A->buildMesh(*eastOnly, A->meshGeneration(), A->meshRevision(), mask);
					A->getMeshResultPool()->finishBuild(eastOnly);
				}
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 4);
				CHECK(A->needsGPUUpload(),
				      "overlap: the ready member waits for its fused group");
				submitFrame();

				// South edit lands BEFORE any commit: it shares A, so the
				// registration must fuse {A,B} + {A,S} into one {A,B,S}.
				CHECK(managerX.placeVoxel(worldOf(A, 8, y1, 15), BRICKS),
				      "overlap: south border edit accepted");
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 1,
				      "overlap: overlapping groups fused into one");
				{
					const std::vector<Chunk *> &fused =
						ChunkManagerProbe::groupChunks(managerX, 0);
					const bool allThere = fused.size() == 3 &&
					                      groupContains(fused, A) &&
					                      groupContains(fused, B) &&
					                      groupContains(fused, S);
					CHECK(allThere, "overlap: fused group is exactly {A,B,S}");
				}

				// The east-only result built before the south edit is now
				// stale: revision validation must reject it (the A publish
				// below then carries BOTH edits).
				CHECK(!A->publishMeshResult(eastOnly),
				      "overlap: superseded east-only result rejected");
				remeshOne(A);
				remeshOne(B);
				remeshOne(S);
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 3);
				CHECK(!A->needsGPUUpload() && !B->needsGPUUpload() && !S->needsGPUUpload(),
				      "overlap: three COPY units commit the fused group together");
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 0,
				      "overlap: fused group consumed");
				submitFrame();

				// --- Partial unload (round 4, items 1-2): a 3-member group
				// losing one member keeps the remaining atomic dependency
				// {A,SS}; a 2-member group losing one member dissolves. ---
				int hU = columnSurface(A, 15, 15);
				CHECK(hU > 1 && hU + 5 < static_cast<int>(CHUNK_HEIGHT),
				      "unload: border columns surfaced");
				const int yU = hU + 3;
				CHECK(managerX.placeVoxel(worldOf(A, 15, yU, 15), BRICKS),
				      "unload: corner edit accepted");
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 1 &&
				          ChunkManagerProbe::groupChunks(managerX, 0).size() == 3,
				      "unload: three-member group registered");
				ChunkManagerProbe::removeFromGroups(managerX, B); // B unloads
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 1,
				      "unload: group survives with two members");
				{
					const std::vector<Chunk *> survivors =
						ChunkManagerProbe::groupChunks(managerX, 0);
					CHECK(survivors.size() == 2 && groupContains(survivors, A) &&
					          groupContains(survivors, S),
					      "unload: group is now exactly {A,SS}");
				}
				remeshOne(A);
				remeshOne(S);
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 2);
				CHECK(!A->needsGPUUpload() && !S->needsGPUUpload(),
				      "unload: the surviving pair commits together (old S impossible)");
				submitFrame();

				// B is gone from the group: it remeshes and uploads on its own.
				remeshOne(B);
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 4);
				CHECK(!B->needsGPUUpload(), "unload: removed member uploads independently");
				submitFrame();

				// A 2-member group losing one member has no coupling left.
				CHECK(managerX.placeVoxel(worldOf(A, 15, yU, 12), BRICKS),
				      "unload2: east edit accepted");
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 1,
				      "unload2: pair group registered");
				ChunkManagerProbe::removeFromGroups(managerX, B);
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 0,
				      "unload2: pair group dissolved");
				remeshOne(A);
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 2);
				CHECK(!A->needsGPUUpload(), "unload2: A uploads solo after the dissolve");
				submitFrame();

				// --- Uploaded-but-unpublished member superseded (round 4,
				// item 8/13): A records in frame 1, is re-edited before
				// publication, its stale replacement is discarded, and the
				// newest result joins B for the atomic publish. ---
				int hS = -1;
				for (int lz : {5, 6, 10, 12, 13})
					hS = std::max(hS, columnSurface(A, 15, lz));
				CHECK(hS > 1 && hS + 5 < static_cast<int>(CHUNK_HEIGHT),
				      "supersession2: border columns surfaced");
				const int yS = hS + 3;
				CHECK(managerX.placeVoxel(worldOf(A, 15, yS, 5), BRICKS),
				      "supersession2: border edit accepted");
				remeshOne(A);
				remeshOne(B);
				beginFrame();
				{
					VkDeviceSize off = 0;
					void *sink = nullptr;
					const VkDeviceSize needA = stagingNeedOf(A);
					CHECK(stagingX.alloc(stagingX.sliceCapacity() - needA, off, sink),
					      "supersession2: ring leaves room for exactly A");
				}
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 1);
				CHECK(ChunkManagerProbe::groupState(managerX, 0) == CommitGroupState::Uploading,
				      "supersession2: A recorded, B deferred by exact-fit staging");
				submitFrame();
				CHECK(managerX.placeVoxel(worldOf(A, 15, yS, 6), BRICKS),
				      "supersession2: A re-edited while uploaded but unpublished");
				remeshOne(A);
				remeshOne(B);
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 4);
				CHECK(!A->needsGPUUpload() && !B->needsGPUUpload(),
				      "supersession2: stale replacement discarded, newest results published");
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 0,
				      "supersession2: group consumed");
				submitFrame();

				// --- Unload of the recorded member (round 4, item 13): the
				// unpublished replacement is discarded and the surviving
				// member falls back to a solo upload. ---
				CHECK(managerX.placeVoxel(worldOf(A, 15, yS, 13), BRICKS),
				      "unload-recorded: border edit accepted");
				remeshOne(A);
				remeshOne(B);
				beginFrame();
				{
					VkDeviceSize off = 0;
					void *sink = nullptr;
					const VkDeviceSize needA = stagingNeedOf(A);
					CHECK(stagingX.alloc(stagingX.sliceCapacity() - needA, off, sink),
					      "unload-recorded: ring leaves room for exactly A");
				}
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 1);
				CHECK(ChunkManagerProbe::groupState(managerX, 0) == CommitGroupState::Uploading,
				      "unload-recorded: A recorded, B deferred by exact-fit staging");
				const auto liveBlocksBeforeDissolve = [&]() {
					return arenas.opaqueVertex.metrics().liveBlocks +
					       arenas.opaqueIndex.metrics().liveBlocks +
					       arenas.waterVertex.metrics().liveBlocks +
					       arenas.waterIndex.metrics().liveBlocks;
				}();
				submitFrame();
				ChunkManagerProbe::removeFromGroups(managerX, A); // A unloads
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 0,
				      "unload-recorded: pair group dissolved");
				const auto liveBlocksAfterDissolve =
				    arenas.opaqueVertex.metrics().liveBlocks + arenas.opaqueIndex.metrics().liveBlocks +
				    arenas.waterVertex.metrics().liveBlocks + arenas.waterIndex.metrics().liveBlocks;
				CHECK(liveBlocksAfterDissolve < liveBlocksBeforeDissolve,
				      "unload-recorded: unrecorded survivor replacement ranges freed");
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 4);
				CHECK(!B->needsGPUUpload(),
				      "unload-recorded: remaining member falls back to solo upload");
				submitFrame();

				// --- New border edit fused into a partially-uploaded group
				// (round 4, item 13): the recorded-but-stale replacement of
				// the shared member is discarded, the fresh one is recorded,
				// and the whole group publishes together. ---
				CHECK(managerX.placeVoxel(worldOf(A, 15, yS, 10), BRICKS),
				      "fuse-live: border edit accepted");
				remeshOne(A);
				remeshOne(B);
				beginFrame();
				{
					VkDeviceSize off = 0;
					void *sink = nullptr;
					const VkDeviceSize needA = stagingNeedOf(A);
					CHECK(stagingX.alloc(stagingX.sliceCapacity() - needA, off, sink),
					      "fuse-live: ring leaves room for exactly A");
				}
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 1);
				CHECK(ChunkManagerProbe::groupState(managerX, 0) == CommitGroupState::Uploading,
				      "fuse-live: A recorded, B deferred by exact-fit staging");
				submitFrame();
				CHECK(managerX.placeVoxel(worldOf(A, 15, yS, 12), BRICKS),
				      "fuse-live: second border edit fuses into the uploading group");
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 1,
				      "fuse-live: still one fused group");
				CHECK(ChunkManagerProbe::replacementRecorded(managerX, 0, A),
				      "fuse-live: fusion preserves the existing recorded replacement owner");
				remeshOne(A);
				remeshOne(B);
				beginFrame();
				managerX.uploadPendingMeshes(vk.allocator.handle(), stagingX, vk.cmd,
				                             retire, arenas, camX, 4);
				CHECK(!A->needsGPUUpload() && !B->needsGPUUpload(),
				      "fuse-live: fused group publishes together");
				CHECK(ChunkManagerProbe::commitGroups(managerX) == 0,
				      "fuse-live: group consumed");
				submitFrame();

				// --- Transitive fusion of the registration helper itself
				// (probe-level): {A,B} + {B,S} + {S,W} must collapse into a
				// single {A,B,S,W} group, exercising the fixed-point merge
				// without staging four real edits.
				{
					Chunk *W = managerX.getChunk(glm::ivec3(-1, 0, 0));
					CHECK(W != nullptr, "transitive: west chunk bootstrapped");
					if (W)
					{
						ChunkManagerProbe::registerGroup(managerX, {A, B});
						ChunkManagerProbe::registerGroup(managerX, {B, S});
						CHECK(ChunkManagerProbe::commitGroups(managerX) == 1 &&
						          ChunkManagerProbe::groupChunks(managerX, 0).size() == 3,
						      "transitive: {A,B} and {B,S} fuse to {A,B,S}");
						ChunkManagerProbe::registerGroup(managerX, {S, W});
						CHECK(ChunkManagerProbe::commitGroups(managerX) == 1,
						      "transitive: one group remains");
						const std::vector<Chunk *> &chain =
							ChunkManagerProbe::groupChunks(managerX, 0);
						const bool chainOk = chain.size() == 4 &&
						                     groupContains(chain, A) &&
						                     groupContains(chain, B) &&
						                     groupContains(chain, S) &&
						                     groupContains(chain, W);
						CHECK(chainOk,
						      "transitive: {A,B}+{B,S}+{S,W} collapse to {A,B,S,W}");
					}
				}
			}
			stagingX.shutdown();
		}

		// -----------------------------------------------------------------
		// 19. Commit-group teardown ownership (issue #177 review): the
		// manager owns prepared-but-unpublished replacements and must discard
		// them before returning their chunks to the pool. Cover both the
		// immediate-free and submitted-copy retirement paths, then recreate a
		// manager against the same arenas (the reloadWorld ownership contract).
		// -----------------------------------------------------------------
		{
			GpuResourceRetire teardownRetire;
			teardownRetire.init(vk.allocator.handle(), 2);
			MeshArenas teardownArenas;
			teardownArenas.init(vk.allocator.handle(), teardownRetire, sizeof(Vertex), 2,
			                    2ull * 1024ull * 1024ull, 1ull * 1024ull * 1024ull);
			StagingRing teardownStaging;
			teardownStaging.init(vk.allocator.handle(), 2);
			ChunkPool teardownPool(32);
			TerrainGenerator teardownGen(177);
			Camera teardownCamera(glm::vec3(8.0f, 100.0f, 8.0f));
			uint64_t teardownFrame = 30000;

			const auto snapshotMetrics = [&]() {
				return std::make_tuple(teardownArenas.opaqueVertex.metrics(),
				                       teardownArenas.opaqueIndex.metrics(),
				                       teardownArenas.waterVertex.metrics(),
				                       teardownArenas.waterIndex.metrics());
			};
			const auto baseline = snapshotMetrics();
			const auto metricsMatch = [](const MeshArena::Metrics &a,
			                             const MeshArena::Metrics &b) {
				return a.liveBlocks == b.liveBlocks && a.liveBytes == b.liveBytes;
			};
			const auto checkBaseline = [&](const char *stage) {
				const auto [ov0, oi0, wv0, wi0] = baseline;
				const auto [ov1, oi1, wv1, wi1] = snapshotMetrics();
				CHECK(metricsMatch(ov0, ov1),
				      std::string(stage) + ": opaque vertex live blocks/bytes recovered");
				CHECK(metricsMatch(oi0, oi1),
				      std::string(stage) + ": opaque index live blocks/bytes recovered");
				CHECK(metricsMatch(wv0, wv1),
				      std::string(stage) + ": water vertex live blocks/bytes recovered");
				CHECK(metricsMatch(wi0, wi1),
				      std::string(stage) + ": water index live blocks/bytes recovered");
			};
			const auto beginTeardownFrame = [&]() {
				teardownRetire.beginFrame(teardownFrame);
				teardownArenas.beginFrame(teardownFrame);
				teardownStaging.beginFrame(static_cast<uint32_t>(teardownFrame % 2));
				++teardownFrame;
			};
			const auto drainTeardownRetirement = [&]() {
				for (int i = 0; i < 6; ++i)
				{
					teardownRetire.beginFrame(teardownFrame);
					teardownArenas.beginFrame(teardownFrame);
					++teardownFrame;
				}
			};
			const auto createUploadingPair = [&](ChunkManager &teardownManager) {
				teardownManager.generateInitialArea(glm::vec3(0.0f, 0.0f, 0.0f), 1,
				                                    vk.allocator.handle(), imm, teardownArenas);
				Chunk *a = teardownManager.getChunk(glm::ivec3(0, 0, 0));
				Chunk *b = teardownManager.getChunk(glm::ivec3(1, 0, 0));
				CHECK(a != nullptr && b != nullptr, "teardown: adjacent chunks bootstrapped");
				if (!a || !b)
					return std::make_pair(a, b);

				int surface = -1;
				for (Chunk *chunk : {a, b})
				{
					const uint32_t x = chunk == a ? CHUNK_SIZE - 1 : 0;
					for (int y = static_cast<int>(CHUNK_HEIGHT) - 1; y >= 0; --y)
					{
						if (chunk->getVoxel(x, static_cast<uint32_t>(y), 5).type !=
						    static_cast<uint8_t>(AIR))
						{
							surface = std::max(surface, y);
							break;
						}
					}
				}
				CHECK(surface > 1 && surface + 5 < static_cast<int>(CHUNK_HEIGHT),
				      "teardown: border columns surfaced");
				const glm::vec3 p = a->getPosition();
				CHECK(teardownManager.placeVoxel(
				          glm::vec3(p.x + static_cast<float>(CHUNK_SIZE) - 0.5f,
				                    static_cast<float>(surface + 3) + 0.5f, p.z + 5.5f),
				          BRICKS),
				      "teardown: border edit accepted");

				for (Chunk *chunk : {a, b})
				{
					const uint16_t mask = chunk->takeDirtySections();
					MeshBuildResult *result = chunk->getMeshResultPool()->acquire();
					chunk->buildMesh(*result, chunk->meshGeneration(), chunk->meshRevision(), mask);
					chunk->getMeshResultPool()->finishBuild(result);
					CHECK(chunk->publishMeshResult(result), "teardown: remesh published");
				}
				return std::make_pair(a, b);
			};

			// A. Both replacements are prepared, but staging prevents COPY.
			{
				ChunkManager teardownManager(&teardownGen, nullptr, &teardownPool);
				const auto [a, b] = createUploadingPair(teardownManager);
				if (a && b)
				{
					beginTeardownFrame();
					VkDeviceSize offset = 0;
					void *sink = nullptr;
					CHECK(teardownStaging.alloc(teardownStaging.sliceCapacity(), offset, sink),
					      "teardown-unrecorded: staging slice exhausted");
					teardownManager.uploadPendingMeshes(
					    vk.allocator.handle(), teardownStaging, vk.cmd, teardownRetire,
					    teardownArenas, teardownCamera, 1);
					CHECK(ChunkManagerProbe::commitGroups(teardownManager) == 1,
					      "teardown-unrecorded: fixture owns an unfinished group");
					CHECK(ChunkManagerProbe::groupState(teardownManager, 0) ==
					          CommitGroupState::Uploading,
					      "teardown-unrecorded: group remains Uploading");
					CHECK(ChunkManagerProbe::replacementValid(teardownManager, 0, a) &&
					          ChunkManagerProbe::replacementValid(teardownManager, 0, b),
					      "teardown-unrecorded: both replacements are prepared");
					CHECK(!ChunkManagerProbe::replacementRecorded(teardownManager, 0, a) &&
					          !ChunkManagerProbe::replacementRecorded(teardownManager, 0, b),
					      "teardown-unrecorded: neither replacement recorded COPY");
				}
			}
			drainTeardownRetirement();
			checkBaseline("teardown-unrecorded");

			// B. Exactly one member COPY is submitted; the manager destroys the
			// recorded replacement through retire() and the other immediately.
			{
				ChunkManager teardownManager(&teardownGen, nullptr, &teardownPool);
				const auto [a, b] = createUploadingPair(teardownManager);
				if (a && b)
				{
					beginTeardownFrame();
					teardownManager.uploadPendingMeshes(
					    vk.allocator.handle(), teardownStaging, vk.cmd, teardownRetire,
					    teardownArenas, teardownCamera, 1);
					CHECK(ChunkManagerProbe::commitGroups(teardownManager) == 1,
					      "teardown-recorded: fixture owns an unfinished group");
					CHECK(ChunkManagerProbe::groupState(teardownManager, 0) ==
					          CommitGroupState::Uploading,
					      "teardown-recorded: group remains Uploading");
					CHECK(ChunkManagerProbe::replacementValid(teardownManager, 0, a) &&
					          ChunkManagerProbe::replacementValid(teardownManager, 0, b),
					      "teardown-recorded: both replacements remain owned");
					CHECK(ChunkManagerProbe::replacementRecorded(teardownManager, 0, a) !=
					          ChunkManagerProbe::replacementRecorded(teardownManager, 0, b),
					      "teardown-recorded: exactly one member COPY recorded");
					CHECK(vk.flush(), "teardown-recorded: submitted member COPY");
				}
			}
			drainTeardownRetirement();
			checkBaseline("teardown-recorded");

			// Recreate a manager against the same arenas, matching reloadWorld's
			// resource lifetime without requiring a full Engine fixture.
			{
				ChunkManager recreated(&teardownGen, nullptr, &teardownPool);
				recreated.generateInitialArea(glm::vec3(0.0f, 0.0f, 0.0f), 1,
				                                vk.allocator.handle(), imm, teardownArenas);
				Chunk *center = recreated.getChunk(glm::ivec3(0, 0, 0));
				CHECK(center && center->hasRenderableOpaqueDraws(),
				      "teardown-recreate: replacement manager reuses arenas");
				CHECK(vkQueueWaitIdle(vk.queue) == VK_SUCCESS,
				      "teardown-recreate: queue idle before manager destruction");
			}
			drainTeardownRetirement();
			checkBaseline("teardown-recreate");

			CHECK(vkQueueWaitIdle(vk.queue) == VK_SUCCESS,
			      "teardown: queue idle before resource shutdown");
			teardownStaging.shutdown();
			teardownArenas.shutdown();
			teardownRetire.shutdown();
		}
	}

	arenas.shutdown();
	imm.shutdown();
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
