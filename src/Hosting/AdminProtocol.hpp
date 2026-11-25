#pragma once

#include <Hosting/RoleCredentials.hpp>
#include <World/MultiplayerSave.hpp>

#include <string_view>

namespace localcontrol
{
enum class Role : uint8_t { Player, Admin, Owner };

// Values 1–6 are the original on-disk role audit operations.
enum class Operation : uint8_t
{
	Bootstrap = 1, Provision, GrantAdmin, RevokeAdmin, Transfer, Recover,
	Kick, Ban, Unban, Allow, Unallow, EnableAllowlist, DisableAllowlist, Stop, List
};

enum class Result : uint8_t
{
	Ready, Applied, Forbidden, WrongWorld, Replay, Malformed, UnknownPlayer,
	ProtectedRole, NoChange, InvalidCredential, Offline, StorageError, ChannelError
};

inline constexpr size_t kMaxAudit = 64;
inline constexpr uint64_t kKickSeconds = 60;
inline constexpr std::array<char, 4> kReplyMagic{'F', 'T', 'A', 'R'};
inline constexpr size_t kReplyHeaderSize = 12;
inline constexpr size_t kReplyFixedSize = kReplyHeaderSize + 38;
inline constexpr size_t kPlayerSize = 14, kAuditSize = 17;
inline constexpr size_t kMaxReplySize =
	kReplyFixedSize + worldsave::kMaxPersistentPlayers * kPlayerSize + kMaxAudit * kAuditSize;

struct Request
{
	uint64_t world{}, epoch{}, sequence{};
	Operation operation{};
	sim::PlayerId target;
	std::optional<Bootstrap> delegation;
};

struct Player
{
	sim::PlayerId id;
	Role role{};
	bool banned{}, allowed{}, localAdmin{};
	uint64_t kickedUntil{};
};

struct Audit
{
	uint64_t revision{};
	Operation operation{};
	sim::PlayerId actor, target;
};

struct Reply
{
	uint64_t world{}, epoch{}, sequence{}, revision{};
	Result result{};
	Role role{};
	bool allowlist{};
	std::vector<Player> players;
	std::vector<Audit> audit;
};

inline constexpr size_t kRequestSize = 8 + 3 * sizeof(uint64_t) + 8 + 2 * ControlKey{}.size();
using RequestFrame = std::array<uint8_t, kRequestSize>;

bool validActor(sim::PlayerId id);
bool targeted(Operation operation);
bool permitted(Role role, Operation operation, Role target = Role::Player);
std::string_view label(Operation operation);
std::string_view label(Result result);
std::string_view label(Role role);
uint64_t unixSeconds();

RequestFrame encodeRequest(const Request &request);
std::optional<Request> decodeRequest(const RequestFrame &frame);
std::vector<uint8_t> encodeReply(const Reply &reply);
std::optional<size_t> replySize(std::span<const uint8_t> header);
std::optional<Reply> decodeReply(std::span<const uint8_t> frame);
}
