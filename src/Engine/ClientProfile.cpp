#include <Engine/ClientProfile.hpp>
#include <Network/UdpSocket.hpp>
#include <World/WorldCheckpoint.hpp>
#include <World/WorldSave.hpp>

#include <stdexcept>

net::wire::PlayerKey client::playerKey(const std::filesystem::path &profile, ProfileAccess access)
{
	const auto path = profile / "player.key";
	const auto create = [&] {
		if (!profile.parent_path().empty())
			std::filesystem::create_directories(profile.parent_path());
		return std::filesystem::create_directory(profile);
	};
	const auto writeKey = [&] {
		const auto key = net::udp::randomPlayerKey();
		if (!worldsave::writeCheckpointFileAtomic(path, {key.begin(), key.end()}, true))
			throw std::runtime_error("could not save the multiplayer key in " + profile.string());
		return key;
	};
	if (access == ProfileAccess::Create)
	{
		if (!create())
			throw std::runtime_error("multiplayer profile already exists");
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
