#pragma once

// Persisted player UI preferences — issue #302, Epic #298 Phase D.
//
// The settings seam the epic asks for: the player UI scale override survives a
// restart without touching the developer UI, the simulation, saves or the wire
// protocol. The file is a tiny versioned text record next to the other client
// preferences (`<saves-root>/.client-profiles/player-ui.txt`), written
// atomically (unique staging file + rename) so a crash cannot leave a partial
// record, and read strictly: an unknown version, an unknown key, a duplicate
// key or an out-of-range value degrades to the defaults with `load` returning
// false so the host can log it loudly and carry on.
//
// The record is deliberately presentation-only: no gameplay, renderer or
// network preference may migrate here without its own review.

#include <Engine/PlayerUiScale.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace gameui
{
/// Upper bound for one preferences file; anything larger is corruption.
inline constexpr size_t kMaxPlayerUiSettingsBytes = 4096;
inline constexpr std::string_view kPlayerUiSettingsHeader = "ftvox-player-ui 1";

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

	/// Serialize to the on-disk record (header + one `scale` line).
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
