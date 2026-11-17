#include <World/WorldSave.hpp>

#include <Items/ItemEntitySystem.hpp>
#include <Items/ItemRegistry.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <system_error>

namespace worldsave
{
	// Format geometry pins (issue #228): the save-layer constants must stay
	// aligned with the item domain (the biomeTypeString/kBlockLayers
	// discipline). The registry include is CPU-only — the save layer stays
	// sim/render-free (CoreIncludeGuard).
	static_assert(kInventorySlotCount == static_cast<std::size_t>(items::kInventorySize),
	              "inventory.state slot count must match items::kInventorySize");
	static_assert(kInventoryStateBytes == 153, "inventory.state is exactly 153 bytes");
	static_assert(kItemsStateHeaderBytes == 20, "items.state header is exactly 20 bytes");
	static_assert(kItemEntityRecordBytes == 68, "items.state record is exactly 68 bytes");
	namespace
	{
		bool parseI32Strict(std::string_view text, int32_t &out)
		{
			if (text.empty())
				return false;
			const char *first = text.data();
			const char *last = first + text.size();
			int32_t value = 0;
			const std::from_chars_result result = std::from_chars(first, last, value);
			if (result.ec != std::errc() || result.ptr != last)
				return false;
			out = value;
			return true;
		}

	} // namespace
	// Chunk-file geometry lives in the header (worldsave::kChunkHeaderBytes /
	// kChunkRecordBytes) so the persistence accounting cannot drift from it.
	static_assert(kChunkHeaderBytes == 28, "chunk file header is exactly 28 bytes");
	static_assert(kChunkRecordBytes == 5, "chunk file record is exactly 5 bytes");

	// Little-endian byte helpers (ByteWriter/ByteReader) live in the header:
	// sibling formats such as player.state share them instead of duplicating
	// the byte-order discipline.

	// Writes `bytes` to finalPath.tmp, then renames over finalPath (atomic
	// replace; see the header's atomic-write guarantee). Durability scope
	// (review §4.3): tmp+rename is atomic against a PROCESS crash — a reader
	// sees either the old file or the new one, never a torn write. It is not
	// a power-loss guarantee: neither the file data nor the directory entry
	// is fsynced, so a machine power cut may lose the rename. Saves are
	// re-derivable from seed + the desired/durable state machine, which
	// retries failed coordinates on the next capture/flush.
	bool writeFileAtomic(const std::filesystem::path &finalPath, const std::vector<uint8_t> &bytes)
	{
		std::error_code ec;
		if (!finalPath.parent_path().empty())
		{
			std::filesystem::create_directories(finalPath.parent_path(), ec);
			if (ec)
				return false;
		}
		std::filesystem::path tmp = finalPath;
		tmp += ".tmp";
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out)
				return false;
			if (!bytes.empty())
				out.write(reinterpret_cast<const char *>(bytes.data()),
				          static_cast<std::streamsize>(bytes.size()));
			out.flush();
			if (!out.good())
			{
				out.close();
				std::error_code removeEc;
				std::filesystem::remove(tmp, removeEc);
				return false;
			}
		}
		std::filesystem::rename(tmp, finalPath, ec);
		if (ec)
		{
			std::error_code removeEc;
			std::filesystem::remove(tmp, removeEc);
			return false;
		}
		return true;
	}

	SaveStatus readFileInto(const std::filesystem::path &path, std::vector<uint8_t> &out)
	{
		// Classify by the status TYPE, not by the error_code: MSVC's EC
		// overloads also report the not-found condition through `ec`,
		// while libstdc++/libc++ clear it there. not_found means the
		// path is absent (the common "no save data yet" case); any other
		// status or a hard error is a real IO error.
		std::error_code ec;
		const std::filesystem::file_status st = std::filesystem::status(path, ec);
		if (st.type() == std::filesystem::file_type::not_found)
			return SaveStatus::NotFound;
		if (ec || st.type() != std::filesystem::file_type::regular)
			return SaveStatus::IoError;
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return SaveStatus::IoError;
		in.seekg(0, std::ios::end);
		const std::streampos end = in.tellg();
		if (end < 0)
			return SaveStatus::IoError;
		in.seekg(0, std::ios::beg);
		out.resize(static_cast<size_t>(end));
		if (!out.empty())
		{
			in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(out.size()));
			if (static_cast<size_t>(in.gcount()) != out.size())
				return SaveStatus::IoError;
		}
		return SaveStatus::Ok;
	}

	uint64_t fnv1a64(const void *data, size_t size)
	{
		// FNV offset basis / prime (FNV-1a, 64-bit).
		uint64_t hash = 0xcbf29ce484222325ull;
		if (!data)
			return hash;
		const uint8_t *p = static_cast<const uint8_t *>(data);
		for (size_t i = 0; i < size; ++i)
		{
			hash ^= p[i];
			hash *= 0x100000001b3ull;
		}
		return hash;
	}

	SaveStatus writeWorldMeta(const std::filesystem::path &metaPath, const WorldMeta &meta)
	{
		ByteWriter w;
		w.raw(kWorldMetaMagic, sizeof(kWorldMetaMagic));
		w.u32(meta.formatVersion);
		w.i32(meta.seed);
		w.u32(meta.generatorVersion);
		return writeFileAtomic(metaPath, w.take()) ? SaveStatus::Ok : SaveStatus::IoError;
	}

	SaveStatus readWorldMeta(const std::filesystem::path &metaPath, WorldMeta &outMeta)
	{
		outMeta = WorldMeta{};
		std::vector<uint8_t> bytes;
		const SaveStatus read = readFileInto(metaPath, bytes);
		if (read != SaveStatus::Ok)
			return read;
		ByteReader r(bytes.data(), bytes.size());
		char magic[sizeof(kWorldMetaMagic)];
		r.raw(magic, sizeof(magic));
		outMeta.formatVersion = r.u32();
		outMeta.seed = r.i32();
		outMeta.generatorVersion = r.u32();
		if (r.failed())
			return SaveStatus::Corrupt;
		if (std::memcmp(magic, kWorldMetaMagic, sizeof(magic)) != 0)
			return SaveStatus::BadMagic;
		// Save format v1 has no migrations: anything that is not the exact
		// supported version (older OR newer) is rejected (issue #180 review).
		if (outMeta.formatVersion != kWorldSaveFormatVersion)
			return SaveStatus::UnsupportedVersion;
		return SaveStatus::Ok;
	}

	std::string chunkFileName(int32_t chunkX, int32_t chunkZ)
	{
		std::string name = std::to_string(chunkX);
		name += '_';
		name += std::to_string(chunkZ);
		name += ".chunk";
		return name;
	}

	bool parseChunkFileName(const std::string &fileName, int32_t &chunkX, int32_t &chunkZ)
	{
		chunkX = 0;
		chunkZ = 0;
		constexpr std::string_view kExt = ".chunk";
		if (fileName.size() <= kExt.size())
			return false;
		if (fileName.compare(fileName.size() - kExt.size(), kExt.size(), kExt.data(), kExt.size()) != 0)
			return false;
		const std::string_view stem(fileName.data(), fileName.size() - kExt.size());
		const size_t sep = stem.find('_');
		if (sep == std::string_view::npos)
			return false;
		if (stem.find('_', sep + 1) != std::string_view::npos)
			return false;
		return parseI32Strict(stem.substr(0, sep), chunkX) && parseI32Strict(stem.substr(sep + 1), chunkZ);
	}

	std::filesystem::path chunkTmpPath(const std::filesystem::path &chunksDir, int32_t chunkX, int32_t chunkZ)
	{
		std::filesystem::path path = chunksDir / chunkFileName(chunkX, chunkZ);
		path += ".tmp";
		return path;
	}

	// Shared encoder: exact header + sorted records, or an empty vector when
	// an out-of-range localIndex refuses the payload (Corrupt).
	std::vector<uint8_t> encodeChunkFileBytes(int32_t chunkX, int32_t chunkZ,
	                                          const std::vector<ChunkEdit> &edits)
	{
		// Writer/reader symmetry (issue #180 review round 3): anything the
		// reader would reject as Corrupt is refused HERE, so every payload
		// this API persists reads back Ok.
		if (edits.empty() || edits.size() > static_cast<size_t>(CHUNK_VOLUME))
			return {};
		// Records are stored in ascending localIndex order; callers may pass
		// any order, so sort a local copy.
		std::vector<ChunkEdit> sorted = edits;
		std::sort(sorted.begin(), sorted.end(), [](const ChunkEdit &a, const ChunkEdit &b)
		          { return a.localIndex < b.localIndex; });
		// Duplicate localIndex values cannot be produced by the capture path
		// (the chunk edit map is unique-keyed) and the reader treats them as
		// corruption.
		for (size_t i = 1; i < sorted.size(); ++i)
			if (sorted[i - 1].localIndex == sorted[i].localIndex)
				return {};

		ByteWriter records;
		for (const ChunkEdit &edit : sorted)
		{
			// Out-of-range indices are a caller bug: refuse to persist them
			// instead of writing a payload every future read would reject.
			if (edit.localIndex >= CHUNK_VOLUME)
				return {};
			// Same for block types that no voxel can ever store.
			if (!isPersistableBlockType(edit.blockType))
				return {};
			records.u32(edit.localIndex);
			records.u8(edit.blockType);
		}
		const std::vector<uint8_t> recordBytes = records.take();

		ByteWriter w;
		w.raw(kChunkFileMagic, sizeof(kChunkFileMagic));
		w.u32(kWorldSaveFormatVersion);
		w.i32(chunkX);
		w.i32(chunkZ);
		w.u32(static_cast<uint32_t>(sorted.size()));
		w.u64(fnv1a64(recordBytes.data(), recordBytes.size()));
		w.raw(recordBytes.data(), recordBytes.size());
		return w.take();
	}

	SaveStatus writeChunkFileTmp(const std::filesystem::path &chunksDir, int32_t chunkX,
	                             int32_t chunkZ, const std::vector<ChunkEdit> &edits,
	                             std::filesystem::path &outTmpPath)
	{
		outTmpPath.clear();
		if (edits.empty())
			return SaveStatus::Corrupt; // deletion is a decision, not an encoding
		const std::vector<uint8_t> bytes = encodeChunkFileBytes(chunkX, chunkZ, edits);
		if (bytes.empty())
			return SaveStatus::Corrupt;

		const std::filesystem::path tmpPath = chunkTmpPath(chunksDir, chunkX, chunkZ);
		std::error_code ec;
		if (!chunksDir.empty())
		{
			std::filesystem::create_directories(chunksDir, ec);
			if (ec)
				return SaveStatus::IoError;
		}
		{
			std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
			if (!out)
				return SaveStatus::IoError;
			out.write(reinterpret_cast<const char *>(bytes.data()),
			          static_cast<std::streamsize>(bytes.size()));
			out.flush();
			if (!out.good())
			{
				out.close();
				std::error_code removeEc;
				std::filesystem::remove(tmpPath, removeEc);
				return SaveStatus::IoError;
			}
		}
		outTmpPath = tmpPath;
		return SaveStatus::Ok;
	}

	SaveStatus commitChunkFile(const std::filesystem::path &tmpPath)
	{
		std::filesystem::path finalPath = tmpPath;
		finalPath.replace_extension(); // "<x>_<z>.chunk.tmp" -> "<x>_<z>.chunk"
		std::error_code ec;
		std::filesystem::rename(tmpPath, finalPath, ec);
		if (ec)
		{
			std::error_code removeEc;
			std::filesystem::remove(tmpPath, removeEc);
			return SaveStatus::IoError;
		}
		return SaveStatus::Ok;
	}

	SaveStatus writeChunkFile(const std::filesystem::path &chunksDir, int32_t chunkX, int32_t chunkZ,
	                          const std::vector<ChunkEdit> &edits)
	{
		if (edits.empty())
		{
			// All overrides reverted => the chunk has no payload: the file
			// must not exist (removal is a no-op Ok when it never did).
			return removeChunkFile(chunksDir, chunkX, chunkZ);
		}

		std::filesystem::path tmpPath;
		const SaveStatus write = writeChunkFileTmp(chunksDir, chunkX, chunkZ, edits, tmpPath);
		if (write != SaveStatus::Ok)
			return write;
		return commitChunkFile(tmpPath);
	}

	SaveStatus readChunkFile(const std::filesystem::path &filePath, int32_t expectedChunkX, int32_t expectedChunkZ,
	                         std::vector<ChunkEdit> &outEdits)
	{
		outEdits.clear();
		std::vector<uint8_t> bytes;
		const SaveStatus read = readFileInto(filePath, bytes);
		if (read != SaveStatus::Ok)
			return read;

		ByteReader r(bytes.data(), bytes.size());
		char magic[sizeof(kChunkFileMagic)];
		r.raw(magic, sizeof(magic));
		const uint32_t version = r.u32();
		const int32_t chunkX = r.i32();
		const int32_t chunkZ = r.i32();
		const uint32_t editCount = r.u32();
		const uint64_t storedChecksum = r.u64();
		if (r.failed())
			return SaveStatus::Corrupt;
		if (std::memcmp(magic, kChunkFileMagic, sizeof(magic)) != 0)
			return SaveStatus::BadMagic;
		// Save format v1 has no migrations: exact version or reject.
		if (version != kWorldSaveFormatVersion)
			return SaveStatus::UnsupportedVersion;
		if (chunkX != expectedChunkX || chunkZ != expectedChunkZ)
			return SaveStatus::CoordinateMismatch;

		// Structural sanity (issue #180 review): a chunk cannot carry more
		// overrides than it has voxels, and a zero-record payload is not
		// producible by the writer (reverted chunks delete the file).
		if (editCount == 0 || editCount > static_cast<uint32_t>(CHUNK_VOLUME))
			return SaveStatus::Corrupt;

		// Exact size: the record region must hold exactly editCount records
		// (catches truncation and a tampered count in one check).
		const uint64_t recordBytesNeeded = static_cast<uint64_t>(editCount) * kChunkRecordBytes;
		if (bytes.size() < kChunkHeaderBytes ||
		    recordBytesNeeded != static_cast<uint64_t>(bytes.size() - kChunkHeaderBytes))
			return SaveStatus::Corrupt;
		if (fnv1a64(bytes.data() + kChunkHeaderBytes, static_cast<size_t>(recordBytesNeeded)) != storedChecksum)
			return SaveStatus::Corrupt;

		std::vector<ChunkEdit> edits;
		edits.reserve(editCount);
		for (uint32_t i = 0; i < editCount; ++i)
		{
			const uint32_t localIndex = r.u32();
			const uint8_t blockType = r.u8();
			if (r.failed())
				return SaveStatus::Corrupt;
			if (localIndex >= CHUNK_VOLUME)
				return SaveStatus::Corrupt;
			// blockType must be an encodable voxel value (real texture
			// ordinal or AIR; COUNT itself is never storable).
			if (!isPersistableBlockType(blockType))
				return SaveStatus::Corrupt;
			edits.push_back(ChunkEdit{localIndex, blockType});
		}

		// Stored order is arbitrary by contract; normalize to ascending.
		std::sort(edits.begin(), edits.end(), [](const ChunkEdit &a, const ChunkEdit &b)
		          { return a.localIndex < b.localIndex; });
		// Duplicate localIndex values are corruption: the writer collapses
		// per-voxel state, so two records for one voxel cannot be produced.
		for (size_t i = 0; i + 1 < edits.size(); ++i)
			if (edits[i].localIndex == edits[i + 1].localIndex)
				return SaveStatus::Corrupt;
		outEdits = std::move(edits);
		return SaveStatus::Ok;
	}

	SaveStatus removeChunkFile(const std::filesystem::path &chunksDir, int32_t chunkX, int32_t chunkZ)
	{
		std::error_code ec;
		const std::filesystem::path path = chunksDir / chunkFileName(chunkX, chunkZ);
		std::filesystem::remove(path, ec);
		// Not-exist leaves ec clear and is the desired end state. A failure
		// on a non-regular entry squatting on the name is ignored (there is
		// no authoritative payload behind it); a regular file that survives
		// removal is a real IoError. Classified by status type (see
		// readFileInto for the MSVC error_code quirk).
		if (!ec)
			return SaveStatus::Ok;
		std::error_code probeEc;
		const std::filesystem::file_status st = std::filesystem::status(path, probeEc);
		if (st.type() == std::filesystem::file_type::not_found)
			return SaveStatus::Ok;
		if (!probeEc && st.type() != std::filesystem::file_type::regular)
			return SaveStatus::Ok;
		return SaveStatus::IoError;
	}

	ScanResult scanChunkFiles(const std::filesystem::path &chunksDir)
	{
		ScanResult result;
		std::error_code ec;
		// Explicit status probe (issue #180 review round 2): never use
		// exists()==false as a synonym for "absent" - with a non-zero ec it
		// means "unknown", which must surface as IoError, not as an empty
		// listing.
		const std::filesystem::file_status st = std::filesystem::status(chunksDir, ec);
		// not_found is classified by the file_status, NOT by ec: this STL
		// sets ec (ENOENT) even when it successfully reports not_found.
		// A genuine probe failure reports file_type::none with ec set.
		if (st.type() == std::filesystem::file_type::not_found)
		{
			result.status = SaveStatus::Ok; // fresh world: no saves yet
			return result;
		}
		if (ec || st.type() != std::filesystem::file_type::directory)
		{
			result.status = SaveStatus::IoError;
			return result;
		}
		std::filesystem::directory_iterator it(chunksDir, ec);
		if (ec)
		{
			result.status = SaveStatus::IoError;
			return result;
		}
		for (const std::filesystem::directory_entry &entry : it)
		{
			std::error_code entryEc;
			if (!entry.is_regular_file(entryEc) || entryEc)
				continue;
			int32_t chunkX = 0;
			int32_t chunkZ = 0;
			if (!parseChunkFileName(entry.path().filename().string(), chunkX, chunkZ))
				continue; // ignores non-chunk names and ".tmp" sidecars
			result.files.push_back(entry.path());
		}
		std::sort(result.files.begin(), result.files.end());
		return result;
	}

	SaveStatus cleanTempFiles(const std::filesystem::path &chunksDir)
	{
		std::error_code ec;
		// Same explicit status discipline as scanChunkFiles (issue #180
		// review round 2): missing directory = Ok/no-op; anything else that
		// cannot be iterated is a real I/O error.
		const std::filesystem::file_status st = std::filesystem::status(chunksDir, ec);
		if (st.type() == std::filesystem::file_type::not_found)
			return SaveStatus::Ok;
		if (ec || st.type() != std::filesystem::file_type::directory)
			return SaveStatus::IoError;
		std::filesystem::directory_iterator it(chunksDir, ec);
		if (ec)
			return SaveStatus::IoError;
		SaveStatus status = SaveStatus::Ok;
		for (const std::filesystem::directory_entry &entry : it)
		{
			std::error_code entryEc;
			if (!entry.is_regular_file(entryEc) || entryEc)
				continue;
			const std::string name = entry.path().filename().string();
			constexpr std::string_view kTmpExt = ".chunk.tmp";
			if (name.size() <= kTmpExt.size() ||
			    name.compare(name.size() - kTmpExt.size(), kTmpExt.size(), kTmpExt.data(), kTmpExt.size()) != 0)
				continue;
			std::error_code removeEc;
			std::filesystem::remove(entry.path(), removeEc);
			if (removeEc)
				status = SaveStatus::IoError;
		}
		return status;
	}

	std::vector<ChunkEdit> diffEditsAgainstBase(const uint8_t *baseVoxels, const std::vector<ChunkEdit> &currentValues)
	{
		std::vector<ChunkEdit> out;
		out.reserve(currentValues.size());
		for (const ChunkEdit &edit : currentValues)
		{
			if (edit.localIndex >= CHUNK_VOLUME)
				continue; // cannot diff what is not in the chunk
			if (baseVoxels[edit.localIndex] != edit.blockType)
				out.push_back(edit);
		}
		std::stable_sort(out.begin(), out.end(), [](const ChunkEdit &a, const ChunkEdit &b)
		                 { return a.localIndex < b.localIndex; });
		return out;
	}

	// ------------------------------------------------------------------
	// Gameplay state codecs (issue #228, Epic #223 Phase E) — format spec
	// in WorldSave.hpp. Encode trusts the caller (the facade captures
	// well-formed authoritative state, the writePlayerState pattern);
	// decode is STRICT: every rule is a loud SaveStatus, never a clamp.
	// ------------------------------------------------------------------

	std::vector<uint8_t> encodeInventoryState(const InventoryPersistState &state)
	{
		ByteWriter w;
		w.raw(kInventoryStateMagic, sizeof(kInventoryStateMagic));
		w.u32(kInventoryStateFormatVersion);
		w.u8(state.selectedSlot);
		for (const InventoryPersistState::Slot &slot : state.slots)
		{
			w.u16(slot.itemId);
			w.u16(slot.count);
		}
		return w.take();
	}

	SaveStatus decodeInventoryState(const uint8_t *data, size_t size, InventoryPersistState &out)
	{
		out = InventoryPersistState{};
		if (size != kInventoryStateBytes)
			return SaveStatus::Corrupt; // exact-size format
		ByteReader r(data, size);
		char magic[sizeof(kInventoryStateMagic)];
		r.raw(magic, sizeof(magic));
		const uint32_t version = r.u32();
		if (std::memcmp(magic, kInventoryStateMagic, sizeof(magic)) != 0)
			return SaveStatus::BadMagic;
		if (version != kInventoryStateFormatVersion)
			return SaveStatus::UnsupportedVersion;
		InventoryPersistState s;
		s.selectedSlot = r.u8();
		if (s.selectedSlot >= static_cast<uint8_t>(items::kHotbarSize))
			return SaveStatus::Corrupt;
		for (InventoryPersistState::Slot &slot : s.slots)
		{
			slot.itemId = r.u16();
			slot.count = r.u16();
			if (slot.count == 0)
			{
				if (slot.itemId != 0)
					return SaveStatus::Corrupt; // canonical empty is 0/0
				continue;
			}
			if (slot.itemId == 0)
				return SaveStatus::Corrupt; // None sentinel with a count
			const items::ItemId id = static_cast<items::ItemId>(slot.itemId);
			if (items::definition(id) == nullptr)
				return SaveStatus::Corrupt; // unknown registry ordinal
			if (slot.count > items::maxStackOf(id))
				return SaveStatus::Corrupt; // never silently clamped
		}
		if (r.failed())
			return SaveStatus::Corrupt;
		out = s;
		return SaveStatus::Ok;
	}

	std::vector<uint8_t> encodeItemsState(const ItemsPersistState &state)
	{
		ByteWriter w;
		w.raw(kItemsStateMagic, sizeof(kItemsStateMagic));
		w.u32(kItemsStateFormatVersion);
		w.u64(state.nextEntityId);
		w.u32(static_cast<uint32_t>(state.entities.size()));
		for (const ItemEntityPersistState &e : state.entities)
		{
			w.u64(e.id);
			w.u16(e.itemId);
			w.u16(e.count);
			w.f64(e.px);
			w.f64(e.py);
			w.f64(e.pz);
			w.f64(e.vx);
			w.f64(e.vy);
			w.f64(e.vz);
			w.u32(e.ageTicks);
			w.u32(e.pickupDelayTicks);
		}
		return w.take();
	}

	SaveStatus decodeItemsState(const uint8_t *data, size_t size, ItemsPersistState &out)
	{
		out = ItemsPersistState{};
		if (size < kItemsStateHeaderBytes)
			return SaveStatus::Corrupt;
		ByteReader r(data, size);
		char magic[sizeof(kItemsStateMagic)];
		r.raw(magic, sizeof(magic));
		const uint32_t version = r.u32();
		if (std::memcmp(magic, kItemsStateMagic, sizeof(magic)) != 0)
			return SaveStatus::BadMagic;
		if (version != kItemsStateFormatVersion)
			return SaveStatus::UnsupportedVersion;
		ItemsPersistState s;
		s.nextEntityId = r.u64();
		const uint32_t count = r.u32();
		if (s.nextEntityId < 1)
			return SaveStatus::Corrupt; // allocator starts at 1
		if (count > items::ItemEntitySettings::capacity)
			return SaveStatus::Corrupt; // bounded by the population cap
		if (size != kItemsStateHeaderBytes + static_cast<size_t>(count) * kItemEntityRecordBytes)
			return SaveStatus::Corrupt; // exact-size format
		uint64_t prevId = 0;
		for (uint32_t i = 0; i < count; ++i)
		{
			ItemEntityPersistState e;
			e.id = r.u64();
			e.itemId = r.u16();
			e.count = r.u16();
			e.px = r.f64();
			e.py = r.f64();
			e.pz = r.f64();
			e.vx = r.f64();
			e.vy = r.f64();
			e.vz = r.f64();
			e.ageTicks = r.u32();
			e.pickupDelayTicks = r.u32();
			// Strictly ascending unique ids, all below the allocator's next
			// id: no reuse across sessions, id-sorted population invariant.
			if (e.id <= prevId || e.id >= s.nextEntityId)
				return SaveStatus::Corrupt;
			prevId = e.id;
			if (e.itemId == 0 || e.count == 0)
				return SaveStatus::Corrupt; // entities always hold items
			const items::ItemId id = static_cast<items::ItemId>(e.itemId);
			if (items::definition(id) == nullptr || e.count > items::maxStackOf(id))
				return SaveStatus::Corrupt;
			if (!std::isfinite(e.px) || !std::isfinite(e.py) || !std::isfinite(e.pz) ||
			    !std::isfinite(e.vx) || !std::isfinite(e.vy) || !std::isfinite(e.vz))
				return SaveStatus::Corrupt; // never hand NaN to the solver
			s.entities.push_back(e);
		}
		if (r.failed())
			return SaveStatus::Corrupt;
		out = std::move(s);
		return SaveStatus::Ok;
	}
} // namespace worldsave
