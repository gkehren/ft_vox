#include <Engine/ClientProfile.hpp>
#include <Engine/ExclusiveFile.hpp>
#include <Network/UdpSocket.hpp>
#include <World/WorldCheckpoint.hpp>
#include <World/WorldSave.hpp>

#include <stdexcept>

namespace
{
namespace fs = std::filesystem;

bool removeOwnedEntry(const fs::path &path, fs::file_type expected)
{
	std::error_code ec;
	const auto type = fs::symlink_status(path, ec).type();
	if (type == fs::file_type::not_found)
		return true;
	if (ec || type != expected)
		return false;
	fs::remove(path, ec); // For a directory, this succeeds only when it is empty.
	return !ec;
}
}

net::wire::PlayerKey client::playerKey(const fs::path &profile, ProfileAccess access, ProfileKeyWriter writer)
{
	const auto path = profile / "player.key";
	auto temporary = path;
	temporary += ".tmp"; // writeCheckpointFileAtomic's staging file
	const auto create = [&] {
		if (!profile.parent_path().empty())
			std::filesystem::create_directories(profile.parent_path());
		return std::filesystem::create_directory(profile);
	};
	const auto writeKey = [&] {
		bool ownsTemporary = false;
		try
		{
			const auto key = net::udp::randomPlayerKey();
			ownsTemporary = reserveFile(temporary) == FileReservation::Created;
			if (ownsTemporary && (writer ? writer(path, key) :
				worldsave::writeCheckpointFileAtomic(path, {key.begin(), key.end()}, true)))
				return key;
		}
		catch (...) {}
		if ((!ownsTemporary || removeOwnedEntry(temporary, fs::file_type::regular)) &&
			removeOwnedEntry(profile, fs::file_type::directory))
			throw ProfileCreationError("Could not write the profile key. Check that the profile folder is writable, then retry the same name.");
		throw ProfileCreationError("Could not write the profile key or safely remove the new profile folder. Inspect it before retrying or choose a new name.");
	};
	if (access == ProfileAccess::Create)
	{
		if (!create())
			throw ProfileCreationError("That profile already exists. Select it or choose a new name.");
		return writeKey();
	}
	if (access == ProfileAccess::Load &&
		(std::filesystem::symlink_status(profile).type() != std::filesystem::file_type::directory ||
		 std::filesystem::symlink_status(path).type() != std::filesystem::file_type::regular))
		throw std::runtime_error("expected a local profile directory and key file");

	net::wire::PlayerKey key{};
	std::vector<uint8_t> bytes;
	auto status = worldsave::readFileInto(path, bytes, key.size());
	if (status == worldsave::SaveStatus::NotFound && access == ProfileAccess::LoadOrCreate)
	{
		// An existing directory with a missing key may still own gameplay
		// state. Only the caller that created the directory may issue a key.
		if (create())
			return writeKey();
		status = worldsave::readFileInto(path, bytes, key.size());
	}
	if (status == worldsave::SaveStatus::Ok && bytes.size() == key.size())
		std::ranges::copy(bytes, key.begin());
	if (key == net::wire::PlayerKey{})
		throw std::runtime_error("missing, corrupt or unreadable multiplayer key in " + profile.string());
	return key;
}
