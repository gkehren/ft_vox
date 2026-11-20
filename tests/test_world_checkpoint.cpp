// test_world_checkpoint — CPU-only acceptance for the generation-based
// gameplay persistence checkpoint layer (stabilization Phase 3, PR5a;
// docs/adr/gameplay-persistence-transactions.md). Links ONLY ft_vox_core:
// no GPU, window, camera or simulation runtime — the store is a pure
// filesystem/format component.
//
// Coverage:
//   * manifest + player codec round-trips and strict corruption taxonomy;
//   * commit -> recover basics (generation numbering, fallback retention,
//     immutable committed payloads, legacy mode untouched);
//   * the full deterministic fault-injection matrix (ADR §3 points 1-5,
//     7-10; point 6 is the SaveService write seam, covered by the runtime
//     integration tests): recovery always yields the previous complete
//     generation (pre-publication faults) or the new one (post-publication),
//     NEVER a mixture;
//   * manifest-level fault tolerance (manifest.prev rotation), generation
//     validation (payload checksums, chunk codec, exact file set), and
//     unrecoverable-corruption reporting;
//   * player carry-forward (explicit / carried / absent / corrupt refusal);
//   * orphaned staging cleanup and retryability.
//
// Style mirrors test_world_save: a CHECK macro, plain main().

#include <World/WorldCheckpoint.hpp>
#include <World/WorldPersistence.hpp>
#include <World/WorldSave.hpp>
#include <Items/ItemTypes.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "TestMain.hpp"

namespace
{
namespace fs = std::filesystem;
using worldsave::CheckpointFaultPoint;
using worldsave::CheckpointManifest;
using worldsave::CheckpointPayloads;
using worldsave::CheckpointStore;
using worldsave::SaveStatus;

fs::path makeTempDir(const char *tag)
{
	const auto base = fs::temp_directory_path() /
					  ("ft_vox_checkpoint_" + std::string(tag) + "_" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(base);
	return base;
}

struct TempRoot
{
	fs::path path;
	explicit TempRoot(const char *tag) : path(makeTempDir(tag)) {}
	~TempRoot()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

std::vector<uint8_t> readBytes(const fs::path &p)
{
	std::ifstream in(p, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path &p, const std::vector<uint8_t> &bytes)
{
	fs::create_directories(p.parent_path());
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void corruptByte(const fs::path &p, size_t offset)
{
	auto bytes = readBytes(p);
	CHECK(!bytes.empty(), "corrupt target exists");
	bytes[offset % bytes.size()] ^= 0xFF;
	writeBytes(p, bytes);
}

// Valid encoded payloads (the sibling formats' own codecs).
worldsave::InventoryPersistState sampleInventory(uint8_t selected = 2)
{
	worldsave::InventoryPersistState s;
	s.selectedSlot = selected;
	s.slots[0] = {static_cast<uint16_t>(items::ItemId::Stone), 10};
	s.slots[9] = {static_cast<uint16_t>(items::ItemId::Dirt), 64};
	return s;
}

worldsave::ItemsPersistState sampleItems(uint64_t nextId = 3)
{
	worldsave::ItemsPersistState s;
	s.nextEntityId = nextId;
	worldsave::ItemEntityPersistState e;
	e.id = 1;
	e.itemId = static_cast<uint16_t>(items::ItemId::Cobblestone);
	e.count = 1;
	e.px = 8.5;
	e.py = 64.0;
	e.pz = 8.5;
	e.ageTicks = 12;
	e.pickupDelayTicks = 0;
	s.entities.push_back(e);
	return s;
}

worldsave::PlayerPersistState samplePlayer(double y = 64.0)
{
	worldsave::PlayerPersistState s;
	s.x = 8.5;
	s.y = y;
	s.z = 8.5;
	s.yaw = 90.f;
	s.pitch = -10.f;
	s.flight = false;
	s.selectedBlock = 1;
	return s;
}

CheckpointPayloads makePayloads(bool withPlayer, uint8_t selected = 2, uint64_t nextId = 3)
{
	CheckpointPayloads p;
	p.playerPresent = withPlayer;
	if (withPlayer)
		p.player = worldsave::encodePlayerState(samplePlayer());
	p.inventory = worldsave::encodeInventoryState(sampleInventory(selected));
	p.items = worldsave::encodeItemsState(sampleItems(nextId));
	p.worldEditRevision = 42;
	return p;
}

// A live working set with two chunk override files (the SaveService output).
void makeLiveChunks(const fs::path &worldDir)
{
	const fs::path chunks = worldDir / "chunks";
	std::vector<worldsave::ChunkEdit> a{{8 + 64 * 256 + 8 * 16, static_cast<uint8_t>(STONE)}};
	std::vector<worldsave::ChunkEdit> b{{1 + 70 * 256 + 2 * 16, static_cast<uint8_t>(DIRT)}};
	CHECK(worldsave::writeChunkFile(chunks, 0, 0, a) == SaveStatus::Ok, "live chunk 0_0 written");
	CHECK(worldsave::writeChunkFile(chunks, -1, 2, b) == SaveStatus::Ok, "live chunk -1_2 written");
}

size_t countGenerationDirs(const fs::path &worldDir)
{
	size_t n = 0;
	std::error_code ec;
	for (fs::directory_iterator it(worldsave::checkpointsDir(worldDir), ec), end; it != end;
		 it.increment(ec))
		if (it->is_directory(ec) && it->path().filename().string().rfind("generation-", 0) == 0)
			++n;
	return n;
}

// ---------------------------------------------------------------------------
// 1. Manifest codec: round-trip + strict corruption taxonomy.
// ---------------------------------------------------------------------------
void testManifestCodec()
{
	CheckpointManifest m;
	m.committedGeneration = 7;
	m.fallbackGeneration = 6;
	m.worldEditRevision = 1234;
	m.playerPresent = true;
	m.playerChecksum = 0x1122334455667788ULL;
	m.inventoryChecksum = 0x99AABBCCDDEEFF00ULL;
	m.itemsChecksum = 0x0102030405060708ULL;
	m.chunks = {{-1, 2, 111}, {0, 0, 222}, {5, -3, 333}};
	const auto bytes = worldsave::encodeCheckpointManifest(m);
	CheckpointManifest back;
	CHECK(worldsave::decodeCheckpointManifest(bytes.data(), bytes.size(), back) == SaveStatus::Ok,
		  "manifest round-trip ok");
	CHECK(back.committedGeneration == 7 && back.fallbackGeneration == 6 &&
			  back.worldEditRevision == 1234 && back.playerPresent &&
			  back.playerChecksum == m.playerChecksum &&
			  back.inventoryChecksum == m.inventoryChecksum && back.itemsChecksum == m.itemsChecksum,
		  "manifest fields round-trip");
	CHECK(back.chunks.size() == 3 && back.chunks[0].chunkX == -1 && back.chunks[2].chunkZ == -3,
		  "manifest chunk refs round-trip (sorted)");

	// Corruption taxonomy.
	auto bad = bytes;
	bad[0] ^= 0xFF;
	CHECK(worldsave::decodeCheckpointManifest(bad.data(), bad.size(), back) == SaveStatus::BadMagic,
		  "manifest bad magic");
	bad = bytes;
	bad[4] = 99; // version byte (little-endian u32)
	CHECK(worldsave::decodeCheckpointManifest(bad.data(), bad.size(), back) ==
			  SaveStatus::UnsupportedVersion,
		  "manifest unsupported version");
	CHECK(worldsave::decodeCheckpointManifest(bytes.data(), bytes.size() - 1, back) ==
			  SaveStatus::Corrupt,
		  "manifest truncation rejected");
	auto extended = bytes;
	extended.push_back(0);
	CHECK(worldsave::decodeCheckpointManifest(extended.data(), extended.size(), back) ==
			  SaveStatus::Corrupt,
		  "manifest trailing garbage rejected (exact size)");
	CHECK(worldsave::decodeCheckpointManifest(bytes.data(), 10, back) == SaveStatus::Corrupt,
		  "manifest short header rejected");
	// committedGeneration == 0 is invalid.
	auto zeroGen = bytes;
	std::fill(zeroGen.begin() + 8, zeroGen.begin() + 16, 0);
	CHECK(worldsave::decodeCheckpointManifest(zeroGen.data(), zeroGen.size(), back) ==
			  SaveStatus::Corrupt,
		  "manifest generation 0 rejected");
}

// ---------------------------------------------------------------------------
// 2. Player codec (extracted to the format layer): round-trip + strictness.
// ---------------------------------------------------------------------------
void testPlayerCodec()
{
	const auto state = samplePlayer(129.75);
	const auto bytes = worldsave::encodePlayerState(state);
	CHECK(bytes.size() == 45, "player.state is 45 bytes");
	worldsave::PlayerPersistState back;
	CHECK(worldsave::decodePlayerState(bytes.data(), bytes.size(), back) == SaveStatus::Ok,
		  "player round-trip ok");
	CHECK(back.x == state.x && back.y == state.y && back.z == state.z && back.yaw == state.yaw &&
			  back.pitch == state.pitch && back.flight == state.flight &&
			  back.selectedBlock == state.selectedBlock,
		  "player fields round-trip");

	CHECK(worldsave::decodePlayerState(bytes.data(), 20, back) == SaveStatus::Corrupt,
		  "player truncation rejected");
	auto extended = bytes;
	extended.push_back(0);
	CHECK(worldsave::decodePlayerState(extended.data(), extended.size(), back) == SaveStatus::Corrupt,
		  "player trailing garbage rejected");
	auto bad = bytes;
	bad[0] ^= 0xFF;
	CHECK(worldsave::decodePlayerState(bad.data(), bad.size(), back) == SaveStatus::BadMagic,
		  "player bad magic");
	bad = bytes;
	bad[4] = 99;
	CHECK(worldsave::decodePlayerState(bad.data(), bad.size(), back) == SaveStatus::UnsupportedVersion,
		  "player unsupported version");
	// NaN y and out-of-range y.
	auto nanState = samplePlayer(std::nan(""));
	auto nanBytes = worldsave::encodePlayerState(nanState);
	CHECK(worldsave::decodePlayerState(nanBytes.data(), nanBytes.size(), back) == SaveStatus::Corrupt,
		  "player NaN rejected");
	auto highBytes = worldsave::encodePlayerState(samplePlayer(CHUNK_HEIGHT + 100.0));
	CHECK(worldsave::decodePlayerState(highBytes.data(), highBytes.size(), back) == SaveStatus::Corrupt,
		  "player y above bound rejected");
	auto boundBytes = worldsave::encodePlayerState(samplePlayer(CHUNK_HEIGHT + 64.0));
	CHECK(worldsave::decodePlayerState(boundBytes.data(), boundBytes.size(), back) == SaveStatus::Ok,
		  "player y at upper bound accepted");
}

// ---------------------------------------------------------------------------
// 3. Commit / recover basics + retention + legacy mode.
// ---------------------------------------------------------------------------
void testCommitRecoverBasics()
{
	TempRoot root("basics");
	const fs::path world = root.path / "world";
	fs::create_directories(world);
	makeLiveChunks(world);

	CheckpointStore store(world);
	std::string error;
	CHECK(store.recover(error) == CheckpointStore::RecoveryStatus::Legacy,
		  "no checkpoints -> legacy mode");
	CHECK(store.activeGeneration() == 0 && store.activeDir().empty(), "legacy store inactive");

	CHECK(store.commit(world / "chunks", makePayloads(true), error), "first commit succeeds");
	CHECK(store.activeGeneration() == 1, "first generation is 1");
	CHECK(store.activePlayerPresent(), "explicit player payload present");
	CHECK(fs::exists(worldsave::checkpointManifestPath(world)), "manifest published");
	CHECK(fs::exists(store.activeDir() / "chunks" / "0_0.chunk"), "chunk copied into generation");
	CHECK(fs::exists(store.activeDir() / "chunks" / "-1_2.chunk"), "negative-coord chunk copied");

	// A fresh store (simulated restart) recovers generation 1.
	CheckpointStore re(world);
	CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered, "restart recovers gen 1");
	CHECK(re.activeGeneration() == 1, "recovered generation 1");

	// Second commit: generation 2, fallback 1; the committed gen-1 payloads
	// are IMMUTABLE (byte-compare before/after).
	const auto gen1Inventory = readBytes(worldsave::checkpointGenerationDir(world, 1) / "inventory.state");
	const auto gen1Chunk = readBytes(worldsave::checkpointGenerationDir(world, 1) / "chunks" / "0_0.chunk");
	CHECK(store.commit(world / "chunks", makePayloads(true, 5, 9), error), "second commit succeeds");
	CHECK(store.activeGeneration() == 2, "second generation is 2");
	CHECK(readBytes(worldsave::checkpointGenerationDir(world, 1) / "inventory.state") == gen1Inventory,
		  "committed gen-1 inventory untouched by gen-2 commit");
	CHECK(readBytes(worldsave::checkpointGenerationDir(world, 1) / "chunks" / "0_0.chunk") == gen1Chunk,
		  "committed gen-1 chunk untouched by gen-2 commit");
	CHECK(fs::exists(worldsave::checkpointManifestPrevPath(world)), "previous manifest rotated");

	// Recovery selects the LATEST valid generation.
	CheckpointStore re2(world);
	CHECK(re2.recover(error) == CheckpointStore::RecoveryStatus::Recovered && re2.activeGeneration() == 2,
		  "restart recovers the latest generation");

	// Retention: a third commit cleans generation 1 (keeps 2 + 3).
	CHECK(store.commit(world / "chunks", makePayloads(false), error), "third commit succeeds");
	CHECK(countGenerationDirs(world) == 2, "retention keeps current + previous only");
	CHECK(!fs::exists(worldsave::checkpointGenerationDir(world, 1)), "generation 1 cleaned");
	CHECK(fs::exists(worldsave::checkpointGenerationDir(world, 2)) &&
			  fs::exists(worldsave::checkpointGenerationDir(world, 3)),
		  "generations 2 + 3 retained");
	// The third commit carried the player forward from gen 2 (no explicit player).
	CheckpointStore re3(world);
	CHECK(re3.recover(error) == CheckpointStore::RecoveryStatus::Recovered &&
			  re3.activeGeneration() == 3 && re3.activePlayerPresent(),
		  "carry-forward player present after no-player commit");

	// Legacy files are never touched by the store.
	const auto legacyMeta = world / "world.meta";
	writeBytes(legacyMeta, {1, 2, 3, 4});
	CheckpointStore legacyProbe(world);
	legacyProbe.recover(error);
	CHECK(readBytes(legacyMeta) == std::vector<uint8_t>({1, 2, 3, 4}), "legacy files untouched");
}

// ---------------------------------------------------------------------------
// 4. Fault injection BEFORE publication (points 1-5, 7, 8): the previous
//    complete generation survives; a first-commit fault leaves legacy mode.
// ---------------------------------------------------------------------------
void testPrePublicationFaults()
{
	const CheckpointFaultPoint points[] = {
		CheckpointFaultPoint::BeforeStaging,
		CheckpointFaultPoint::AfterPlayerStaged,
		CheckpointFaultPoint::AfterInventoryStaged,
		CheckpointFaultPoint::AfterItemsStaged,
		CheckpointFaultPoint::DuringChunkCopy,
		CheckpointFaultPoint::AfterStagingBeforePublish,
		CheckpointFaultPoint::DuringManifestPublish,
	};
	for (const auto point : points)
	{
		TempRoot root("fault");
		const fs::path world = root.path / "world";
		fs::create_directories(world);
		makeLiveChunks(world);
		CheckpointStore store(world);
		std::string error;

		// Establish generation 1 (the previous complete state).
		CHECK(store.commit(world / "chunks", makePayloads(true, 2, 3), error),
			  "fault fixture: gen 1 committed");
		const auto gen1Manifest = readBytes(worldsave::checkpointManifestPath(world));

		// Faulted second commit: must fail and leave gen 1 authoritative.
		store.setFaultForTests(point);
		CHECK(!store.commit(world / "chunks", makePayloads(true, 7, 11), error),
			  "faulted commit fails");
		CHECK(!error.empty(), "fault reported");
		CHECK(readBytes(worldsave::checkpointManifestPath(world)) == gen1Manifest,
			  "manifest unchanged by a pre-publication fault");
		store.setFaultForTests(CheckpointFaultPoint::None);

		// Recovery (fresh store = simulated crash restart) yields gen 1 with
		// its ORIGINAL payloads — never the faulted staging content.
		CheckpointStore re(world);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered,
			  "fault recovery: previous generation recovered");
		CHECK(re.activeGeneration() == 1, "fault recovery: generation 1, not the orphan");
		worldsave::InventoryPersistState inv;
		auto invBytes = readBytes(re.activeDir() / "inventory.state");
		CHECK(worldsave::decodeInventoryState(invBytes.data(), invBytes.size(), inv) == SaveStatus::Ok &&
				  inv.selectedSlot == 2,
			  "fault recovery: gen-1 inventory content (not the faulted capture)");

		// Retryability: the next commit reuses the orphaned number and succeeds.
		CHECK(re.commit(world / "chunks", makePayloads(true, 7, 11), error),
			  "retry after fault succeeds");
		CHECK(re.activeGeneration() == 2, "retry reuses the orphaned generation number");
		CheckpointStore re2(world);
		CHECK(re2.recover(error) == CheckpointStore::RecoveryStatus::Recovered &&
				  re2.activeGeneration() == 2,
			  "retry recovery sees the new generation");
	}

	// First-commit faults: no previous checkpoint exists -> legacy mode,
	// nothing authoritative created, retry works.
	for (const auto point : {CheckpointFaultPoint::BeforeStaging, CheckpointFaultPoint::DuringChunkCopy,
							 CheckpointFaultPoint::DuringManifestPublish})
	{
		TempRoot root("firstfault");
		const fs::path world = root.path / "world";
		fs::create_directories(world);
		makeLiveChunks(world);
		CheckpointStore store(world);
		std::string error;
		store.setFaultForTests(point);
		CHECK(!store.commit(world / "chunks", makePayloads(true), error), "first-commit fault fails");
		store.setFaultForTests(CheckpointFaultPoint::None);
		CheckpointStore re(world);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Legacy,
			  "first-commit fault leaves legacy mode (no partial checkpoint)");
		CHECK(re.commit(world / "chunks", makePayloads(true), error), "first-commit retry succeeds");
	}
}

// ---------------------------------------------------------------------------
// 5. Fault injection AFTER publication (points 9, 10): the NEW generation is
//    durable; cleanup is retryable and never invalidates the commit.
// ---------------------------------------------------------------------------
void testPostPublicationFaults()
{
	// Point 9: crash immediately after the manifest rename.
	{
		TempRoot root("postpublish");
		const fs::path world = root.path / "world";
		fs::create_directories(world);
		makeLiveChunks(world);
		CheckpointStore store(world);
		std::string error;
		CHECK(store.commit(world / "chunks", makePayloads(true), error), "gen 1 committed");
		store.setFaultForTests(CheckpointFaultPoint::AfterManifestPublish);
		CHECK(store.commit(world / "chunks", makePayloads(true, 6, 8), error),
			  "point-9 commit IS durable (returns true)");
		store.setFaultForTests(CheckpointFaultPoint::None);
		CheckpointStore re(world);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered &&
				  re.activeGeneration() == 2,
			  "point-9 recovery yields the NEW generation");
		auto invBytes = readBytes(re.activeDir() / "inventory.state");
		worldsave::InventoryPersistState inv;
		CHECK(worldsave::decodeInventoryState(invBytes.data(), invBytes.size(), inv) ==
					  SaveStatus::Ok &&
				  inv.selectedSlot == 6,
			  "point-9 recovery carries the new payload");
		// Cleanup has not run (the crash skipped it): the retained pair
		// (current + previous) is exactly what cleanup preserves.
		CHECK(countGenerationDirs(world) == 2, "point-9 cleanup skipped");
		re.cleanup(error);
		CHECK(error.empty() && countGenerationDirs(world) == 2,
			  "cleanup after point 9 keeps the retained pair (current + fallback)");
		CHECK(fs::exists(worldsave::checkpointGenerationDir(world, 1)),
			  "the fallback generation is retained for recovery");
	}

	// Point 10: fault during obsolete-generation cleanup — the commit is
	// durable, cleanup made partial progress and is retryable.
	{
		TempRoot root("cleanupfault");
		const fs::path world = root.path / "world";
		fs::create_directories(world);
		makeLiveChunks(world);
		CheckpointStore store(world);
		std::string error;
		for (int i = 0; i < 3; ++i)
			CHECK(store.commit(world / "chunks", makePayloads(true), error), "setup commit");
		// Retention kept generations 2+3. An orphaned staging directory for
		// generation 4 (a failed commit) gives cleanup two removal candidates.
		store.setFaultForTests(CheckpointFaultPoint::AfterStagingBeforePublish);
		CHECK(!store.commit(world / "chunks", makePayloads(true), error), "orphan staging created");
		CHECK(countGenerationDirs(world) == 3, "orphan + retained pair present");
		store.setFaultForTests(CheckpointFaultPoint::DuringCleanup);
		CHECK(store.commit(world / "chunks", makePayloads(true, 4, 6), error),
			  "point-10 commit IS durable");
		CHECK(!error.empty(), "point-10 cleanup failure reported (non-fatal)");
		store.setFaultForTests(CheckpointFaultPoint::None);
		CheckpointStore re(world);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered &&
				  re.activeGeneration() == 4,
			  "point-10 recovery yields the NEW generation");
		re.cleanup(error);
		CHECK(error.empty(), "cleanup retry succeeds");
		CHECK(countGenerationDirs(world) == 2, "cleanup retry restores retention");
	}
}

// ---------------------------------------------------------------------------
// 6. Manifest corruption + generation validation + fallback selection.
// ---------------------------------------------------------------------------
void testRecoveryValidation()
{
	TempRoot root("validation");
	const fs::path world = root.path / "world";
	fs::create_directories(world);
	makeLiveChunks(world);
	CheckpointStore store(world);
	std::string error;
	CHECK(store.commit(world / "chunks", makePayloads(true, 2, 3), error), "gen 1 committed");
	CHECK(store.commit(world / "chunks", makePayloads(true, 5, 9), error), "gen 2 committed");

	// (a) Corrupt the live manifest -> manifest.prev recovers generation 1.
	corruptByte(worldsave::checkpointManifestPath(world), 12);
	{
		CheckpointStore re(world);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered,
			  "corrupt manifest falls back to manifest.prev");
		CHECK(re.activeGeneration() == 1, "manifest.prev names generation 1");
		CHECK(!error.empty(), "fallback is reported");
	}
	// Restore the manifest for the next cases (copy prev back is NOT valid —
	// prev names gen 1; instead re-commit from a store recovered via prev).
	// Simpler: rebuild the fixture for each sub-case.
	auto freshPair = [&](const fs::path &w) {
		fs::create_directories(w);
		makeLiveChunks(w);
		CheckpointStore s(w);
		std::string e;
		CHECK(s.commit(w / "chunks", makePayloads(true, 2, 3), e), "fixture gen 1");
		CHECK(s.commit(w / "chunks", makePayloads(true, 5, 9), e), "fixture gen 2");
		return s;
	};

	// (b) Corrupt gen-2 items.state -> whole generation invalid -> gen 1.
	{
		const fs::path w = root.path / "case-b";
		freshPair(w);
		corruptByte(worldsave::checkpointGenerationDir(w, 2) / "items.state", 30);
		CheckpointStore re(w);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered &&
				  re.activeGeneration() == 1,
			  "corrupt gen-2 payload falls back to gen 1 (never mixed)");
	}
	// (c) Corrupt a gen-2 chunk file (checksum) -> gen 1.
	{
		const fs::path w = root.path / "case-c";
		freshPair(w);
		corruptByte(worldsave::checkpointGenerationDir(w, 2) / "chunks" / "0_0.chunk", 30);
		CheckpointStore re(w);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered &&
				  re.activeGeneration() == 1,
			  "corrupt gen-2 chunk falls back to gen 1");
	}
	// (d) Delete a referenced gen-2 chunk -> file-set mismatch -> gen 1.
	{
		const fs::path w = root.path / "case-d";
		freshPair(w);
		fs::remove(worldsave::checkpointGenerationDir(w, 2) / "chunks" / "-1_2.chunk");
		CheckpointStore re(w);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered &&
				  re.activeGeneration() == 1,
			  "missing referenced chunk falls back to gen 1");
	}
	// (e) Add an UNREFERENCED chunk to gen 2 -> incomplete staging -> gen 1.
	{
		const fs::path w = root.path / "case-e";
		freshPair(w);
		std::vector<worldsave::ChunkEdit> extra{{5, static_cast<uint8_t>(STONE)}};
		CHECK(worldsave::writeChunkFile(worldsave::checkpointGenerationDir(w, 2) / "chunks", 9, 9,
										extra) == SaveStatus::Ok,
			  "extra chunk written");
		CheckpointStore re(w);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered &&
				  re.activeGeneration() == 1,
			  "unreferenced chunk invalidates the generation");
	}
	// (f) Both manifests corrupt, generations intact -> UNRECOVERABLE (never
	// silently mixed with the live working set).
	{
		const fs::path w = root.path / "case-f";
		freshPair(w);
		corruptByte(worldsave::checkpointManifestPath(w), 12);
		corruptByte(worldsave::checkpointManifestPrevPath(w), 12);
		CheckpointStore re(w);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Unrecoverable,
			  "both manifests corrupt -> unrecoverable");
		CHECK(!error.empty(), "unrecoverable corruption reported explicitly");
		CHECK(re.activeGeneration() == 0, "no generation selected on unrecoverable");
	}
	// (g) Both manifests corrupt AND no generation dirs -> legacy (an empty
	// checkpoints shell is not a migrated world).
	{
		const fs::path w = root.path / "case-g";
		fs::create_directories(worldsave::checkpointsDir(w));
		writeBytes(worldsave::checkpointManifestPath(w), {0, 1, 2});
		CheckpointStore re(w);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Legacy,
			  "manifest shell without generations stays legacy");
	}
	// (h) Gen-1 player payload corrupt with playerPresent -> gen 1 invalid;
	// no fallback (first generation) -> unrecoverable.
	{
		const fs::path w = root.path / "case-h";
		fs::create_directories(w);
		makeLiveChunks(w);
		CheckpointStore s(w);
		CHECK(s.commit(w / "chunks", makePayloads(true), error), "single gen committed");
		corruptByte(worldsave::checkpointGenerationDir(w, 1) / "player.state", 20);
		CheckpointStore re(w);
		CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Unrecoverable,
			  "corrupt sole generation is unrecoverable (no silent fresh fallback)");
	}
}

// ---------------------------------------------------------------------------
// 7. Player carry-forward semantics + payload pre-validation.
// ---------------------------------------------------------------------------
void testCarryForwardAndValidation()
{
	TempRoot root("carry");
	const fs::path world = root.path / "world";
	fs::create_directories(world);
	makeLiveChunks(world);
	CheckpointStore store(world);
	std::string error;

	// No player anywhere: playerPresent false, quiet.
	CHECK(store.commit(world / "chunks", makePayloads(false), error), "no-player commit");
	CHECK(!store.activePlayerPresent(), "absent player flagged");
	CheckpointStore re(world);
	CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered && !re.activePlayerPresent(),
		  "absent player survives recovery");

	// Legacy root player.state carried forward on the next commit.
	CHECK(worldsave::writeFileAtomic(world / "player.state",
									 worldsave::encodePlayerState(samplePlayer(70.0))),
		  "legacy root player written");
	CHECK(store.commit(world / "chunks", makePayloads(false, 4, 5), error), "carry-forward commit");
	CHECK(store.activePlayerPresent(), "root player carried forward");
	auto carried = readBytes(store.activeDir() / "player.state");
	worldsave::PlayerPersistState player;
	CHECK(worldsave::decodePlayerState(carried.data(), carried.size(), player) == SaveStatus::Ok &&
			  player.y == 70.0,
		  "carried player bytes are the legacy payload");

	// Corrupt carry-forward source: the commit REFUSES (never silently drops).
	// Magic-byte corruption: a mantissa flip would stay a finite double and
	// pass the codec (the manifest checksum catches those at recovery time).
	corruptByte(world / "player.state", 0);
	// The active generation now holds a VALID carried player, so the store
	// carries from IT (not the corrupt root) — corrupt the active one too.
	corruptByte(store.activeDir() / "player.state", 0);
	CHECK(!store.commit(world / "chunks", makePayloads(false), error),
		  "corrupt carry-forward source refuses the commit");
	CHECK(!error.empty(), "carry-forward refusal reported");

	// Malformed captured payloads never produce staging.
	CheckpointStore clean(world);
	std::string e2;
	CHECK(clean.recover(e2) == CheckpointStore::RecoveryStatus::Recovered, "fixture recovered");
	CheckpointPayloads bad = makePayloads(true);
	bad.inventory = {0, 1, 2};
	CHECK(!clean.commit(world / "chunks", bad, error), "malformed inventory refused pre-staging");
	bad = makePayloads(true);
	bad.items.clear();
	CHECK(!clean.commit(world / "chunks", bad, error), "malformed items refused pre-staging");
	bad = makePayloads(true);
	bad.player = {9, 9, 9};
	CHECK(!clean.commit(world / "chunks", bad, error), "malformed player refused pre-staging");
}

// ---------------------------------------------------------------------------
// 8. Empty chunk set + live-set evolution between commits.
// ---------------------------------------------------------------------------
void testChunkSetEvolution()
{
	TempRoot root("chunks");
	const fs::path world = root.path / "world";
	fs::create_directories(world);
	CheckpointStore store(world);
	std::string error;

	// No live chunks at all (a fresh world with no edits): valid generation.
	CHECK(store.commit(world / "chunks", makePayloads(true), error), "empty-chunk commit");
	CheckpointStore re(world);
	CHECK(re.recover(error) == CheckpointStore::RecoveryStatus::Recovered, "empty-chunk recovery");

	// Edits appear: the next commit snapshots the new live set.
	makeLiveChunks(world);
	CHECK(store.commit(world / "chunks", makePayloads(true), error), "second commit with chunks");
	CheckpointStore re2(world);
	CHECK(re2.recover(error) == CheckpointStore::RecoveryStatus::Recovered &&
			  fs::exists(re2.activeDir() / "chunks" / "0_0.chunk"),
		  "evolved live set snapshotted");

	// An edit reverts (live file removed): the next generation drops it.
	fs::remove(world / "chunks" / "0_0.chunk");
	CHECK(store.commit(world / "chunks", makePayloads(true), error), "revert commit");
	CheckpointStore re3(world);
	CHECK(re3.recover(error) == CheckpointStore::RecoveryStatus::Recovered &&
			  !fs::exists(re3.activeDir() / "chunks" / "0_0.chunk") &&
			  fs::exists(re3.activeDir() / "chunks" / "-1_2.chunk"),
		  "reverted chunk absent from the new generation (copy-on-write semantics)");
}

// ---------------------------------------------------------------------------
// 9. Runtime integration (stabilization PR5b): WorldPersistence recovery
//    re-seeds the LIVE working set from the committed generation, so a crash
//    after an uncommitted flush never resurrects pre-crash edits through the
//    next checkpoint. ~WorldPersistence does NOT commit (only the VoxelWorld/
//    GameSimulation close seams do) — destroying it simulates the crash.
// ---------------------------------------------------------------------------
void testLiveReseedIsolation()
{
	TempRoot root("reseed");
	const fs::path saves = root.path;
	const uint32_t genVersion = 1;
	const uint32_t idx1 = 8 + 64 * 256 + 8 * 16;
	const uint32_t idx2 = 9 + 64 * 256 + 8 * 16;

	{
		WorldPersistence wp;
		auto info = wp.openOrCreate(saves, "reseed", 42, genVersion);
		CHECK(info.ok, "reseed fixture open");
		// E1: captured, flushed, COMMITTED (generation 1).
		wp.captureChunkEdits(0, 0, {{idx1, static_cast<uint8_t>(STONE)}});
		CHECK(wp.flush(), "E1 flushed");
		worldsave::CheckpointPayloads payloads;
		payloads.inventory = worldsave::encodeInventoryState(worldsave::InventoryPersistState{});
		payloads.items = worldsave::encodeItemsState(worldsave::ItemsPersistState{});
		CHECK(wp.commitCheckpoint(payloads), "E1 committed as generation 1");
		CHECK(wp.checkpointMode() && wp.checkpointGeneration() == 1, "checkpoint mode active");
		// E2: captured + flushed (live advances) but NEVER committed.
		wp.captureChunkEdits(0, 0, {{idx2, static_cast<uint8_t>(DIRT)}});
		CHECK(wp.flush(), "E2 flushed to the live set");
		std::vector<worldsave::ChunkEdit> live;
		CHECK(worldsave::readChunkFile(saves / "reseed" / "chunks" / "0_0.chunk", 0, 0, live) ==
				  SaveStatus::Ok,
			  "live file holds E1+E2 before the crash");
		CHECK(live.size() == 2, "live set advanced past the checkpoint");
		// Simulated crash: the persistence is destroyed WITHOUT a close
		// commit (~WorldPersistence only shuts the save service down).
	}

	// Recovery: generation 1 is authoritative; the live set is re-seeded to
	// it — the uncommitted E2 must be GONE (the next commit never resurrects it).
	WorldPersistence re;
	auto info = re.openOrCreate(saves, "reseed", 42, genVersion);
	CHECK(info.ok, "recovery open succeeds");
	CHECK(re.checkpointMode() && re.checkpointGeneration() == 1, "recovered generation 1");
	const auto snap = re.overridesSnapshot(0, 0);
	bool hasE1 = false;
	bool hasE2 = false;
	for (const auto &e : snap)
	{
		if (e.localIndex == idx1 && e.blockType == static_cast<uint8_t>(STONE))
			hasE1 = true;
		if (e.localIndex == idx2)
			hasE2 = true;
	}
	CHECK(hasE1 && !hasE2, "recovery re-seed discarded the uncommitted E2 (never mixed)");
	std::vector<worldsave::ChunkEdit> live;
	CHECK(worldsave::readChunkFile(saves / "reseed" / "chunks" / "0_0.chunk", 0, 0, live) ==
			  SaveStatus::Ok,
		  "live file re-seeded");
	CHECK(live.size() == 1 && live[0].localIndex == idx1, "live set matches the committed generation");

	// A new commit after recovery snapshots exactly the recovered state.
	worldsave::CheckpointPayloads payloads;
	payloads.inventory = worldsave::encodeInventoryState(worldsave::InventoryPersistState{});
	payloads.items = worldsave::encodeItemsState(worldsave::ItemsPersistState{});
	CHECK(re.commitCheckpoint(payloads), "post-recovery commit");
	CHECK(re.checkpointGeneration() == 2, "generation advanced");
	std::vector<worldsave::ChunkEdit> gen2;
	CHECK(worldsave::readChunkFile(re.checkpointGeneration() == 2
									   ? saves / "reseed" / "checkpoints" / "generation-2" / "chunks" / "0_0.chunk"
									   : fs::path(),
								   0, 0, gen2) == SaveStatus::Ok,
		  "generation 2 chunk readable");
	CHECK(gen2.size() == 1 && gen2[0].localIndex == idx1, "generation 2 carries only the recovered E1");
}

// ---------------------------------------------------------------------------
// 10. First-migration failure boundary (PR #243 review, finding 2): the
//     transactional recovery guarantee begins at the FIRST SUCCESSFUL
//     checkpoint commit. A legacy world whose first publication fails has no
//     committed checkpoint: recovery uses the legacy per-file policy (durable
//     files preserved, never destroyed) and makes NO transactional claim.
//     Once a commit succeeds, recovery is transactional and a corrupt
//     generation is never silently downgraded to the live set.
// ---------------------------------------------------------------------------
void testFirstMigrationFailureBoundary()
{
	TempRoot root("first-migration");
	const fs::path saves = root.path;
	const uint32_t genVersion = 1;
	const uint32_t idx1 = 8 + 64 * 256 + 8 * 16;
	const uint32_t idx2 = 9 + 64 * 256 + 8 * 16;
	worldsave::CheckpointPayloads payloads;
	payloads.inventory = worldsave::encodeInventoryState(worldsave::InventoryPersistState{});
	payloads.items = worldsave::encodeItemsState(worldsave::ItemsPersistState{});

	// (a) Legacy edits flushed, first publication faulted, crash: reopen is
	// LEGACY (no committed checkpoint), durable edits intact, nothing destroyed.
	{
		WorldPersistence wp;
		CHECK(wp.openOrCreate(saves, "legacy", 42, genVersion).ok, "legacy open");
		CHECK(!wp.checkpointMode(), "a fresh world starts in legacy mode");
		wp.captureChunkEdits(0, 0, {{idx1, static_cast<uint8_t>(STONE)}});
		wp.captureChunkEdits(1, 0, {{idx2, static_cast<uint8_t>(DIRT)}});
		CHECK(wp.flush(), "legacy working set flushed");
		wp.setCheckpointFaultForTests(worldsave::CheckpointFaultPoint::DuringManifestPublish);
		CHECK(!wp.commitCheckpoint(payloads), "first publication fails before the manifest rename");
		// Simulated crash: ~WorldPersistence does not commit.
	}
	{
		WorldPersistence re;
		CHECK(re.openOrCreate(saves, "legacy", 42, genVersion).ok, "reopen after failed first migration");
		CHECK(!re.checkpointMode(), "no committed checkpoint -> legacy recovery (guarantee boundary)");
		CHECK(!fs::exists(saves / "legacy" / "checkpoints" / "manifest"), "no manifest published");
		const auto snap0 = re.overridesSnapshot(0, 0);
		const auto snap1 = re.overridesSnapshot(1, 0);
		CHECK(snap0.size() == 1 && snap0[0].localIndex == idx1, "durable legacy edit (0,0) preserved");
		CHECK(snap1.size() == 1 && snap1[0].localIndex == idx2, "durable legacy edit (1,0) preserved");
		// The first SUCCESSFUL commit establishes the transactional baseline.
		// With no manifest, recovery numbers PAST the orphaned staging (the
		// orphan is cleaned by retention), so the first committed generation
		// is 2 here — the number is an internal monotonic counter.
		CHECK(re.commitCheckpoint(payloads), "first successful commit");
		CHECK(re.checkpointMode() && re.checkpointGeneration() == 2,
			  "transactional guarantees begin here");
		CHECK(countGenerationDirs(saves / "legacy") == 1, "orphaned staging cleaned by retention");
	}
	// From now on recovery is transactional: a corrupt sole generation is
	// UNRECOVERABLE (never a silent downgrade to the live working set).
	{
		const fs::path gen = saves / "legacy" / "checkpoints" / "generation-2";
		corruptByte(gen / "inventory.state", 0);
		WorldPersistence re;
		const auto info = re.openOrCreate(saves, "legacy", 42, genVersion);
		CHECK(!info.ok, "migrated world with a corrupt generation refuses the open");
		CHECK(info.error.find("unrecoverable") != std::string::npos, "explicit unrecoverable error");
	}

	// (b) Partial legacy flush (one coordinate's write fails) + crash BEFORE
	// any commit: legacy recovery exposes exactly the per-file durable state
	// — the documented pre-migration policy, not a transactional snapshot.
	{
		WorldPersistence wp;
		CHECK(wp.openOrCreate(saves, "partial", 42, genVersion).ok, "partial-flush world open");
		wp.captureChunkEdits(0, 0, {{idx1, static_cast<uint8_t>(STONE)}});
		wp.captureChunkEdits(1, 0, {{idx2, static_cast<uint8_t>(DIRT)}});
		wp.setWriteTmpFnForTests([](const fs::path &dir, int32_t x, int32_t z,
									const std::vector<worldsave::ChunkEdit> &ov, fs::path &out) {
			if (x == 1)
				return SaveStatus::IoError; // coordinate (1,0) never becomes durable
			return worldsave::writeChunkFileTmp(dir, x, z, ov, out);
		});
		CHECK(!wp.flush(), "partial flush reports failure");
		// The runtime seams never commit after a failed flush (finding 1);
		// the crash leaves the legacy per-file state.
	}
	{
		WorldPersistence re;
		CHECK(re.openOrCreate(saves, "partial", 42, genVersion).ok, "partial-flush reopen");
		CHECK(!re.checkpointMode(), "still legacy: no checkpoint was ever committed");
		const auto snap0 = re.overridesSnapshot(0, 0);
		const auto snap1 = re.overridesSnapshot(1, 0);
		CHECK(snap0.size() == 1 && snap0[0].localIndex == idx1, "durable coordinate recovered");
		CHECK(snap1.empty(), "never-durable coordinate absent (per-file legacy policy)");
	}
}

void testMultiplayerGeneration()
{
	TempRoot temp("roster");
	const auto live = temp.path / "chunks";
	makeLiveChunks(temp.path);
	constexpr uint64_t worldId = 51;
	worldsave::PlayerRoster roster{worldId, {
		{{2}, samplePlayer(), sampleInventory(2), {1.5, -2, 0}},
		{{4}, samplePlayer(90), sampleInventory(7), {0, 0, 0}}
	}};
	const auto priorRoster = worldsave::encodePlayerRoster(roster);
	worldsave::PlayerRoster decoded;
	CHECK_EXPR(worldsave::decodePlayerRoster(priorRoster.data(), priorRoster.size(), decoded) == SaveStatus::Ok);
	CHECK_EXPR(decoded.worldId == worldId && decoded.players.size() == 2);
	if (decoded.players.size() != 2)
		return;
	CHECK_EXPR(decoded.players[0].id.value == 2 && decoded.players[0].velocity[1] == -2);
	CHECK_EXPR(decoded.players[1].id.value == 4 && decoded.players[1].inventory.selectedSlot == 7);
	CHECK_EXPR(worldsave::decodePlayerRoster(priorRoster.data(), priorRoster.size() - 1, decoded) == SaveStatus::Corrupt);
	auto duplicate = roster;
	duplicate.players[1].id = duplicate.players[0].id;
	const auto bad = worldsave::encodePlayerRoster(duplicate);
	CHECK_EXPR(worldsave::decodePlayerRoster(bad.data(), bad.size(), decoded) == SaveStatus::Corrupt);

	std::string error;
	auto prior = makePayloads(true);
	prior.roster = priorRoster;
	CheckpointStore store(temp.path);
	CHECK(store.commit(live, prior, error), error);
	const auto priorDir = store.activeDir();
	const auto originalChunk = readBytes(priorDir / "chunks" / "0_0.chunk");

	// Simulate a pickup into one actor, a selection change on the other,
	// and a chunk edit at the same capture boundary.
	roster.players[0].inventory.slots[1] = {static_cast<uint16_t>(items::ItemId::Cobblestone), 1};
	roster.players[1].inventory.selectedSlot = 8;
	auto update = prior;
	update.roster = worldsave::encodePlayerRoster(roster);
	update.items = worldsave::encodeItemsState({3, {}});
	CHECK_EXPR(worldsave::writeChunkFile(live, 0, 0, {{314, static_cast<uint8_t>(GLASS)}}) == SaveStatus::Ok);
	const auto verifyPrior = [&] {
		CheckpointStore recovered(temp.path);
		CHECK(recovered.recover(error) == CheckpointStore::RecoveryStatus::Recovered, error);
		CHECK_EXPR(recovered.activeWorldId() == worldId && recovered.activeDir() == priorDir);
		CHECK_EXPR(readBytes(recovered.activeDir() / worldsave::kRosterFile) == priorRoster);
		CHECK_EXPR(readBytes(recovered.activeDir() / "items.state") == prior.items);
		CHECK_EXPR(readBytes(recovered.activeDir() / "chunks" / "0_0.chunk") == originalChunk);
	};
	for (const auto fault : {CheckpointFaultPoint::AfterItemsStaged, CheckpointFaultPoint::AfterRosterStaged,
							 CheckpointFaultPoint::DuringChunkCopy, CheckpointFaultPoint::DuringManifestPublish})
	{
		store.setFaultForTests(fault);
		CHECK_EXPR(!store.commit(live, update, error));
		verifyPrior();
	}
	store.setFaultForTests(CheckpointFaultPoint::AfterManifestPublish);
	CHECK(store.commit(live, update, error), error);
	CheckpointStore recovered(temp.path);
	CHECK(recovered.recover(error) == CheckpointStore::RecoveryStatus::Recovered, error);
	CHECK_EXPR(readBytes(recovered.activeDir() / worldsave::kRosterFile) == update.roster);
	CHECK_EXPR(readBytes(recovered.activeDir() / "items.state") == update.items);
	CHECK_EXPR(readBytes(recovered.activeDir() / "chunks" / "0_0.chunk") != originalChunk);

	corruptByte(recovered.activeDir() / worldsave::kRosterFile, 25);
	verifyPrior();
	CHECK(recovered.recover(error) == CheckpointStore::RecoveryStatus::Recovered, error);
	recovered.setFaultForTests(CheckpointFaultPoint::DuringManifestPublish);
	CHECK_EXPR(!recovered.commit(live, update, error));
	verifyPrior();
}

} // namespace

int main()
{
	std::printf("test_world_checkpoint: CPU-only checkpoint format/recovery acceptance (no GPU)\n");
	testManifestCodec();
	testPlayerCodec();
	testCommitRecoverBasics();
	testPrePublicationFaults();
	testPostPublicationFaults();
	testRecoveryValidation();
	testCarryForwardAndValidation();
	testChunkSetEvolution();
	testLiveReseedIsolation();
	testFirstMigrationFailureBoundary();
	testMultiplayerGeneration();
	std::printf("%ld checks, %ld testmain::fails\n", testmain::checks, testmain::fails);
	if (testmain::fails == 0)
		std::printf("PASS\n");
	return testmain::exitCode();
}
