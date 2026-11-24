#include <Hosting/ChildProcess.hpp>
#include <Hosting/ServerSupervisor.hpp>
#include <Network/ClientTransport.hpp>
#include <Server/ServerHost.hpp>
#include <World/WorldCheckpoint.hpp>

#include "TestMain.hpp"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#endif

namespace server
{
struct ServerHostProbe
{
	static bool startWithCheckpointHook(ServerHost &host, std::string &error, void (*hook)())
	{
		return host.start(error, hook);
	}
	static bool startWithRoleHook(ServerHost &host, std::string &error, void (*hook)())
	{
		return host.start(error, nullptr, hook);
	}
};
}

namespace
{
namespace fs = std::filesystem;
namespace udp = net::udp;
namespace wire = net::wire;
namespace ws = worldsave;
using namespace std::chrono_literals;
using hosting::Snapshot;
using hosting::State;
constexpr wire::PlayerKey kHost{41}, kGuest{42}, kReplacement{43};

void require(bool condition, const std::string &error)
{
	if (!condition)
		throw std::runtime_error(error);
}

struct Result
{
	int code;
	std::string output;
	bool has(std::string_view text) const { return output.find(text) != std::string::npos; }
};

Result run(const fs::path &exe, const std::vector<fs::path> &args,
	std::span<const uint8_t> input = {}, bool keepOpen = false)
{
	hosting::ChildProcess process(fs::absolute(exe), args);
	if (!input.empty())
		require(process.writeControl(input), "write child bootstrap fixture");
	if (!keepOpen)
		process.closeControl();
	const auto deadline = std::chrono::steady_clock::now() + 12s;
	std::array<char, 4096> buffer{};
	std::string output;
	const auto drain = [&] {
		while (const auto count = process.readOutput(buffer))
			output.append(buffer.data(), count);
	};
	while (std::chrono::steady_clock::now() < deadline)
	{
		drain();
		if (auto exit = process.pollExit())
		{
			drain();
			return {*exit, std::move(output)};
		}
		std::this_thread::sleep_for(10ms);
	}
	process.closeControl();
	process.forceStop(); // Owned test fixture only.
	throw std::runtime_error("timed out waiting for role command");
}

uint64_t field(const Result &result, std::string_view label)
{
	const auto at = result.output.find(label);
	require(at != std::string::npos, "missing status field: " + std::string(label));
	const auto *begin = result.output.data() + at + label.size();
	uint64_t value = 0;
	const auto parsed = std::from_chars(begin, result.output.data() + result.output.size(), value);
	require(parsed.ec == std::errc{}, "invalid status field");
	return value;
}

Snapshot await(const hosting::ServerSupervisor &supervisor, const auto &done)
{
	const auto deadline = std::chrono::steady_clock::now() + 15s;
	while (std::chrono::steady_clock::now() < deadline)
	{
		auto snapshot = supervisor.snapshot();
		if (done(snapshot))
			return snapshot;
		std::this_thread::sleep_for(10ms);
	}
	throw std::runtime_error("role integration server did not reach its expected state");
}

Snapshot ready(const hosting::ServerSupervisor &supervisor)
{
	auto state = await(supervisor, [](const Snapshot &s) { return s.state == State::Ready || !s.active(); });
	require(state.state == State::Ready, state.message);
	return state;
}

void stop(hosting::ServerSupervisor &supervisor)
{
	supervisor.stop();
	auto state = await(supervisor, [](const Snapshot &s) { return !s.active(); });
	require(state.state == State::Stopped, state.message);
}

void pump(udp::ClientTransport &client, const auto &done)
{
	const auto deadline = std::chrono::steady_clock::now() + 6s;
	client.connect(udp::ClientTransport::Clock::now());
	while (std::chrono::steady_clock::now() < deadline && !done(client))
	{
		client.poll(udp::ClientTransport::Clock::now());
		std::this_thread::sleep_for(5ms);
	}
	require(done(client), "UDP fixture did not reach its expected state");
}

wire::Welcome connect(udp::ClientTransport &client)
{
	pump(client, [](const auto &transport) { return transport.status() == udp::ClientStatus::Connected; });
	require(client.welcome().has_value(), "client has no Welcome");
	return *client.welcome();
}

hosting::Options options(std::string world)
{
	hosting::Options settings;
	settings.world = std::move(world);
	settings.port = 0;
	settings.workers = 0;
	settings.interestRadius = 1;
	return settings;
}

void writeProfile(const fs::path &file, const wire::PlayerKey &key)
{
	require(localcontrol::writePrivateFile(file, key, false), "write profile fixture");
}

fs::path authority(const fs::path &root, std::string_view world)
{
	return root / ".ft_vox..server-roles" / world;
}

std::vector<uint8_t> readPrivateFixture(const fs::path &path)
{
	std::vector<uint8_t> bytes;
	require(localcontrol::readPrivateFile(path, bytes, 200'000), "read private fixture");
	return bytes;
}

#ifdef _WIN32
void allowEveryoneToRead(const fs::path &path)
{
	auto name = path.native();
	alignas(SID) std::array<BYTE, SECURITY_MAX_SID_SIZE> sid{};
	DWORD size = static_cast<DWORD>(sid.size());
	require(CreateWellKnownSid(WinWorldSid, nullptr, sid.data(), &size) != FALSE, "build Everyone SID");
	PACL current = nullptr;
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	require(GetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
		nullptr, nullptr, &current, nullptr, &descriptor) == ERROR_SUCCESS, "read fixture DACL");
	EXPLICIT_ACCESS_W grant{};
	grant.grfAccessPermissions = FILE_GENERIC_READ;
	grant.grfAccessMode = GRANT_ACCESS;
	BuildTrusteeWithSidW(&grant.Trustee, sid.data());
	PACL expanded = nullptr;
	const auto built = SetEntriesInAclW(1, &grant, current, &expanded);
	const auto changed = built == ERROR_SUCCESS ? SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT,
		DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, expanded, nullptr) : built;
	LocalFree(expanded);
	LocalFree(descriptor);
	require(changed == ERROR_SUCCESS, "widen protected fixture DACL");
}
#endif

void checkBootstrapAndRoles(const fs::path &exe, const fs::path &root)
{
	const auto ownerFile = root / "owner secret", guestFile = root / "next owner secret";
	const auto recoverFile = root / "recovery secret", recoveryProfile = root / "replacement player key";
	const auto wireKeyFile = root / "host player key", copiedCredential = root / "wire key in control format";
	for (const auto &path : {ownerFile, guestFile, recoverFile})
		require(run(exe, {"roles", "keygen", path}).code == 0, "create offline role-control key");
	CHECK(run(exe, {"roles", "keygen", ownerFile}).code != 0, "keygen never replaces a credential");
	writeProfile(wireKeyFile, kHost);
	writeProfile(recoveryProfile, kReplacement);
	std::array<uint8_t, 40> forged{'F', 'T', 'V', 'K', 1, 0, 0, 0};
	std::ranges::copy(kHost, forged.begin() + 8);
	require(localcontrol::writePrivateFile(copiedCredential, forged, false), "encode copied wire key");
	const auto ownerControl = localcontrol::readControlKey(ownerFile), guestControl = localcontrol::readControlKey(guestFile);
	require(ownerControl && guestControl, "read generated credential");
#ifndef _WIN32
	const auto publicBits = fs::perms::group_all | fs::perms::others_all;
	CHECK((fs::status(ownerFile).permissions() & publicBits) == fs::perms::none,
		"new control files are private");
#endif

	const auto settings = options("Hosted roles");
	const auto status = [&] { return run(exe, {"roles", "status", root, settings.world}); };
	hosting::ServerSupervisor supervisor;
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, ownerFile}), "start owner child");
	const auto first = ready(supervisor);
	CHECK(status().has("could not lock server world"), "offline commands cannot race the live server");
	udp::ClientTransport guest(udp::parseAddress(first.address, first.port), kGuest);
	const auto guestWelcome = connect(guest);
	// A copied bearer key can still log in as that gameplay actor in wire v5.
	// It must never become an authorization credential for role changes.
	udp::ClientTransport copiedLogin(udp::parseAddress(first.address, first.port), kHost);
	const auto hostWelcome = connect(copiedLogin);
	CHECK(hostWelcome.player.value == ws::kFirstRemotePlayerId && guestWelcome.player.value > hostWelcome.player.value,
		"owner ID was enrolled before the guest that won the connection race");
	udp::ClientTransport duplicate(udp::parseAddress(first.address, first.port), kHost);
	pump(duplicate, [](const auto &transport) { return transport.status() == udp::ClientStatus::Rejected; });
	CHECK(duplicate.status() == udp::ClientStatus::Rejected, "colliding profile is refused while the actor is active");
	stop(supervisor);

	const auto worldId = std::to_string(hostWelcome.worldId);
	const auto ownerId = std::to_string(hostWelcome.player.value);
	const auto guestId = std::to_string(guestWelcome.player.value);
	const auto subject = "owner_id=" + ownerId;
	auto state = status();
	CHECK(state.code == 0 && state.has(subject) && state.has("operation=bootstrap"),
		"offline audit confirms launcher-selected owner");
	const auto revision = [&] { return std::to_string(field(status(), "revision=")); };
	auto rev = revision();
	const auto mutation = [&](std::string_view command, const std::string &scope, const std::string &version,
		const fs::path &target, const fs::path &credential, const fs::path &next = {}) {
		std::vector<fs::path> args{"roles", command, root, settings.world, scope, version, target, credential};
		if (!next.empty())
			args.push_back(next);
		return run(exe, args);
	};
	const auto ownerBytes = readPrivateFixture(ownerFile);
	fs::remove(ownerFile);
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, ownerFile}), "reopen with missing owner credential");
	auto refused = await(supervisor, [](const Snapshot &s) { return !s.active(); });
	CHECK(refused.state == State::Failed && refused.port == 0 && refused.message.find("offline roles recover") != std::string::npos &&
		!fs::exists(ownerFile), "missing owner credential fails before UDP with recovery instructions and no replacement");
	require(localcontrol::createControlKey(ownerFile), "create distinct wrong credential fixture");
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, ownerFile}), "reopen with mismatched owner credential");
	refused = await(supervisor, [](const Snapshot &s) { return !s.active(); });
	CHECK(refused.state == State::Failed && refused.port == 0 && refused.message.find("offline roles recover") != std::string::npos,
		"replacement credential cannot start as the registered owner");
	require(localcontrol::writePrivateFile(ownerFile, ownerBytes, true), "restore matching owner credential");
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, ownerFile}), "reopen with restored owner credential");
	ready(supervisor);
	stop(supervisor);

#ifdef _WIN32
	allowEveryoneToRead(ownerFile);
	CHECK(!localcontrol::readControlKey(ownerFile) && mutation("grant-admin", worldId, rev, guestId, ownerFile).code == 2,
		"protected control key with an Everyone read grant is rejected by offline owner authentication");
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, ownerFile}), "reopen with a widened control DACL");
	CHECK(await(supervisor, [](const Snapshot &s) { return !s.active(); }).message.find("offline roles recover") != std::string::npos,
		"an insecure Windows control key cannot start as the registered owner");
	require(localcontrol::writePrivateFile(ownerFile, ownerBytes, true), "restore private owner DACL");
	const auto authorityFile = authority(root, settings.world);
	const auto authorityBytes = readPrivateFixture(authorityFile);
	allowEveryoneToRead(authorityFile);
	CHECK(status().has("status=invalid"), "protected authority with an Everyone read grant is rejected");
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, ownerFile}), "reopen with a widened authority DACL");
	CHECK(await(supervisor, [](const Snapshot &s) { return !s.active(); }).message.find("role authority") != std::string::npos,
		"insecure Windows authority fails before UDP");
	require(localcontrol::writePrivateFile(authorityFile, authorityBytes, true), "restore private authority DACL");
#endif
	CHECK(mutation("grant-admin", worldId, rev, guestId, wireKeyFile).code == 2,
		"wire key file is not a control credential");
	CHECK(mutation("grant-admin", worldId, rev, guestId, copiedCredential).has("valid local owner credential"),
		"copied wire key bytes cannot authorize roles even inside a valid control file");
	CHECK(mutation("grant-admin", "0", rev, guestId, ownerFile).code == 2,
		"malformed scope is rejected");
	CHECK(mutation("grant-admin", std::to_string(hostWelcome.worldId ^ 1), rev, guestId, ownerFile).has("different world"),
		"cross-world request is rejected");
	CHECK(mutation("grant-admin", worldId, rev, guestId, ownerFile).has("admin_id=" + guestId),
		"owner can delegate a registered player");
	{
		server::HostConfig config;
		config.world = {.worldName = settings.world, .savesRoot = root};
		config.workers = 0;
		server::ServerHost host(config);
		std::string error;
		const bool started = host.start(error);
		require(started, error);
		CHECK(host.role(hostWelcome.player) == server::Role::Owner && host.role(guestWelcome.player) == server::Role::Admin &&
			host.role({}) == server::Role::Player, "the dedicated server loads the owner and admin roles before joining");
		const bool stopped = host.stop(error);
		require(stopped, error);
	}
	CHECK(mutation("grant-admin", worldId, rev, guestId, ownerFile).has("stale role revision"),
		"replaying the same request is rejected");
	CHECK(mutation("transfer", worldId, revision(), ownerId, guestFile, recoverFile).has("valid local owner credential"),
		"admin cannot transfer ownership");
	CHECK(mutation("revoke-admin", worldId, revision(), ownerId, ownerFile).code != 0,
		"the primary owner cannot be revoked as an admin");

	const auto saved = root / "world backup";
	fs::copy(root / settings.world, saved, fs::copy_options::recursive);
	const auto olderAuthority = root / "authority before revocation";
	const auto newestAuthority = root / "authority after revocation";
	const auto activeAuthority = authority(root, settings.world);
	require(localcontrol::writePrivateFile(olderAuthority, readPrivateFixture(activeAuthority), false), "save old authority");
	CHECK(mutation("revoke-admin", worldId, revision(), guestId, ownerFile).has("operation=revoke-admin"),
		"revocation is audited");
	require(localcontrol::writePrivateFile(newestAuthority, readPrivateFixture(activeAuthority), false), "save current authority");
	require(localcontrol::writePrivateFile(activeAuthority, readPrivateFixture(olderAuthority), true), "restore old authority");
	CHECK(status().has("status=stale"), "restoring an older authority alone is recognized by the registry revision");
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, ownerFile}), "try opening rolled-back role authority");
	CHECK(await(supervisor, [](const Snapshot &s) { return !s.active(); }).state == State::Failed,
		"stale authority cannot start UDP or reactivate a revoked admin");
	const auto pendingAuthority = activeAuthority.parent_path() / (".pending.." + settings.world);
	require(localcontrol::writePrivateFile(pendingAuthority, readPrivateFixture(newestAuthority), false), "stage current authority");
	const auto completed = status();
	CHECK(completed.has("status=owned") && completed.has("operation=revoke-admin") &&
		!completed.has("admin_id=") && !fs::exists(pendingAuthority),
		"startup recovery promotes the staged authority attested by the registry");
	fs::rename(root / settings.world, root / "newer gameplay files");
	fs::copy(saved, root / settings.world, fs::copy_options::recursive);

	udp::Socket previousPort(udp::parseAddress(first.address, first.port));
	const auto noGuestCredential = root / "no guest credential";
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kGuest, noGuestCredential}), "restart with different launcher");
	const auto second = ready(supervisor);
	CHECK(first.port != second.port && !fs::exists(noGuestCredential), "different launcher can host without creating an owner credential");
	udp::ClientTransport ownerAgain(udp::parseAddress(second.address, second.port), kHost);
	const auto retained = connect(ownerAgain);
	udp::ClientTransport guestAgain(udp::parseAddress(second.address, second.port), kGuest);
	CHECK(retained.worldId == hostWelcome.worldId && retained.player == hostWelcome.player &&
		connect(guestAgain).player == guestWelcome.player, "reversed connection order retains persistent identities");
	stop(supervisor);
	state = status();
	CHECK(state.has(subject) && !state.has("admin_id=") && state.has("operation=revoke-admin"),
		"reopening via another launcher and restoring gameplay cannot reactivate a revoked role");

	CHECK(mutation("transfer", worldId, revision(), guestId, ownerFile, guestFile).has("owner_id=" + guestId),
		"authenticated owner can atomically transfer ownership and rotate the credential");
	CHECK(mutation("grant-admin", worldId, revision(), ownerId, ownerFile).has("valid local owner credential"),
		"prior owner's credential is retired immediately");
	CHECK(mutation("grant-admin", worldId, revision(), ownerId, guestFile).has("admin_id=" + ownerId),
		"the new owner can delegate");

	// Missing marked authority fails closed. Recovery runs under the native
	// lock, binds a new profile and clears every prior delegation.
	fs::rename(authority(root, settings.world), root / "offline authority backup");
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, ownerFile}), "try to open missing authority");
	CHECK(await(supervisor, [](const Snapshot &s) { return !s.active(); }).state == State::Failed,
		"missing marked authority fails closed without a new owner");
	CHECK(status().has("status=missing"), "offline status identifies missing role authority");
	const auto missingRevision = revision();
	const auto recovered = mutation("recover", worldId, missingRevision, recoveryProfile, recoverFile);
	CHECK(recovered.code == 0 && recovered.has("operation=recover") && !recovered.has("admin_id="),
		"local recovery installs new owner and clears delegation");
	CHECK(mutation("recover", worldId, missingRevision, recoveryProfile, recoverFile).has("stale role revision"),
		"recovery cannot be replayed");
	CHECK(mutation("grant-admin", worldId, revision(), guestId, guestFile).has("valid local owner credential"),
		"recovery retires the previous owner's credential");

	std::fstream roleState(authority(root, settings.world), std::ios::in | std::ios::out | std::ios::binary);
	roleState.put('X');
	roleState.close();
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, ownerFile}), "try to open corrupt authority");
	CHECK(await(supervisor, [](const Snapshot &s) { return !s.active(); }).state == State::Failed &&
		status().has("status=invalid"), "corrupt authority is never interpreted as unowned");
}

void checkLegacyAndCrash(const fs::path &exe, const fs::path &self, const fs::path &root)
{
	const auto settings = options("Legacy roles");
	const auto controlFile = root / "legacy credential", profile = root / "legacy profile";
	require(localcontrol::createControlKey(controlFile), "create legacy credential");
	writeProfile(profile, kHost);
	const auto control = localcontrol::readControlKey(controlFile);
	require(control.has_value(), "load legacy credential");
	const auto world = root / settings.world;
	fs::create_directories(world);
	require(ws::writeWorldMeta(world / "world.meta", {ws::kWorldSaveFormatVersion, 17, TerrainGenerator::kGeneratorVersion})
		== ws::SaveStatus::Ok, "write legacy world metadata");
	std::ofstream(world / "player.state") << "damaged migration payload";
	hosting::ServerSupervisor supervisor;
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, controlFile}), "start legacy migration fixture");
	CHECK(await(supervisor, [](const Snapshot &s) { return !s.active(); }).state == State::Failed &&
		!fs::exists(authority(root, settings.world)), "failed legacy migration creates no owner");
	fs::remove(world / "player.state");
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, controlFile}), "try reopening repaired legacy world in GUI");
	CHECK(await(supervisor, [](const Snapshot &s) { return !s.active(); }).message.find("offline roles provision") != std::string::npos,
		"GUI requires explicit provisioning for repaired legacy worlds");
	require(run(exe, {"--world", settings.world, "--saves-root", root, "--workers", "0", "--ticks", "1"}).code == 0,
		"CLI establishes the dedicated checkpoint for a repaired legacy world");
	const auto status = run(exe, {"roles", "status", root, settings.world});
	CHECK(status.has("status=unowned"), "CLI migration keeps the world unowned until provisioning");
	const auto worldId = std::to_string(field(status, "world_id="));
	CHECK(run(exe, {"roles", "provision", root, settings.world, worldId, "0", profile, controlFile}).has("operation=provision"),
		"operator can explicitly provision an unowned migrated or CLI world");
	CHECK(run(exe, {"roles", "provision", root, settings.world, worldId, "0", profile, controlFile}).code != 0,
		"provision cannot replace an existing owner");
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kHost, controlFile}), "reopen provisioned world in GUI");
	ready(supervisor);
	stop(supervisor);
	const auto registry = world / ws::kIdentityFile;
	std::ofstream(registry, std::ios::binary | std::ios::trunc) << "bad registry";
	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kGuest, controlFile}), "start with corrupt registry");
	CHECK(await(supervisor, [](const Snapshot &s) { return !s.active(); }).state == State::Failed &&
		run(exe, {"roles", "status", root, settings.world}).has("identity registry"), "corrupt registry fails closed");

	CHECK(run(self, {"--crash-before-authority", root, controlFile}).code == 84,
		"interrupted after world metadata and registry but before recording bootstrap");
	const auto early = options("Early roles");
	CHECK(fs::exists(root / early.world / "world.meta") && fs::exists(root / early.world / ws::kIdentityFile) &&
		!fs::exists(authority(root, early.world)), "early crash leaves a partial world without authority");
	for (const auto &profileKey : {kHost, kGuest})
	{
		require(supervisor.start(early, exe, root, hosting::OwnerProfile{profileKey, controlFile}), "retry early startup from GUI");
		const auto refused = await(supervisor, [](const Snapshot &s) { return !s.active(); });
		CHECK(refused.state == State::Failed && refused.port == 0 &&
			refused.message.find("offline roles provision") != std::string::npos &&
			!fs::exists(authority(root, early.world)), "early retry requires provisioning and never grants or serves an unowned world");
	}
	require(run(exe, {"--world", early.world, "--saves-root", root, "--workers", "0", "--ticks", "1"}).code == 0,
		"CLI can finish the checkpoint after early interruption");
	const auto earlyStatus = run(exe, {"roles", "status", root, early.world});
	CHECK(run(exe, {"roles", "provision", root, early.world, std::to_string(field(earlyStatus, "world_id=")),
		"0", profile, controlFile}).has("operation=provision"), "early world can be explicitly provisioned");
	require(supervisor.start(early, exe, root, hosting::OwnerProfile{kHost, controlFile}), "reopen early world after provisioning");
	ready(supervisor);
	stop(supervisor);

	CHECK(run(self, {"--crash-pending", root, controlFile}).code == 83, "interrupted before the first checkpoint publication");
	const auto pending = options("Pending roles");
	CHECK(fs::exists(authority(root, pending.world)) &&
		!fs::exists(ws::checkpointManifestPath(root / pending.world)), "bootstrap intent outlives the interrupted first checkpoint");
	require(supervisor.start(pending, exe, root, hosting::OwnerProfile{kGuest, controlFile}), "retry pending owner with a different launcher");
	const auto state = ready(supervisor);
	udp::ClientTransport original(udp::parseAddress(state.address, state.port), kHost);
	udp::ClientTransport other(udp::parseAddress(state.address, state.port), kGuest);
	CHECK(connect(other).player.value > ws::kFirstRemotePlayerId &&
		connect(original).player.value == ws::kFirstRemotePlayerId, "retry completes only the original persisted bootstrap");
	stop(supervisor);

	const auto cli = options("CLI roles");
	CHECK(run(exe, {"--world", cli.world, "--saves-root", root, "--workers", "0", "--ticks", "1"}).code == 0,
		"standalone CLI still creates a world");
	CHECK(run(exe, {"roles", "status", root, cli.world}).has("status=unowned"), "standalone world starts unowned");
	require(supervisor.start(cli, exe, root, hosting::OwnerProfile{kHost, controlFile}), "host existing CLI world");
	CHECK(await(supervisor, [](const Snapshot &s) { return !s.active(); }).message.find("offline roles provision") != std::string::npos &&
		run(exe, {"roles", "status", root, cli.world}).has("status=unowned"), "a GUI start cannot claim or serve an unowned CLI world");
	require(localcontrol::writePrivateFile(authority(root, cli.world), readPrivateFixture(authority(root, pending.world)), false),
		"copy foreign authority fixture");
	require(supervisor.start(cli, exe, root, hosting::OwnerProfile{kHost, controlFile}), "try copying another world's authority");
	CHECK(await(supervisor, [](const Snapshot &s) { return !s.active(); }).state == State::Failed &&
		run(exe, {"roles", "status", root, cli.world}).has("status=wrong_world"), "another world's authority cannot be transplanted");
}

void checkMalformed(const fs::path &exe, const fs::path &root)
{
	const std::vector<fs::path> args{"--world", "Bad bootstrap", "--saves-root", root,
		"--port", "0", "--launcher-control", "bootstrap"};
	auto frame = localcontrol::encodeBootstrap({kHost, localcontrol::ControlKey{51}});
	auto malformed = frame;
	malformed[4] = 0;
	CHECK(run(exe, args, malformed, true).has("invalid local owner bootstrap"), "unknown bootstrap version is rejected");
	CHECK(run(exe, args, std::span{frame}.first(20)).has("invalid local owner bootstrap"), "truncated bootstrap is rejected");
	std::vector<uint8_t> repeated(frame.begin(), frame.end());
	repeated.insert(repeated.end(), frame.begin(), frame.end());
	CHECK(run(exe, args, repeated, true).has("invalid local owner bootstrap"), "multiple bootstrap records are rejected");
	CHECK(!fs::exists(root / "Bad bootstrap"), "malformed control input never opens a world or binds UDP");
}
}

int main(int argc, char **argv)
{
	const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
	if (argc == 4 && (mode == "--crash-pending" || mode == "--crash-before-authority"))
	{
		const auto control = localcontrol::readControlKey(argv[3]);
		if (!control)
			return 1;
		server::HostConfig config;
		const bool early = mode == "--crash-before-authority";
		config.world = {.seed = 17, .worldName = early ? "Early roles" : "Pending roles", .savesRoot = argv[2]};
		config.workers = 0;
		config.ownerBootstrap = localcontrol::Bootstrap{kHost, *control};
		server::ServerHost host(config);
		std::string error;
		const bool started = early ?
			server::ServerHostProbe::startWithRoleHook(host, error, [] { std::_Exit(84); }) :
			server::ServerHostProbe::startWithCheckpointHook(host, error, [] { std::_Exit(83); });
		if (!started)
			std::cerr << error << '\n';
		return 2;
	}
	if (argc != 2)
		return 2;
	const auto root = fs::temp_directory_path() / ("ft vox roles " + std::to_string(udp::randomId()));
	try
	{
		fs::create_directory(root);
		checkMalformed(argv[1], root);
		checkBootstrapAndRoles(argv[1], root);
		checkLegacyAndCrash(argv[1], argv[0], root);
	}
	catch (const std::exception &error)
	{
		CHECK(false, error.what());
	}
	std::error_code ignored;
	fs::remove_all(root, ignored);
	return testmain::exitCode();
}
