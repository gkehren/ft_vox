#include <Engine/ClientProfile.hpp>
#include <Engine/MultiplayerJoin.hpp>

#include "TestMain.hpp"

#include <fstream>
#include <iterator>

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
	CHECK(rejectsProfile(bob, ProfileAccess::Load), "corrupt credentials cannot silently change identity");

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
	checkProfilesAndPreferences(root.path);
	return testmain::exitCode();
}
