#include <Engine/PlayerUiPreferences.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <Engine/PlayerUiPreferencesWin32.hpp>

#include <iostream>
#include <process.h>
#include <syncstream>
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
	const long fraction = hundredths % 100;
	return std::to_string(hundredths / 100) + '.' + (fraction < 10 ? "0" : "") + std::to_string(fraction);
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

#if defined(_WIN32)
using namespace settings_io;

FileAttempt retryFileOperation(const char *name, auto operation)
{
	using Clock = std::chrono::steady_clock;
	static const bool trace = [] {
		wchar_t enabled[2]{};
		return GetEnvironmentVariableW(L"FT_VOX_PLAYER_SETTINGS_TRACE", enabled, 2) == 1 && enabled[0] == L'1';
	}();
	struct Failure
	{
		FileAttempt result;
		std::chrono::microseconds elapsed;
	};
	std::array<Failure, kAttempts> failures;
	const auto started = trace ? Clock::now() : Clock::time_point{};
	size_t failed = 0;
	const auto final = retryWindowsFile([&] {
		const auto result = operation();
		if (trace && result.error != ERROR_SUCCESS)
			failures[failed++] = {result, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started)};
		return result;
	}, [] { Sleep(1); });
	// Emit after the sequence so logging does not delay its retries. Keep all
	// attempts together and omit paths and preference contents.
	if (trace && failed != 0)
	{
		std::osyncstream log(std::clog);
		constexpr std::array phaseNames{"open", "read", "rename"};
		log << "[player-settings] " << name << " final_error=" << final.error
			<< " attempts=" << failed + (final.error == ERROR_SUCCESS ? 1 : 0) << '\n';
		for (size_t i = 0; i < failed; ++i)
			log << "  attempt=" << i + 1 << " phase=" << phaseNames[static_cast<size_t>(failures[i].result.phase)]
				<< " error=" << failures[i].result.error
				<< " elapsed_us=" << failures[i].elapsed.count() << '\n';
	}
	return final;
}
#endif

/// Atomically replace `to` with `from`. On POSIX rename(2) replaces atomically;
/// Windows needs MOVEFILE_REPLACE_EXISTING, and NEVER an explicit delete of the
/// previous record (that would leave a window with no preferences at all).
bool replaceFile(const std::filesystem::path &from, const std::filesystem::path &to)
{
#if defined(_WIN32)
	// Two game instances can replace the same record at the same time. Windows
	// reports a sharing/lock violation instead of arbitrating, so a bounded
	// retry loop turns that race into a normal last-writer-wins outcome.
	return retryFileOperation("replace", [&] {
		return FileAttempt{MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
			? ERROR_SUCCESS : GetLastError(), FilePhase::Rename};
	}).error == ERROR_SUCCESS;
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

std::string_view takeLine(std::string_view &record)
{
	const auto end = record.find('\n');
	const auto line = trimTrailingCarriageReturn(record.substr(0, end));
	record.remove_prefix(end == std::string_view::npos ? record.size() : end + 1);
	return line;
}

bool parseScale(std::string_view line, ScalePolicy &scale)
{
	if (line == "scale auto")
		return true;
	float value = 0.f;
	if (!line.starts_with("scale fixed ") || !parseStep(line.substr(12), value) || !isUserScale(value))
		return false;
	scale = {ScaleMode::Fixed, nearestUserScale(value)};
	return true;
}

bool parseInt(std::string_view text, int &value)
{
	if (text.empty())
		return false;
	const auto end = text.data() + text.size();
	const auto result = std::from_chars(text.data(), end, value);
	return result.ec == std::errc{} && result.ptr == end;
}

bool parseBool(std::string_view line, std::string_view key, bool &value)
{
	if (!line.starts_with(key))
		return false;
	const auto token = line.substr(key.size());
	value = token == "on";
	return value || token == "off";
}

const char *onOff(bool value)
{
	return value ? "on" : "off";
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
	return std::string(kPlayerUiSettingsHeader) + '\n' + formatScalePolicy(scale) +
		"\nwindow " + std::to_string(window.width) + ' ' + std::to_string(window.height) +
		"\nvsync " + onOff(vsync) + "\nfxaa " + onOff(fxaa) +
		"\nsensitivity " + std::to_string(sensitivity) + "\ninvert-y " + onOff(invertY) + '\n';
}

bool PlayerUiSettings::valid() const
{
	return (scale.mode == ScaleMode::Auto || (scale.mode == ScaleMode::Fixed && isUserScale(scale.value))) &&
		std::ranges::find(kWindowSizes, window) != kWindowSizes.end() &&
		std::ranges::find(kSensitivitySteps, sensitivity) != kSensitivitySteps.end();
}

bool PlayerUiSettings::parse(std::string_view record)
{
	if (record.empty() || record.size() > kMaxPlayerUiSettingsBytes)
		return false;
	PlayerUiSettings next;
	if (takeLine(record) != kPlayerUiSettingsHeader || !parseScale(takeLine(record), next.scale))
		return false;
	const auto windowLine = takeLine(record);
	const auto separator = windowLine.find(' ', 7);
	if (!windowLine.starts_with("window ") || separator == std::string_view::npos ||
		!parseInt(windowLine.substr(7, separator - 7), next.window.width) ||
		!parseInt(windowLine.substr(separator + 1), next.window.height) ||
		!parseBool(takeLine(record), "vsync ", next.vsync) ||
		!parseBool(takeLine(record), "fxaa ", next.fxaa))
		return false;
	const auto sensitivityLine = takeLine(record);
	if (!sensitivityLine.starts_with("sensitivity ") ||
		!parseInt(sensitivityLine.substr(12), next.sensitivity) ||
		!parseBool(takeLine(record), "invert-y ", next.invertY))
		return false;
	if (!record.empty() || !next.valid())
		return false;
	*this = next;
	return true;
}

bool parseLegacyPlayerScale(std::string_view record, ScalePolicy &scale)
{
	ScalePolicy next;
	if (record.size() > kMaxPlayerUiSettingsBytes || takeLine(record) != kLegacyPlayerUiSettingsHeader ||
		!parseScale(takeLine(record), next) || !record.empty())
		return false;
	scale = next;
	return true;
}

namespace
{
LoadStatus readRecord(const std::filesystem::path &path, auto decode)
{
	if (path.empty())
		return LoadStatus::Missing;
#if defined(_WIN32)
	// A reader stays on one complete record while MoveFileEx replaces the
	// path. The CRT stream does not share DELETE access with the replacer.
	char record[kMaxPlayerUiSettingsBytes + 1];
	DWORD bytesRead = 0;
	const auto result = retryFileOperation("load", [&] {
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return FileAttempt{GetLastError(), FilePhase::Open};
		const DWORD readError = ReadFile(file, record, static_cast<DWORD>(sizeof(record)), &bytesRead, nullptr)
			? ERROR_SUCCESS : GetLastError();
		CloseHandle(file);
		return FileAttempt{readError, FilePhase::Read};
	});
	if (result.error == ERROR_SUCCESS)
		return decode(std::string_view(record, bytesRead)) ? LoadStatus::Ok : LoadStatus::Corrupt;
	return loadFailure(result);
#else
	// Use the open error from this operation. A second path lookup could see
	// a different record after a concurrent rename.
	auto *file = std::fopen(path.c_str(), "rb");
	if (!file)
		return errno == ENOENT ? LoadStatus::Missing : LoadStatus::Unavailable;
	char record[kMaxPlayerUiSettingsBytes + 1];
	const auto bytesRead = std::fread(record, 1, sizeof(record), file);
	const bool readFailed = std::ferror(file) != 0;
	const bool closeFailed = std::fclose(file) != 0;
	if (readFailed || closeFailed)
		return LoadStatus::Unavailable;
	return decode(std::string_view(record, bytesRead)) ? LoadStatus::Ok : LoadStatus::Corrupt;
#endif
}
} // namespace

LoadStatus PlayerUiSettings::load(const std::filesystem::path &path)
{
	return readRecord(path, [this](std::string_view record) { return parse(record); });
}

LoadStatus loadLegacyPlayerScale(const std::filesystem::path &path, ScalePolicy &scale)
{
	return readRecord(path, [&scale](std::string_view record) { return parseLegacyPlayerScale(record, scale); });
}

bool PlayerUiSettings::save(const std::filesystem::path &path) const
{
	std::error_code ec;
	if (path.empty() || !valid())
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
