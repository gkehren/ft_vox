#pragma once

#include <Engine/PlayerUiScale.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace gameui
{
/// Upper bound for one preferences file; anything larger is corruption.
inline constexpr size_t kMaxPlayerUiSettingsBytes = 4096;
inline constexpr std::string_view kLegacyPlayerUiSettingsHeader = "ftvox-player-ui 1";
inline constexpr std::string_view kPlayerUiSettingsHeader = "ftvox-player-settings 1";
inline constexpr std::string_view kPlayerSettingsPath = ".client-settings/player-settings.txt";
inline constexpr std::string_view kLegacyPlayerSettingsPath = ".client-profiles/player-ui.txt";

struct WindowSize
{
	int width{}, height{};
	bool operator==(const WindowSize &) const = default;
};

inline constexpr std::array kWindowSizes{
	WindowSize{960, 540}, WindowSize{1280, 720}, WindowSize{1600, 900}, WindowSize{1920, 1080}};
inline constexpr WindowSize kDefaultWindowSize = kWindowSizes.back();

inline constexpr std::array kSensitivitySteps{25, 50, 75, 100, 125, 150, 200, 250, 300};
inline constexpr int kDefaultSensitivity = 100;
inline constexpr float kDefaultCameraSensitivity = 0.1f;

inline float cameraSensitivity(int percent)
{
	return kDefaultCameraSensitivity * static_cast<float>(percent) / 100.f;
}

/// Why a load did or did not change the settings.
enum class LoadStatus : uint8_t
{
	Ok = 0,
	Missing,  // fresh install: defaults, no log
	Corrupt,  // unreadable record: defaults + a loud log, never a session change
};

struct PlayerUiSettings
{
	ScalePolicy scale{};
	WindowSize window{kDefaultWindowSize};
	bool vsync{true};
	bool fxaa{true};
	int sensitivity{kDefaultSensitivity};
	bool invertY{};

	bool valid() const;
	std::string serialize() const;

	/// Parse a whole record. Returns false (and leaves `*this` untouched) when
	/// the record is not exactly one valid version of our format.
	bool parse(std::string_view record);

	/// Read the record. A missing file is NOT an error (fresh install) and
	/// keeps the current values; a corrupt file also keeps them, so a bad
	/// record never silently changes the running session.
	LoadStatus load(const std::filesystem::path &path);

	/// Write the record atomically. Creates the parent directory.
	bool save(const std::filesystem::path &path) const;

	bool operator==(const PlayerUiSettings &) const = default;
};

/// `scale auto` / `scale fixed <step>` line, exposed for tests and logs.
std::string formatScalePolicy(ScalePolicy policy);
} // namespace gameui
