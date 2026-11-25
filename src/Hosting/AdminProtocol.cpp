#include <Hosting/AdminProtocol.hpp>

#include <algorithm>
#include <chrono>

namespace localcontrol
{
namespace
{
constexpr std::array<char, 4> kRequestMagic{'F', 'T', 'A', 'Q'};
constexpr uint32_t kVersion = 1;
}

bool validActor(sim::PlayerId id)
{
	return id.value >= worldsave::kFirstRemotePlayerId && id.value <= sim::kMaxPlayerId;
}

bool targeted(Operation op)
{
	return op == Operation::GrantAdmin || op == Operation::RevokeAdmin ||
		(op >= Operation::Kick && op <= Operation::Unallow);
}

bool permitted(Role role, Operation op, Role target)
{
	if (role == Role::Player || op < Operation::GrantAdmin ||
		op == Operation::Transfer || op == Operation::Recover || op > Operation::List)
		return false;
	if (op == Operation::List)
		return true;
	if (target == Role::Owner || (role == Role::Admin && target == Role::Admin))
		return false;
	return role == Role::Owner || op == Operation::Kick || op == Operation::Ban || op == Operation::Unban;
}

std::string_view label(Operation operation)
{
	switch (operation)
	{
	case Operation::Bootstrap: return "bootstrap";
	case Operation::Provision: return "provision";
	case Operation::GrantAdmin: return "grant-admin";
	case Operation::RevokeAdmin: return "revoke-admin";
	case Operation::Transfer: return "transfer";
	case Operation::Recover: return "recover";
	case Operation::Kick: return "kick";
	case Operation::Ban: return "ban";
	case Operation::Unban: return "unban";
	case Operation::Allow: return "allow";
	case Operation::Unallow: return "unallow";
	case Operation::EnableAllowlist: return "enable-allowlist";
	case Operation::DisableAllowlist: return "disable-allowlist";
	case Operation::Stop: return "stop";
	case Operation::List: return "list";
	}
	return "invalid";
}

std::string_view label(Result result)
{
	switch (result)
	{
	case Result::Ready: return "Administration status received.";
	case Result::Applied: return "Command accepted.";
	case Result::Forbidden: return "This local controller lacks permission.";
	case Result::WrongWorld: return "The request belongs to another server launch.";
	case Result::Replay: return "The request sequence was already used or skipped.";
	case Result::Malformed: return "The request was malformed or exceeded the control queue.";
	case Result::UnknownPlayer: return "The target profile is unknown.";
	case Result::ProtectedRole: return "The target role is protected.";
	case Result::NoChange: return "The requested policy is already in effect.";
	case Result::InvalidCredential: return "The selected local profile could not be authorized for this player.";
	case Result::Offline: return "The target has no admitted session.";
	case Result::StorageError: return "The server could not confirm policy storage. Check its log.";
	case Result::ChannelError: return "The server did not confirm the command. Check the server state before retrying.";
	}
	return "Unknown result.";
}

std::string_view label(Role role)
{
	switch (role)
	{
	case Role::Owner: return "Owner";
	case Role::Admin: return "Admin";
	case Role::Player: return "Player";
	}
	return "Invalid";
}

uint64_t unixSeconds()
{
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

RequestFrame encodeRequest(const Request &request)
{
	worldsave::ByteWriter writer;
	writer.raw(kRequestMagic.data(), kRequestMagic.size());
	writer.u32(kVersion);
	writer.u64(request.world);
	writer.u64(request.epoch);
	writer.u64(request.sequence);
	writer.u8(static_cast<uint8_t>(request.operation));
	writer.u8(request.delegation.has_value());
	writer.u16(0);
	writer.u32(request.target.value);
	const auto delegation = request.delegation.value_or(Bootstrap{});
	writer.raw(delegation.player.data(), delegation.player.size());
	writer.raw(delegation.control.data(), delegation.control.size());
	RequestFrame frame{};
	std::ranges::copy(writer.take(), frame.begin());
	return frame;
}

std::optional<Request> decodeRequest(const RequestFrame &frame)
{
	worldsave::ByteReader reader(frame.data(), frame.size());
	std::array<char, kRequestMagic.size()> magic{};
	reader.raw(magic.data(), magic.size());
	const auto version = reader.u32();
	Request request;
	request.world = reader.u64();
	request.epoch = reader.u64();
	request.sequence = reader.u64();
	request.operation = static_cast<Operation>(reader.u8());
	const auto hasDelegation = reader.u8();
	const auto reserved = reader.u16();
	request.target = {reader.u32()};
	Bootstrap delegation{};
	reader.raw(delegation.player.data(), delegation.player.size());
	reader.raw(delegation.control.data(), delegation.control.size());
	if (reader.failed() || magic != kRequestMagic || version != kVersion || reserved || hasDelegation > 1 ||
		!request.sequence || request.operation < Operation::GrantAdmin || request.operation > Operation::List ||
		request.operation == Operation::Transfer || request.operation == Operation::Recover ||
		(targeted(request.operation) ? !validActor(request.target) : request.target.valid()))
		return std::nullopt;
	if (hasDelegation)
	{
		if (request.operation != Operation::GrantAdmin || !validBootstrap(delegation))
			return std::nullopt;
		request.delegation = delegation;
	}
	else if (delegation.player != net::wire::PlayerKey{} || delegation.control != ControlKey{})
		return std::nullopt;
	return request;
}

std::vector<uint8_t> encodeReply(const Reply &reply)
{
	worldsave::ByteWriter writer;
	writer.raw(kReplyMagic.data(), kReplyMagic.size());
	writer.u32(kVersion);
	writer.u32(static_cast<uint32_t>(kReplyFixedSize + reply.players.size() * kPlayerSize + reply.audit.size() * kAuditSize));
	writer.u64(reply.world);
	writer.u64(reply.epoch);
	writer.u64(reply.sequence);
	writer.u64(reply.revision);
	writer.u8(static_cast<uint8_t>(reply.result));
	writer.u8(static_cast<uint8_t>(reply.role));
	writer.u8(reply.allowlist);
	writer.u16(static_cast<uint16_t>(reply.players.size()));
	writer.u8(static_cast<uint8_t>(reply.audit.size()));
	for (const auto &player : reply.players)
	{
		writer.u32(player.id.value);
		writer.u8(static_cast<uint8_t>(player.role));
		writer.u8((player.banned ? 1 : 0) | (player.allowed ? 2 : 0) | (player.localAdmin ? 4 : 0));
		writer.u64(player.kickedUntil);
	}
	for (const auto &event : reply.audit)
	{
		writer.u64(event.revision);
		writer.u8(static_cast<uint8_t>(event.operation));
		writer.u32(event.actor.value);
		writer.u32(event.target.value);
	}
	return writer.take();
}

std::optional<size_t> replySize(std::span<const uint8_t> header)
{
	if (header.size() != kReplyHeaderSize)
		return std::nullopt;
	worldsave::ByteReader reader(header.data(), header.size());
	std::array<char, kReplyMagic.size()> magic{};
	reader.raw(magic.data(), magic.size());
	const auto version = reader.u32();
	const auto size = reader.u32();
	if (magic != kReplyMagic || version != kVersion || size < kReplyFixedSize || size > kMaxReplySize)
		return std::nullopt;
	return size;
}

std::optional<Reply> decodeReply(std::span<const uint8_t> frame)
{
	if (frame.size() < kReplyHeaderSize || replySize(frame.first(kReplyHeaderSize)) != frame.size())
		return std::nullopt;
	worldsave::ByteReader reader(frame.data() + kReplyHeaderSize, frame.size() - kReplyHeaderSize);
	Reply reply;
	reply.world = reader.u64();
	reply.epoch = reader.u64();
	reply.sequence = reader.u64();
	reply.revision = reader.u64();
	reply.result = static_cast<Result>(reader.u8());
	reply.role = static_cast<Role>(reader.u8());
	const auto allowlist = reader.u8();
	reply.allowlist = allowlist != 0;
	const auto count = reader.u16();
	const auto events = reader.u8();
	if (!reply.world || !reply.epoch || reply.result >= Result::ChannelError || reply.role > Role::Owner ||
		allowlist > 1 || count > worldsave::kMaxPersistentPlayers || events > kMaxAudit ||
		(reply.role == Role::Player && (count || events || allowlist || reply.revision)) ||
		frame.size() != kReplyFixedSize + count * kPlayerSize + events * kAuditSize)
		return std::nullopt;
	reply.players.reserve(count);
	for (unsigned i = 0; i < count; ++i)
	{
		Player player;
		player.id = {reader.u32()};
		player.role = static_cast<Role>(reader.u8());
		const auto flags = reader.u8();
		player.banned = (flags & 1) != 0;
		player.allowed = (flags & 2) != 0;
		player.localAdmin = (flags & 4) != 0;
		player.kickedUntil = reader.u64();
		if (!validActor(player.id) || player.role > Role::Owner || flags > 7 ||
			(player.localAdmin && player.role != Role::Admin) ||
			(i && player.id.value <= reply.players.back().id.value))
			return std::nullopt;
		reply.players.push_back(player);
	}
	for (unsigned i = 0; i < events; ++i)
	{
		const Audit event{reader.u64(), static_cast<Operation>(reader.u8()), {reader.u32()}, {reader.u32()}};
		if (!event.revision || event.revision > reply.revision || event.operation < Operation::Bootstrap ||
			event.operation >= Operation::List || (event.actor.valid() && !validActor(event.actor)) ||
			(event.target.valid() && !validActor(event.target)) ||
			(i && event.revision != reply.audit.back().revision + 1))
			return std::nullopt;
		reply.audit.push_back(event);
	}
	if (reader.failed() || (events && reply.audit.back().revision != reply.revision))
		return std::nullopt;
	return reply;
}
}
