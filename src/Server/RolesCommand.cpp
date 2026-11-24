#include <Server/RolesCommand.hpp>
#include <Server/WorldRoles.hpp>

#include <World/WorldCheckpoint.hpp>
#include <World/WorldPersistence.hpp>

#include <algorithm>
#include <charconv>
#include <iostream>
#include <string_view>

namespace server
{
namespace
{
namespace fs = std::filesystem;
namespace ws = worldsave;

int failure(int code, std::string_view message)
{
	std::cerr << "ROLES error=" << message << '\n';
	return code;
}

template<class T>
bool parseNumber(std::string_view text, T &out)
{
	const auto *end = text.data() + text.size();
	const auto parsed = std::from_chars(text.data(), end, out);
	return parsed.ec == std::errc{} && parsed.ptr == end;
}

std::optional<RoleOperation> parseOperation(std::string_view operation)
{
	if (operation == "provision") return RoleOperation::Provision;
	if (operation == "grant-admin") return RoleOperation::GrantAdmin;
	if (operation == "revoke-admin") return RoleOperation::RevokeAdmin;
	if (operation == "transfer") return RoleOperation::Transfer;
	if (operation == "recover") return RoleOperation::Recover;
	return std::nullopt;
}

std::optional<net::wire::PlayerKey> readProfile(const fs::path &path)
{
	std::error_code ec;
	if (fs::symlink_status(path, ec).type() != fs::file_type::regular || ec)
		return std::nullopt;
	std::vector<uint8_t> bytes;
	net::wire::PlayerKey key{};
	if (ws::readFileInto(path, bytes, key.size()) != ws::SaveStatus::Ok || bytes.size() != key.size())
		return std::nullopt;
	std::ranges::copy(bytes, key.begin());
	return key == net::wire::PlayerKey{} ? std::nullopt : std::optional{key};
}

bool readRequest(RoleRequest &request, int argc, char **argv)
{
	const auto requested = parseOperation(argv[2]);
	if (!requested || argc < 9 || !parseNumber(argv[5], request.worldId) || !request.worldId ||
		!parseNumber(argv[6], request.revision))
		return false;
	request.operation = *requested;
	if (*requested == RoleOperation::Provision || *requested == RoleOperation::Recover)
	{
		if (argc != 9)
			return false;
		request.profile = readProfile(argv[7]);
		request.newControl = localcontrol::readControlKey(argv[8]);
		return request.profile && request.newControl;
	}
	const bool transfer = *requested == RoleOperation::Transfer;
	if (argc != (transfer ? 10 : 9) || !parseNumber(argv[7], request.target.value) || !request.target.valid())
		return false;
	request.authorization = localcontrol::readControlKey(argv[8]);
	if (transfer)
		request.newControl = localcontrol::readControlKey(argv[9]);
	return request.authorization && (!transfer || request.newControl);
}
}

void printRolesUsage()
{
	std::cout <<
		"Offline roles (stop the server first):\n"
		"  ft_vox_server roles keygen NEW_CONTROL_FILE\n"
		"  ft_vox_server roles status SAVES_ROOT WORLD\n"
		"  ft_vox_server roles provision|recover SAVES_ROOT WORLD WORLD_ID REVISION PLAYER_KEY NEW_CONTROL_FILE\n"
		"  ft_vox_server roles grant-admin|revoke-admin SAVES_ROOT WORLD WORLD_ID REVISION PLAYER_ID OWNER_CONTROL_FILE\n"
		"  ft_vox_server roles transfer SAVES_ROOT WORLD WORLD_ID REVISION PLAYER_ID OWNER_CONTROL_FILE NEW_CONTROL_FILE\n";
}

int runRolesCommand(int argc, char **argv)
{
	if (argc < 3 || std::string_view(argv[2]) == "--help")
	{
		printRolesUsage();
		return argc < 3 ? 2 : 0;
	}
	const std::string_view operation = argv[2];
	if (operation == "keygen")
	{
		if (argc != 4)
			return failure(2, "keygen requires one new control file path");
		if (!localcontrol::createControlKey(argv[3]))
			return failure(4, "could not create private control file; choose a new writable path");
		std::cout << "ROLES status=credential_created\n";
		return 0;
	}
	const bool status = operation == "status";
	RoleRequest request;
	if ((status && argc != 5) || (!status && !readRequest(request, argc, argv)))
		return failure(2, "invalid role request or unreadable key file; see roles --help");
	const std::string_view world = argv[4];
	if (world == "." || !WorldPersistence::isValidWorldName(world))
		return failure(2, "invalid world name");
	const fs::path directory = fs::path(argv[3]) / world;
	std::error_code ec;
	if (fs::symlink_status(directory, ec).type() != fs::file_type::directory || ec)
		return failure(3, "world directory is unavailable");
	WorldIdentities identities;
	std::string error;
	if (!identities.lock(directory, error))
		return failure(3, error);
	ws::WorldMeta meta;
	if (ws::readWorldMeta(directory / "world.meta", meta) != ws::SaveStatus::Ok)
		return failure(3, "world metadata is missing or corrupt");
	ws::CheckpointStore checkpoints(directory);
	const auto recovered = checkpoints.recover(error);
	if (recovered != ws::CheckpointStore::RecoveryStatus::Recovered)
		return failure(3, "role commands require a valid dedicated checkpoint; first migrate legacy saves with the CLI server");
	if (!identities.load(meta.seed, meta.generatorVersion, checkpoints.publishedMultiplayerWorldId(), true, error))
		return failure(3, error);
	const auto checkpointWorldId = checkpoints.activeWorldId();
	if (checkpointWorldId && identities.worldId() != checkpointWorldId)
		return failure(3, error.empty() ? "checkpoint world identity mismatch" : error);
	if (checkpointWorldId)
	{
		ws::PlayerRoster roster;
		if (ws::readPlayerRoster(checkpoints.activeDir() / ws::kRosterFile, roster) != ws::SaveStatus::Ok ||
			roster.worldId != identities.worldId() ||
			!std::ranges::all_of(roster.players, [&](const auto &player) { return identities.contains(player.id); }))
			return failure(3, "checkpoint roster has a missing or mismatched identity");
	}
	else if (!identities.checkpointRequired())
		return failure(3, "role commands require a dedicated world; first migrate legacy saves with the CLI server");
	WorldRoles roles(directory, identities);
	if (!status && !roles.apply(request, error))
		return failure(4, error);
	roles.printStatus(std::cout);
	return 0;
}
}
