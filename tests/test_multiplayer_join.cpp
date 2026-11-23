#include <Engine/ClientProfile.hpp>
#include <Engine/MultiplayerJoin.hpp>

#include "TestMain.hpp"

#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <utility>

namespace
{
namespace fs = std::filesystem;
using client::ProfileAccess;

struct TempDirectory
{
	const fs::path path = fs::temp_directory_path() / ("ft-vox-join-" + std::to_string(net::udp::randomId()));
	~TempDirectory()
	{
		std::error_code ignored;
		fs::remove_all(path, ignored);
	}
};

bool rejectsProfile(const fs::path &path, ProfileAccess access)
{
	try { (void)client::playerKey(path, access); }
	catch (const std::exception &) { return true; }
	return false;
}

std::string creationFailure(const fs::path &path, ProfileAccess access, client::ProfileKeyWriter writer)
{
	try { (void)client::playerKey(path, access, writer); }
	catch (const client::ProfileCreationError &error) { return error.what(); }
	return {};
}

void checkProfileRollback(const fs::path &root)
{
	const auto profile = root / "retry";
	const auto failAfterWrite = [](const fs::path &path, const net::wire::PlayerKey &) {
		std::ofstream(path.string() + ".tmp", std::ios::app) << "partial key";
		return false;
	};
	const auto throwDuringWrite = [](const fs::path &, const net::wire::PlayerKey &) -> bool {
		throw std::runtime_error("injected write error");
	};
	CHECK(creationFailure(profile, ProfileAccess::Create, failAfterWrite).find("retry the same name") !=
		std::string::npos && !fs::exists(profile), "failed initial write removes its temporary file and empty folder");
	CHECK(creationFailure(profile, ProfileAccess::LoadOrCreate, throwDuringWrite).find("retry the same name") !=
		std::string::npos && !fs::exists(profile), "throwing writer also rolls back a new CLI profile");
	const auto key = client::playerKey(profile, ProfileAccess::Create);
	CHECK(client::playerKey(profile, ProfileAccess::Load) == key, "the same profile name succeeds after rollback");
	CHECK(!creationFailure(profile, ProfileAccess::Create, failAfterWrite).empty() &&
		client::playerKey(profile, ProfileAccess::Load) == key, "a failed creation attempt leaves an existing key intact");

	const auto occupied = root / "occupied";
	const auto failWithUnrelatedFile = [](const fs::path &path, const net::wire::PlayerKey &) {
		std::ofstream(path.parent_path() / "keep.txt") << "unrelated";
		return false;
	};
	CHECK(creationFailure(occupied, ProfileAccess::Create, failWithUnrelatedFile).find("Inspect it") !=
		std::string::npos, "rollback blocked by unknown contents reports how to recover");
	CHECK(fs::is_regular_file(occupied / "keep.txt") && !fs::exists(occupied / "player.key.tmp") &&
		rejectsProfile(occupied, ProfileAccess::LoadOrCreate) && !fs::exists(occupied / "player.key"),
		"only the owned temporary file is removed and the existing directory stays protected");
}

void checkProfileDiscovery(const fs::path &root)
{
	const auto first = root / "discovery-a";
	const auto second = root / "discovery-b";
	CHECK(client::listProfiles(first).empty(), "missing profile directory is empty");
	fs::create_directories(first);
	fs::create_directories(second);
	CHECK(client::listProfiles(second).empty(), "empty profile directory is empty");

	std::vector<std::string> names;
	for (int i = 100; i < 149; ++i)
		names.push_back("profile-" + std::to_string(i));
	const std::vector<std::string> expected(names.begin(), names.begin() + 32);
	std::mt19937 random(284);
	for (const auto &directory : {first, second})
	{
		std::shuffle(names.begin(), names.end(), random);
		for (const auto &name : names)
			fs::create_directory(directory / name);
		fs::create_directory(directory / "_invalid");
		fs::create_directory(directory / "UPPERCASE");
		fs::create_directory(directory / fs::path(u8"caf\u00e9"));
		std::ofstream(directory / "aaa-file") << "regular file";
		std::error_code error;
		fs::create_directory_symlink(directory / expected.front(), directory / "aaa-link", error);
		CHECK(client::listProfiles(directory) == expected,
			"discovery returns the same sorted 32 profiles and excludes invalid entries");
	}
}

void checkEndpointSelection()
{
	client::JoinPreferences prefs;
	prefs.toggleFavorite({0x7f000001, 8001});
	prefs.recordRecent({0x7f000001, 8002});
	client::JoinDraft draft;
	draft.setEndpoint("127.0.0.1:7777");
	draft.expectedSeed = {'4', '2'};
	CHECK(draft.selectSavedEndpoint(prefs.favorites.front()) &&
		draft.request().endpoint == "127.0.0.1:8001" && draft.request().expectedSeed.empty(),
		"choosing a different favorite clears the seed");
	draft.expectedSeed = {'4', '3'};
	CHECK(draft.selectSavedEndpoint(prefs.recent.front()) &&
		draft.request().endpoint == "127.0.0.1:8002" && draft.request().expectedSeed.empty(),
		"choosing a different recent server clears the seed");
	draft.expectedSeed = {'4', '4'};
	draft.setEndpoint("127.0.0.1:08002");
	CHECK(!draft.selectSavedEndpoint(prefs.recent.front()) && draft.request().expectedSeed == "44",
		"reselecting the same numeric endpoint preserves the seed");
	draft.setProfile("another-profile");
	const auto request = draft.request();
	std::string error;
	const auto target = client::validateJoin(request.endpoint, request.expectedSeed, error);
	CHECK(request.profile == "another-profile" && target && target->expectedSeed == 44u && error.empty(),
		"profile changes preserve the manually entered seed for validation");
}

void checkInputs()
{
	std::string error;
	const auto address = client::parseEndpoint("127.0.0.1:65535");
	CHECK(address && client::formatEndpoint(*address) == "127.0.0.1:65535", "numeric endpoint round trips");
	for (const auto *input : {"localhost:7777", "127.0.0.1", "0.0.0.0:7777", "127.0.0.1:0",
		"127.0.0.1:65536", "127.0.0.1:-1", "127.0.0.1:7 extra", "127.0.0.999:7777"})
		CHECK(!client::validateJoin(input, "", error) && !error.empty(), "invalid endpoint explains the form error");
	const auto maxSeed = client::validateJoin("127.0.0.1:7777", "4294967295", error);
	CHECK(maxSeed && maxSeed->expectedSeed == UINT32_MAX && error.empty(), "seed uses the wire's full unsigned range");
	const auto withoutSeed = client::validateJoin("127.0.0.1:7777", "", error);
	CHECK(withoutSeed && !withoutSeed->expectedSeed, "blank seed disables the expected-seed check");
	CHECK(!client::validateJoin("127.0.0.1:7777", "4294967296", error) && !error.empty(),
		"overflowing seed is reported");
	CHECK(client::profileName("Alice_7-x") == "alice_7-x", "one profile name across case-sensitive and insensitive filesystems");
	for (const auto *name : {"", "../alice", "alice.key", "alice/bob", "A\\B", "two words", "CON", "Lpt2", "_name"})
		CHECK(!client::profileName(name), "unsafe profile name is rejected");
	CHECK(!client::profileName(std::string(33, 'a')), "oversized profile is rejected");
}

void checkProfilesAndPreferences(const fs::path &root)
{
	const auto named = root / "named";
	const auto alice = named / "alice";
	const auto bob = named / "bob";
	CHECK(rejectsProfile(alice, ProfileAccess::Load) && !fs::exists(alice), "join cannot mint a missing profile");
	const auto aliceKey = client::playerKey(alice, ProfileAccess::Create);
	const auto bobKey = client::playerKey(bob, ProfileAccess::Create);
	CHECK(aliceKey != bobKey && client::playerKey(alice, ProfileAccess::Load) == aliceKey,
		"named profiles have distinct stable keys");
	CHECK(rejectsProfile(alice, ProfileAccess::Create) && client::playerKey(alice, ProfileAccess::Load) == aliceKey,
		"duplicate creation preserves the existing key");
	CHECK(client::playerKey(alice, ProfileAccess::LoadOrCreate) == aliceKey,
		"CLI can resume a named profile through its existing directory option");
	const auto cli = root / "127.0.0.1-7777";
	const auto cliKey = client::playerKey(cli, ProfileAccess::LoadOrCreate);
	CHECK(cliKey == client::playerKey(cli, ProfileAccess::LoadOrCreate), "CLI creates and reuses its legacy key");
	fs::remove(bob / "player.key");
	CHECK(rejectsProfile(bob, ProfileAccess::Load) && rejectsProfile(bob, ProfileAccess::Create) &&
		rejectsProfile(bob, ProfileAccess::LoadOrCreate) && !fs::exists(bob / "player.key"),
		"missing existing key is never regenerated");
	std::ofstream(bob / "player.key") << "truncated";
	CHECK(rejectsProfile(bob, ProfileAccess::Load) && rejectsProfile(bob, ProfileAccess::LoadOrCreate) &&
		fs::file_size(bob / "player.key") == std::string_view("truncated").size(),
		"corrupt credentials cannot silently change identity");

	const auto settings = root / "join-settings.txt";
	client::JoinPreferences prefs;
	CHECK(prefs.load(settings) && prefs.recent.empty(), "fresh start without settings is valid");
	prefs.selectedProfile = "alice";
	for (uint16_t port = 4000; port < 4010; ++port)
	{
		const net::udp::Address address{0x7f000001, port};
		prefs.recordRecent(address);
		prefs.toggleFavorite(address);
	}
	CHECK(prefs.recent.size() == 8 && prefs.favorites.size() == 8 &&
		prefs.recent.front().port == 4009 && prefs.recent.back().port == 4002,
		"recent and favorite lists are ordered and bounded");
	prefs.toggleFavorite(prefs.favorites.front());
	prefs.recordRecent(prefs.recent.back());
	CHECK(prefs.save(settings), "first settings write");
	prefs.selectedProfile = "bob";
	CHECK(prefs.save(settings), "settings replace an existing file");
	client::JoinPreferences restored;
	CHECK(restored.load(settings) && restored.selectedProfile == "bob" &&
		restored.recent == prefs.recent && restored.favorites == prefs.favorites,
		"reload restores the profile selection and endpoint ordering");
	{
		std::ifstream file(settings, std::ios::binary);
		const std::string data{std::istreambuf_iterator<char>(file), {}};
		CHECK(data.find("profile bob\n") != std::string::npos &&
			data.find(std::string(aliceKey.begin(), aliceKey.end())) == std::string::npos,
			"settings store profile names and omit key bytes");
	}
	std::ofstream(settings) << "FTVOX-JOIN-1\nprofile ../../invalid\n";
	CHECK(!restored.load(settings) && restored.selectedProfile == "bob",
		"invalid settings cannot replace a valid in-memory selection");
}
}

int main()
{
	checkInputs();
	const TempDirectory root;
	checkProfileRollback(root.path);
	checkProfileDiscovery(root.path);
	checkEndpointSelection();
	checkProfilesAndPreferences(root.path);
	return testmain::exitCode();
}
