#include <World/WorldSave.hpp>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <system_error>

namespace worldsave
{
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

		// Exact record region size of a chunk file: 5 bytes per edit.
		constexpr size_t kChunkHeaderBytes = 4 + 4 + 4 + 4 + 4 + 8;
		constexpr size_t kChunkRecordBytes = 4 + 1;
	} // namespace

	// Little-endian byte helpers (ByteWriter/ByteReader) live in the header:
	// sibling formats such as player.state share them instead of duplicating
	// the byte-order discipline.

	// Writes `bytes` to finalPath.tmp, then renames over finalPath (atomic
	// replace; see the header's atomic-write guarantee).
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
		if (outMeta.formatVersion > kWorldSaveFormatVersion)
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

	SaveStatus writeChunkFile(const std::filesystem::path &chunksDir, int32_t chunkX, int32_t chunkZ,
	                          const std::vector<ChunkEdit> &edits)
	{
		if (edits.empty())
		{
			// All overrides reverted => the chunk has no payload: the file
			// must not exist (removal is a no-op Ok when it never did).
			return removeChunkFile(chunksDir, chunkX, chunkZ);
		}

		// Records are stored in ascending localIndex order; callers may pass
		// any order, so sort a local copy.
		std::vector<ChunkEdit> sorted = edits;
		std::sort(sorted.begin(), sorted.end(), [](const ChunkEdit &a, const ChunkEdit &b)
		          { return a.localIndex < b.localIndex; });

		ByteWriter records;
		for (const ChunkEdit &edit : sorted)
		{
			// Out-of-range indices are a caller bug: refuse to persist them
			// instead of writing a payload every future read would reject.
			if (edit.localIndex >= CHUNK_VOLUME)
				return SaveStatus::Corrupt;
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

		return writeFileAtomic(chunksDir / chunkFileName(chunkX, chunkZ), w.take()) ? SaveStatus::Ok
		                                                                            : SaveStatus::IoError;
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
		if (version > kWorldSaveFormatVersion)
			return SaveStatus::UnsupportedVersion;
		if (chunkX != expectedChunkX || chunkZ != expectedChunkZ)
			return SaveStatus::CoordinateMismatch;

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
			edits.push_back(ChunkEdit{localIndex, blockType});
		}

		// Stored order is arbitrary by contract; normalize to ascending.
		std::sort(edits.begin(), edits.end(), [](const ChunkEdit &a, const ChunkEdit &b)
		          { return a.localIndex < b.localIndex; });
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

	std::vector<std::filesystem::path> scanChunkFiles(const std::filesystem::path &chunksDir)
	{
		std::vector<std::filesystem::path> out;
		std::error_code ec;
		std::filesystem::directory_iterator it(chunksDir, ec);
		if (ec)
			return out; // missing/inaccessible directory: nothing to scan
		for (const std::filesystem::directory_entry &entry : it)
		{
			std::error_code entryEc;
			if (!entry.is_regular_file(entryEc) || entryEc)
				continue;
			int32_t chunkX = 0;
			int32_t chunkZ = 0;
			if (!parseChunkFileName(entry.path().filename().string(), chunkX, chunkZ))
				continue; // ignores non-chunk names and ".tmp" sidecars
			out.push_back(entry.path());
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	SaveStatus cleanTempFiles(const std::filesystem::path &chunksDir)
	{
		std::error_code ec;
		std::filesystem::directory_iterator it(chunksDir, ec);
		if (ec)
			return SaveStatus::Ok; // nothing to clean when the directory is absent
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
} // namespace worldsave
