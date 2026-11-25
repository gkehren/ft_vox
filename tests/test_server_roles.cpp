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
using localcontrol::Operation;
using AdminResult = localcontrol::Result;
constexpr wire::PlayerKey kKicked{44}, kOutside{45};

udp::Address endpoint(const Snapshot &snapshot)
{
	return udp::parseAddress(snapshot.address, snapshot.port);
}

Snapshot awaitAdmin(const hosting::ServerSupervisor &supervisor)
{
	auto snapshot = await(supervisor, [](const Snapshot &s) { return s.administration || !s.active(); });
	require(snapshot.state == State::Ready && snapshot.administration, snapshot.message);
	return snapshot;
}

Snapshot command(hosting::ServerSupervisor &supervisor, Operation operation, sim::PlayerId target = {},
	std::optional<hosting::OwnerProfile> delegate = {})
{
	const auto before = supervisor.snapshot();
	const auto priorSequence = before.administration->sequence;
	require(supervisor.administer(before.launch, {operation, target, std::move(delegate)}), "queue local admin command");
	return await(supervisor, [&](const Snapshot &s) {
		return !s.active() || (!s.adminPending && s.adminOperation == operation && s.adminResult &&
			(s.adminChannelFailed || (s.administration && s.administration->sequence > priorSequence)));
	});
}

const localcontrol::Player &player(const localcontrol::Reply &state, sim::PlayerId id)
{
	const auto found = std::ranges::find_if(state.players, [&](const auto &row) { return row.id == id; });
	require(found != state.players.end(), "expected registered actor in admin state");
	return *found;
}

void disconnected(const hosting::ServerSupervisor &supervisor, udp::ClientTransport &client,
	std::initializer_list<udp::ClientTransport *> otherClients = {})
{
	using Status = udp::ClientStatus;
	const auto id = client.welcome()->player;
	const auto done = [&] {
		return client.status() == Status::Disconnected || client.status() == Status::TimedOut ||
			client.status() == Status::Rejected;
	};
	const auto pollClients = [&] {
		const auto now = udp::ClientTransport::Clock::now();
		client.poll(now);
		for (auto *other : otherClients)
			other->poll(now);
	};
	await(supervisor, [&](const Snapshot &state) {
		pollClients();
		return state.status && std::ranges::none_of(state.status->transport.roster(), [&](const auto &row) {
			return row.id == id;
		});
	});
	const auto deadline = udp::ClientTransport::Clock::now() + 12s;
	while (!done() && udp::ClientTransport::Clock::now() < deadline)
	{
		pollClients();
		std::this_thread::sleep_for(5ms);
	}
	require(done(), "moderated client #" + std::to_string(id.value) + " did not observe departure");
}

void rejected(udp::Address address, const wire::PlayerKey &key)
{
	udp::ClientTransport client(address, key);
	pump(client, [](const auto &session) { return session.status() == udp::ClientStatus::Rejected; });
	CHECK(!client.welcome(), "denied profile never receives Welcome or activates");
}

// Direct inherited IPC fixture bypasses the UI adapter's permission and
// one-request guards. It uses the same private pipe launcher as production.
class ControlChild
{
public:
	ControlChild(const fs::path &exe, const fs::path &root, const std::string &world,
		const localcontrol::Bootstrap &proof)
		: process(fs::absolute(exe), {"--world", world, "--saves-root", root, "--port", "0", "--workers", "0",
			"--interest-radius", "1", "--launcher-control", "admin"}, true)
	{
		require(process.writeControl(localcontrol::encodeBootstrap(proof)), "write the bootstrap proof");
	}

	~ControlChild()
	{
		if (!finished)
		{
			process.closeControl();
			process.forceStop();
			const auto deadline = std::chrono::steady_clock::now() + 2s;
			while (!process.pollExit() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(5ms);
		}
	}

	localcontrol::Reply readReply()
	{
		const auto deadline = std::chrono::steady_clock::now() + 10s;
		std::array<uint8_t, localcontrol::kReplyHeaderSize> header{};
		while (std::chrono::steady_clock::now() < deadline)
		{
			readExact(header, deadline);
			const auto size = localcontrol::replySize(header);
			std::vector<uint8_t> frame(size.value_or(monitoring::kFrameSize));
			std::ranges::copy(header, frame.begin());
			readExact(std::span{frame}.subspan(header.size()), deadline);
			if (size)
			{
				auto reply = localcontrol::decodeReply(frame);
				require(reply.has_value(), "invalid administration reply");
				return std::move(*reply);
			}
		}
		throw std::runtime_error("no administration reply");
	}

	localcontrol::Reply send(const localcontrol::Request &request)
	{
		require(process.writeControl(localcontrol::encodeRequest(request)), "send administration frame");
		return readReply();
	}

	udp::Address address() const
	{
		return udp::parseAddress("127.0.0.1", static_cast<uint16_t>(field({0, output}, " port=")));
	}

	void stop()
	{
		process.closeControl();
		const auto deadline = std::chrono::steady_clock::now() + 15s;
		while (std::chrono::steady_clock::now() < deadline)
		{
			drainLog();
			if (const auto exit = process.pollExit())
			{
				finished = true;
				drainLog();
				require(*exit == 0 && output.find("reason=launcher saved=1") != std::string::npos,
					"EOF still commits a checkpoint after control errors");
				return;
			}
			std::this_thread::sleep_for(5ms);
		}
		throw std::runtime_error("control child did not exit");
	}

	hosting::ChildProcess process;
	std::string output;

private:
	void drainLog()
	{
		std::array<char, 4096> log{};
		while (const auto count = process.readOutput(log))
			output.append(log.data(), count);
	}

	void readExact(std::span<uint8_t> bytes, std::chrono::steady_clock::time_point deadline)
	{
		while (!bytes.empty() && std::chrono::steady_clock::now() < deadline)
		{
			drainLog();
			const auto count = process.readMonitoring({reinterpret_cast<char *>(bytes.data()), bytes.size()});
			if (count)
				bytes = bytes.subspan(count);
			else if (process.pollExit())
				throw std::runtime_error("control child exited: " + output);
			else
				std::this_thread::sleep_for(5ms);
		}
		require(bytes.empty(), "timed out reading the bounded reply pipe");
	}

	bool finished{};
};

void checkAdministration(const fs::path &exe, const fs::path &root)
{
	using localcontrol::Role;
	auto settings = options("Live administration");
	const hosting::OwnerProfile owner{kHost, root / "live owner key"};
	const hosting::OwnerProfile delegate{kGuest, root / "live delegate key"};
	const auto authorityFile = authority(root, settings.world);
	hosting::ServerSupervisor supervisor;
	require(supervisor.start(settings, exe, root, owner), "launch administration world");
	auto first = awaitAdmin(supervisor);
	require(first.administration->role == Role::Owner && first.administration->players.size() == 1,
		"owner is authenticated before the first join");
	stop(supervisor);

	// Rewrite the original FTRL layout to exercise a real v1 -> v2 migration.
	auto legacy = readPrivateFixture(authorityFile);
	require(legacy.size() >= 120 && legacy[4] == 2, "v2 authority fixture");
	legacy[4] = 1;
	legacy.erase(legacy.begin() + 100, legacy.begin() + 112);
	legacy.resize(legacy.size() - 8);
	ws::ByteWriter checksum;
	checksum.u64(ws::fnv1a64(legacy.data(), legacy.size()));
	const auto suffix = checksum.take();
	legacy.insert(legacy.end(), suffix.begin(), suffix.end());
	require(localcontrol::writePrivateFile(authorityFile, legacy, true), "install original authority format");
	require(supervisor.start(settings, exe, root, owner), "restart with v1 authority");
	first = awaitAdmin(supervisor);
	CHECK(first.administration->role == Role::Owner && !first.administration->allowlist, "v1 migrates with open admission");
	const auto ownerId = first.administration->players.front().id;
	CHECK(!supervisor.administer(first.launch - 1, {Operation::Stop, {}, {}}), "a stale frame cannot address another launch");

	udp::ClientTransport guest(endpoint(first), kGuest), banned(endpoint(first), kReplacement);
	udp::ClientTransport kicked(endpoint(first), kKicked), outside(endpoint(first), kOutside);
	const auto guestId = connect(guest).player, bannedId = connect(banned).player;
	const auto kickedId = connect(kicked).player;
	connect(outside);
	await(supervisor, [](const Snapshot &s) {
		return s.status && s.status->registeredProfiles == 5 && s.status->transport.count == 4 &&
			std::ranges::all_of(s.status->transport.roster(), [](const auto &row) {
				return row.state == monitoring::SessionState::Connected;
			});
	});
	auto state = command(supervisor, Operation::List);
	CHECK(state.adminResult == AdminResult::Applied && state.administration->players.size() == 5,
		"owner lists sorted registered roles without gameplay secrets");

	auto granted = command(supervisor, Operation::GrantAdmin, guestId);
	CHECK(granted.adminResult == AdminResult::Applied && player(*granted.administration, guestId).role == Role::Admin &&
		!player(*granted.administration, guestId).localAdmin, "role-only delegation confers no local credential");
	auto wrong = command(supervisor, Operation::GrantAdmin, guestId, hosting::OwnerProfile{kOutside, root / "other key"});
	CHECK(wrong.adminResult == AdminResult::InvalidCredential && wrong.administration->revision == granted.administration->revision,
		"the selected local profile must match the target actor");
	auto bound = command(supervisor, Operation::GrantAdmin, guestId, delegate);
	CHECK(bound.adminResult == AdminResult::Applied && player(*bound.administration, guestId).localAdmin,
		"owner explicitly authorizes the matching private local admin credential");

	auto ban = command(supervisor, Operation::Ban, bannedId);
	CHECK(ban.adminResult == AdminResult::Applied && player(*ban.administration, bannedId).banned, "ban is applied before acknowledgment");
	disconnected(supervisor, banned, {&guest, &kicked, &outside});
	rejected(endpoint(first), kReplacement);
	CHECK(guest.status() == udp::ClientStatus::Connected && outside.status() == udp::ClientStatus::Connected,
		"moderation leaves unrelated client sessions intact");
	auto kick = command(supervisor, Operation::Kick, kickedId);
	CHECK(kick.adminResult == AdminResult::Applied && player(*kick.administration, kickedId).kickedUntil > localcontrol::unixSeconds(),
		"kick installs a bounded rejoin hold");
	disconnected(supervisor, kicked, {&guest, &outside});
	rejected(endpoint(first), kKicked);

	CHECK(command(supervisor, Operation::Allow, guestId).adminResult == AdminResult::Applied, "owner adds delegate to allowlist");
	auto limited = command(supervisor, Operation::EnableAllowlist);
	CHECK(limited.adminResult == AdminResult::Applied && limited.administration->allowlist &&
		limited.administration->audit.back().actor == ownerId, "owner enables allowlist and the audit records the actor");
	disconnected(supervisor, outside);
	rejected(endpoint(first), wire::PlayerKey{46});
	CHECK(command(supervisor, Operation::Ban, ownerId).adminResult == AdminResult::ProtectedRole, "owner is exempt from moderation");
	auto stopped = command(supervisor, Operation::Stop);
	if (stopped.active())
		stopped = await(supervisor, [](const Snapshot &s) { return !s.active(); });
	CHECK(stopped.state == State::Stopped && stopped.checkpoint > first.checkpoint &&
		std::ranges::any_of(stopped.logs, [](const auto &line) { return line.find("reason=control saved=1") != std::string::npos; }),
		"privileged stop waits for the committed checkpoint");

	const auto delegateKey = localcontrol::readControlKey(delegate.controlFile);
	const auto ownerKey = localcontrol::readControlKey(owner.controlFile);
	require(delegateKey && ownerKey, "read separate local fixtures");
	std::vector<uint8_t> beforeLatestBan;
	{
		ControlChild child(exe, root, settings.world, {delegate.player, *delegateKey});
		const auto initial = child.readReply();
		require(initial.role == Role::Admin && initial.allowlist && player(initial, bannedId).banned, "delegate authenticates after restart");
		CHECK(initial.epoch != first.administration->epoch && player(initial, kickedId).kickedUntil > localcontrol::unixSeconds(),
			"new process epoch and durable kick cooldown");
		rejected(child.address(), kKicked);
		const auto request = [&](uint64_t sequence, Operation op, sim::PlayerId id = {}) {
			return localcontrol::Request{initial.world, initial.epoch, sequence, op, id, std::nullopt};
		};
		auto foreign = request(1, Operation::Unban, bannedId);
		foreign.world ^= 1;
		CHECK(child.send(foreign).result == AdminResult::WrongWorld, "different-world command is rejected");
		foreign.world = initial.world;
		foreign.epoch = first.administration->epoch;
		CHECK(child.send(foreign).result == AdminResult::WrongWorld, "previous-process command is rejected");
		CHECK(child.send(request(2, Operation::Unban, bannedId)).result == AdminResult::Replay, "skipped sequence is rejected");
		auto malformed = localcontrol::encodeRequest(request(1, Operation::Unban, bannedId));
		malformed[4] = 99;
		require(child.process.writeControl(malformed), "send malformed request version");
		CHECK(child.readReply().result == AdminResult::Malformed, "invalid version is rejected before dispatch");
		CHECK(child.send(request(1, Operation::GrantAdmin, guestId)).result == AdminResult::Forbidden, "delegate cannot self-promote");
		CHECK(child.send(request(2, Operation::Ban, ownerId)).result == AdminResult::ProtectedRole, "delegate cannot alter owner access");
		CHECK(child.send(request(3, Operation::RevokeAdmin, ownerId)).result == AdminResult::Forbidden, "delegate cannot revoke owner");
		CHECK(child.send(request(4, Operation::Stop)).result == AdminResult::Forbidden, "typed shutdown is owner-only");
		CHECK(child.send(request(5, Operation::Transfer, guestId)).result == AdminResult::Malformed, "ownership changes have no live opcode");
		CHECK(child.send(request(5, Operation::DisableAllowlist)).result == AdminResult::Forbidden, "allowlist policy is owner-only");

		auto unban = child.send(request(6, Operation::Unban, bannedId));
		CHECK(unban.result == AdminResult::Applied && unban.revision == initial.revision + 1 &&
			unban.audit.back().actor == guestId && unban.audit.back().target == bannedId,
			"only the approved moderation changes durable state and the audit names actor and target");
		const auto replay = child.send(request(6, Operation::Ban, bannedId));
		CHECK(replay.result == AdminResult::Replay && replay.revision == unban.revision && !player(replay, bannedId).banned,
			"replay does not reapply or change policy");
		beforeLatestBan = readPrivateFixture(authorityFile);
		const auto reban = child.send(request(7, Operation::Ban, bannedId));
		CHECK(reban.result == AdminResult::Applied && player(reban, bannedId).banned, "delegated ban is persisted");
		const auto excess = localcontrol::encodeRequest(request(8, Operation::Unban, bannedId));
		std::array<uint8_t, 2 * localcontrol::kRequestSize> burst{};
		std::ranges::copy(excess, burst.begin());
		std::ranges::copy(excess, burst.begin() + excess.size());
		require(child.process.writeControl(burst), "send oversized request batch");
		const auto over = child.readReply();
		CHECK(over.result == AdminResult::Malformed && over.revision == reban.revision && player(over, bannedId).banned,
			"over-capacity burst applies neither command");
		child.stop();
	}

	const auto pending = authorityFile.parent_path() / (".pending.." + settings.world);
	require(localcontrol::writePrivateFile(pending, readPrivateFixture(authorityFile), false) &&
		localcontrol::writePrivateFile(authorityFile, beforeLatestBan, true), "simulate interruption after advancing role revision");
	require(fs::is_regular_file(ws::checkpointManifestPrevPath(root / settings.world)), "gameplay recovery fixture has a fallback");
	std::ofstream(ws::checkpointManifestPath(root / settings.world), std::ios::binary | std::ios::trunc).put('X');
	require(supervisor.start(settings, exe, root, owner), "restart from interrupted authority and damaged gameplay checkpoint");
	auto recovered = awaitAdmin(supervisor);
	CHECK(!fs::exists(pending) && recovered.administration->allowlist && player(*recovered.administration, bannedId).banned &&
		recovered.administration->audit.back().operation == Operation::Ban, "authority recovery preserves ban over gameplay rollback");
	rejected(endpoint(recovered), kReplacement);

	udp::ClientTransport activeDelegate(endpoint(recovered), kGuest);
	CHECK(connect(activeDelegate).player == guestId, "delegate keeps its persistent gameplay profile");
	await(supervisor, [](const Snapshot &s) {
		return s.status && s.status->transport.count == 1 &&
			s.status->transport.roster().front().state == monitoring::SessionState::Connected;
	});
	const auto revoked = command(supervisor, Operation::RevokeAdmin, guestId);
	CHECK(revoked.adminResult == AdminResult::Applied && player(*revoked.administration, guestId).role == Role::Player &&
		!player(*revoked.administration, guestId).localAdmin, "revocation clears the role and proof");
	disconnected(supervisor, activeDelegate);
	udp::ClientTransport ordinary(endpoint(recovered), kGuest);
	CHECK(connect(ordinary).player == guestId, "after revocation an allowed profile can reconnect as a player");
	await(supervisor, [&](const Snapshot &s) {
		ordinary.poll(udp::ClientTransport::Clock::now());
		return s.status && s.status->transport.count == 1 &&
			s.status->transport.roster().front().state == monitoring::SessionState::Connected;
	});
	CHECK(command(supervisor, Operation::Unallow, guestId).adminResult == AdminResult::Applied, "owner removes player from enabled allowlist");
	disconnected(supervisor, ordinary);
	rejected(endpoint(recovered), kGuest);
	stop(supervisor);

	for (const auto &proof : {localcontrol::Bootstrap{delegate.player, *delegateKey}, localcontrol::Bootstrap{kOutside, *ownerKey}})
	{
		ControlChild child(exe, root, settings.world, proof);
		const auto initial = child.readReply();
		CHECK(initial.role == Role::Player && initial.players.empty() && initial.audit.empty(),
			"revoked or mismatched credential receives no administrative data");
		CHECK(child.send({initial.world, initial.epoch, 1, Operation::Unban, bannedId, {}}).result == AdminResult::Forbidden,
			"revoked or mismatched credential cannot execute a valid scoped command");
		child.stop();
	}

	const auto status = run(exe, {"roles", "status", root, settings.world});
	CHECK(status.has("operation=ban actor_id=" + std::to_string(guestId.value) + " target_id=" + std::to_string(bannedId.value)) &&
		status.has("operation=revoke-admin actor_id=" + std::to_string(ownerId.value)), "offline audit shows stable identities");

	const auto replacementProfile = root / "recover-profile";
	const auto replacementControl = root / "recover-control";
	writeProfile(replacementProfile, kOutside);
	require(localcontrol::createControlKey(replacementControl), "create a distinct recovery credential");
	const auto result = run(exe, {"roles", "recover", root, settings.world,
		std::to_string(field(status, "world_id=")), std::to_string(field(status, "revision=")),
		replacementProfile, replacementControl});
	require(result.code == 0, "recover owner from valid authority with existing moderation: " + result.output);
	require(supervisor.start(settings, exe, root, owner), "reopen with the retired owner's saved profile");
	const auto former = awaitAdmin(supervisor);
	CHECK(former.administration->role == Role::Player && former.administration->audit.empty() &&
		!supervisor.administer(former.launch, {Operation::Stop, {}, {}}),
		"the old profile and control key cannot administer the recovered world");
	stop(supervisor);

	require(supervisor.start(settings, exe, root, hosting::OwnerProfile{kOutside, replacementControl}),
		"relaunch with the recovered owner's local profile and credential");
	const auto replacement = awaitAdmin(supervisor);
	CHECK(replacement.administration->role == Role::Owner && replacement.administration->allowlist &&
		player(*replacement.administration, bannedId).banned &&
		replacement.administration->audit.back().operation == Operation::Recover,
		"the recovered owner has live authority with existing bans and audit still in force");
	udp::ClientTransport replacementClient(endpoint(replacement), kOutside);
	const auto replacementId = connect(replacementClient).player;
	CHECK(player(*replacement.administration, replacementId).role == Role::Owner &&
		player(*replacement.administration, guestId).role == Role::Player &&
		player(*replacement.administration, ownerId).role == Role::Player,
		"only the recovered owner bypasses admission and former role holders stay ordinary players");
	auto final = command(supervisor, Operation::Stop);
	if (final.active())
		final = await(supervisor, [](const Snapshot &s) { return !s.active(); });
	CHECK(final.state == State::Stopped && final.checkpoint > replacement.checkpoint,
		"recovered owner can complete a live privileged stop and checkpoint");
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
	const bool administration = argc == 3 && std::string_view(argv[2]) == "--administration";
	if (argc != 2 && !administration)
		return 2;
	const auto root = fs::temp_directory_path() / ("ft vox roles " + std::to_string(udp::randomId()));
	try
	{
		fs::create_directory(root);
		if (administration)
			checkAdministration(argv[1], root);
		else
		{
			checkMalformed(argv[1], root);
			checkBootstrapAndRoles(argv[1], root);
			checkLegacyAndCrash(argv[1], argv[0], root);
		}
	}
	catch (const std::exception &error)
	{
		CHECK(false, error.what());
	}
	std::error_code ignored;
	fs::remove_all(root, ignored);
	return testmain::exitCode();
}
