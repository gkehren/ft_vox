#include <Engine/PlayerUiPreferences.hpp>

#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>

#include <process.h>
#else
#include <unistd.h>
#endif

namespace gameui
{
namespace
{
/// Parse a scale step without touching the C locale (issue #302 F06): the
/// decimal separator is always '.', no sign, no exponent, at most two decimals.
bool parseStep(std::string_view text, float &value)
{
	if (text.empty() || text.size() > 8)
		return false;
	long whole = 0;
	int fraction = 0, digits = 0;
	bool dot = false, any = false;
	for (const char c : text)
	{
		if (c == '.')
		{
			if (dot)
				return false;
			dot = true;
			continue;
		}
		if (c < '0' || c > '9')
			return false;
		if (!dot)
		{
			whole = whole * 10 + (c - '0');
			if (whole > 100)
				return false;
		}
		else
		{
			if (digits == 2)
				return false;
			fraction = fraction * 10 + (c - '0');
			++digits;
		}
		any = true;
	}
	if (!any)
		return false;
	if (dot)
		while (digits++ < 2)
			fraction *= 10;
	const double parsed = double(whole) + double(fraction) / 100.0;
	if (!(parsed > 0.0) || parsed > double(kMaxScale))
		return false;
	value = static_cast<float>(parsed);
	return true;
}

/// Format `value` as a fixed two-decimal ASCII string. Integer formatting is
/// locale-independent, unlike snprintf("%.2f").
std::string formatStep(float value)
{
	const long hundredths = std::lround(value * 100.f);
	char buffer[16]{};
	std::snprintf(buffer, sizeof(buffer), "%ld.%02ld", hundredths / 100, hundredths % 100);
	return buffer;
}

unsigned long processId()
{
#if defined(_WIN32)
	return static_cast<unsigned long>(_getpid());
#else
	return static_cast<unsigned long>(getpid());
#endif
}

/// A staging name no other writer can pick: process id + monotonic clock +
/// per-process counter (issue #302 F06). Nothing else is ever deleted.
std::filesystem::path stagingPath(const std::filesystem::path &path)
{
	static std::atomic<uint64_t> counter{0};
	const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	char suffix[96]{};
	std::snprintf(suffix, sizeof(suffix), ".tmp.%lu.%llu.%llu", processId(),
		static_cast<unsigned long long>(stamp),
		static_cast<unsigned long long>(counter.fetch_add(1)));
	std::filesystem::path staging = path;
	staging += suffix;
	return staging;
}

/// Atomically replace `to` with `from`. On POSIX rename(2) replaces atomically;
/// Windows needs MOVEFILE_REPLACE_EXISTING, and NEVER an explicit delete of the
/// previous record (that would leave a window with no preferences at all).
bool replaceFile(const std::filesystem::path &from, const std::filesystem::path &to)
{
#if defined(_WIN32)
	// Two game instances can replace the same record at the same time. Windows
	// reports a sharing/lock violation instead of arbitrating, so a bounded
	// retry loop turns that race into a normal last-writer-wins outcome.
	for (int attempt = 0; attempt < 40; ++attempt)
	{
		if (MoveFileExW(from.c_str(), to.c_str(),
				MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
			return true;
		const auto error = GetLastError();
		if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
			error != ERROR_LOCK_VIOLATION)
			return false;
		Sleep(1);
	}
	return false;
#else
	std::error_code ec;
	std::filesystem::rename(from, to, ec);
	return !ec;
#endif
}

std::string_view trimTrailingCarriageReturn(std::string_view line)
{
	if (!line.empty() && line.back() == '\r')
		line.remove_suffix(1);
	return line;
}
} // namespace

std::string formatScalePolicy(ScalePolicy policy)
{
	if (policy.mode == ScaleMode::Auto)
		return "scale auto";
	return "scale fixed " + formatStep(nearestUserScale(policy.value));
}

std::string PlayerUiSettings::serialize() const
{
	return std::string(kPlayerUiSettingsHeader) + '\n' + formatScalePolicy(scale) + '\n';
}

bool PlayerUiSettings::parse(std::string_view record)
{
	if (record.empty() || record.size() > kMaxPlayerUiSettingsBytes)
		return false;
	const auto end = record.find('\n');
	if (end == std::string_view::npos)
		return false;
	if (trimTrailingCarriageReturn(record.substr(0, end)) != kPlayerUiSettingsHeader)
		return false;
	std::string_view rest = record.substr(end + 1);
	PlayerUiSettings next;
	bool sawScale = false;
	while (!rest.empty())
	{
		const auto lineEnd = rest.find('\n');
		const std::string_view line = trimTrailingCarriageReturn(rest.substr(0, lineEnd));
		rest.remove_prefix(lineEnd == std::string_view::npos ? rest.size() : lineEnd + 1);
		if (line.empty())
			return false;
		if (line == "scale auto" && !sawScale)
		{
			next.scale = {ScaleMode::Auto, 1.f};
			sawScale = true;
			continue;
		}
		if (line.starts_with("scale fixed ") && !sawScale)
		{
			float value = 0.f;
			if (!parseStep(line.substr(12), value) || !isUserScale(value))
				return false;
			next.scale = {ScaleMode::Fixed, nearestUserScale(value)};
			sawScale = true;
			continue;
		}
		return false; // unknown key, duplicate key or malformed value
	}
	if (!sawScale)
		return false;
	*this = next;
	return true;
}

LoadStatus PlayerUiSettings::load(const std::filesystem::path &path)
{
	if (path.empty())
		return LoadStatus::Missing;
#if defined(_WIN32)
	// A reader stays on one complete record while MoveFileEx replaces the
	// path. The CRT stream does not share DELETE access with the replacer.
	for (int attempt = 0; attempt < 40; ++attempt)
	{
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE)
		{
			char record[kMaxPlayerUiSettingsBytes + 1];
			DWORD bytesRead = 0;
			const bool read = ReadFile(file, record, static_cast<DWORD>(sizeof(record)), &bytesRead, nullptr) != 0;
			CloseHandle(file);
			return read && parse(std::string_view(record, bytesRead)) ? LoadStatus::Ok : LoadStatus::Corrupt;
		}
		const auto error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
			return LoadStatus::Missing;
		if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
			error != ERROR_LOCK_VIOLATION && error != ERROR_DELETE_PENDING)
			break;
		Sleep(1);
	}
	return LoadStatus::Corrupt;
#else
	// Open first, then read bounded chunks from that same handle (issue #302
	// R03). Asking for the size separately would describe one record while the
	// handle pointed at another once a concurrent writer replaces the file.
	std::ifstream in;
	for (int attempt = 0; attempt < 5 && !in.is_open(); ++attempt)
	{
		in.open(path, std::ios::binary);
		if (!in.is_open())
		{
			// A concurrent writer holds the record for a moment while it renames;
			// that transient sharing violation is not corruption.
			in.clear();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	if (!in.is_open())
	{
		// A record that simply is not there (including a not-yet-created parent
		// directory) is a fresh install, not corruption.
		std::error_code ec;
		const bool present = std::filesystem::exists(path, ec);
		return (!present && !ec) ? LoadStatus::Missing : LoadStatus::Corrupt;
	}
	std::string record;
	char chunk[512];
	while (record.size() <= kMaxPlayerUiSettingsBytes)
	{
		in.read(chunk, static_cast<std::streamsize>(sizeof(chunk)));
		const auto got = static_cast<size_t>(in.gcount());
		record.append(chunk, got);
		if (got < sizeof(chunk))
			break;
	}
	if (in.bad() || record.size() > kMaxPlayerUiSettingsBytes)
		return LoadStatus::Corrupt;
	return parse(record) ? LoadStatus::Ok : LoadStatus::Corrupt;
#endif
}

bool PlayerUiSettings::save(const std::filesystem::path &path) const
{
	std::error_code ec;
	if (path.empty())
		return false;
	const auto parent = path.parent_path();
	if (!parent.empty())
	{
		std::filesystem::create_directories(parent, ec);
		if (ec)
			return false;
	}
	// A sibling directory is never a preference record: refuse instead of
	// replacing (or deleting) it.
	if (std::filesystem::is_directory(path, ec))
		return false;
	const auto staging = stagingPath(path);
	{
		std::ofstream out(staging, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		const std::string record = serialize();
		out.write(record.data(), static_cast<std::streamsize>(record.size()));
		out.close();
		if (!out)
		{
			// Only our own staging file is removed; the previous record stays.
			std::error_code ignored;
			std::filesystem::remove(staging, ignored);
			return false;
		}
	}
	if (!replaceFile(staging, path))
	{
		std::error_code ignored;
		std::filesystem::remove(staging, ignored);
		return false;
	}
	return true;
}
} // namespace gameui
