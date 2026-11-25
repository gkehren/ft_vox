#include <Server/WorldRoles.hpp>

#include <World/WorldCheckpoint.hpp>

#include <algorithm>
#include <iostream>
#include <ostream>
#include <set>
#include <string_view>

namespace server
{
namespace
{
namespace fs = std::filesystem;
namespace ws = worldsave;
using net::wire::PlayerKey;
constexpr std::array<uint8_t, 4> kMagic{'F', 'T', 'R', 'L'};
constexpr uint32_t kVersion = 2;
constexpr size_t kLegacyFixedBytes = 108; // header, owner binding, counts and checksum
constexpr size_t kFixedBytes = kLegacyFixedBytes + 12;
constexpr size_t kAdminBytes = 36, kAccessBytes = 13;
constexpr size_t kMaxState = kFixedBytes + ws::kMaxPersistentPlayers * (2 * kAdminBytes + kAccessBytes) +
	localcontrol::kMaxAudit * localcontrol::kAuditSize;

bool isActor(sim::PlayerId id)
{
	return id.value >= ws::kFirstRemotePlayerId && id.value <= sim::kMaxPlayerId;
}

bool fail(std::string &error, std::string_view message)
{
	error = message;
	return false;
}
}

WorldRoles::WorldRoles(const fs::path &worldDir, WorldIdentities &identities)
	: m_directory(worldDir.parent_path() / ".ft_vox..server-roles"),
	  m_path(m_directory / worldDir.filename()), m_pending(m_directory / ".pending.."),
	  m_identities(identities), m_load(Load::Invalid)
{
	m_pending += worldDir.filename().native();
	m_load = load();
}

WorldRoles::Load WorldRoles::load()
{
	const auto floor = m_identities.roleRevision();
	std::error_code error;
	const auto directory = fs::symlink_status(m_directory, error).type();
	if (directory == fs::file_type::not_found)
		return floor ? Load::Missing : Load::Unowned;
	if (error || directory != fs::file_type::directory)
		return Load::Invalid;

	const auto current = read(m_path, m_state);
	if (current == Load::WrongWorld || (current == Load::Present && m_state.revision >= floor))
		return current;

	State pending;
	if (read(m_pending, pending) == Load::Present && identitiesMatch(pending))
	{
		const bool firstBootstrap = current == Load::Missing && !floor &&
			pending.revision == 1 && !pending.owner.valid();
		if (pending.revision == floor || firstBootstrap)
		{
			if (!m_identities.advanceRoleRevision(pending.revision) || !promotePending())
				return Load::Invalid;
			m_state = std::move(pending);
			return Load::Present;
		}
	}
	if (current == Load::Present)
		return Load::Stale;
	return current == Load::Missing && !floor ? Load::Unowned : current;
}

WorldRoles::Load WorldRoles::read(const fs::path &path, State &state) const
{
	std::error_code error;
	const auto file = fs::symlink_status(path, error);
	if (file.type() == fs::file_type::not_found)
		return Load::Missing;
	if (error || file.type() != fs::file_type::regular)
		return Load::Invalid;
	std::vector<uint8_t> bytes;
	uint64_t worldId = 0;
	if (!localcontrol::readPrivateFile(path, bytes, kMaxState) || !decode(bytes, worldId, state))
		return Load::Invalid;
	return worldId == m_identities.worldId() ? Load::Present : Load::WrongWorld;
}

bool WorldRoles::decode(const std::vector<uint8_t> &bytes, uint64_t &worldId, State &next) const
{
	if (bytes.size() < kLegacyFixedBytes)
		return false;
	const auto payloadSize = bytes.size() - sizeof(uint64_t);
	ws::ByteReader checksum(bytes.data() + payloadSize, sizeof(uint64_t));
	if (checksum.u64() != ws::fnv1a64(bytes.data(), payloadSize))
		return false;
	ws::ByteReader reader(bytes.data(), payloadSize);
	std::array<uint8_t, kMagic.size()> foundMagic{};
	reader.raw(foundMagic.data(), foundMagic.size());
	const auto version = reader.u32();
	if (foundMagic != kMagic || (version != 1 && version != kVersion))
		return false;
	worldId = reader.u64();
	next.revision = reader.u64();
	next.owner = {reader.u32()};
	reader.raw(next.ownerProfile.data(), next.ownerProfile.size());
	reader.raw(next.control.data(), next.control.size());
	const auto admins = reader.u32(), events = reader.u32();
	const auto delegates = version == kVersion ? reader.u32() : 0;
	const auto policies = version == kVersion ? reader.u32() : 0;
	const auto allowlist = version == kVersion ? reader.u32() : 0;
	next.allowlist = allowlist != 0;
	const auto fixedBytes = version == kVersion ? kFixedBytes : kLegacyFixedBytes;
	if (!worldId || !next.revision || (next.owner.valid() && !isActor(next.owner)) ||
		!localcontrol::validBootstrap({next.ownerProfile, next.control}) ||
		admins >= ws::kMaxPersistentPlayers || delegates > admins || policies >= ws::kMaxPersistentPlayers ||
		allowlist > 1 || ((admins || policies || allowlist) && !next.owner.valid()) ||
		!events || events > localcontrol::kMaxAudit || bytes.size() != fixedBytes +
		(admins + delegates) * kAdminBytes + policies * kAccessBytes + events * localcontrol::kAuditSize)
		return false;
	uint32_t previous = ws::kFirstRemotePlayerId - 1;
	std::set<PlayerKey> keys{next.ownerProfile};
	for (uint32_t i = 0; i < admins; ++i)
	{
		const sim::PlayerId id{reader.u32()};
		PlayerKey key{};
		reader.raw(key.data(), key.size());
		if (id.value <= previous || id.value > sim::kMaxPlayerId || id == next.owner ||
			key == PlayerKey{} || !keys.insert(key).second)
			return false;
		next.admins.emplace(id, key);
		previous = id.value;
	}
	keys.insert(next.control);
	previous = ws::kFirstRemotePlayerId - 1;
	for (uint32_t i = 0; i < delegates; ++i)
	{
		const sim::PlayerId id{reader.u32()};
		localcontrol::ControlKey key{};
		reader.raw(key.data(), key.size());
		if (id.value <= previous || !next.admins.contains(id) || key == localcontrol::ControlKey{} ||
			!keys.insert(key).second)
			return false;
		next.delegates.emplace(id, key);
		previous = id.value;
	}
	previous = ws::kFirstRemotePlayerId - 1;
	for (uint32_t i = 0; i < policies; ++i)
	{
		const sim::PlayerId id{reader.u32()};
		const auto flags = reader.u8();
		const auto until = reader.u64();
		if (id.value <= previous || !isActor(id) || id == next.owner || flags > 3 ||
			(!flags && !until) || ((flags & 1) && until))
			return false;
		next.access.emplace(id, Access{(flags & 1) != 0, (flags & 2) != 0, until});
		previous = id.value;
	}
	for (uint32_t i = 0; i < events; ++i)
	{
		localcontrol::Audit event{reader.u64(), static_cast<RoleOperation>(reader.u8()), {reader.u32()}, {reader.u32()}};
		const auto lastOperation = version == 1 ? RoleOperation::Recover : RoleOperation::Stop;
		const bool optionalTarget = event.operation == RoleOperation::Bootstrap || event.operation >= RoleOperation::EnableAllowlist;
		if (!event.revision || event.revision > next.revision || event.operation < RoleOperation::Bootstrap ||
			event.operation > lastOperation || (event.actor.valid() && !isActor(event.actor)) ||
			(!isActor(event.target) && (event.target.valid() || !optionalTarget)) ||
			(event.operation >= RoleOperation::EnableAllowlist && event.target.valid()) ||
			(event.operation >= RoleOperation::Kick && !event.actor.valid()) ||
			(i && event.revision != next.audit.back().revision + 1))
			return false;
		if (version == 1 && (event.operation == RoleOperation::Bootstrap || event.operation == RoleOperation::Recover))
			event.actor = {};
		next.audit.push_back(event);
	}
	if (reader.failed() || next.audit.back().revision != next.revision ||
		(!next.owner.valid() && (next.revision != 1 || next.audit.back().operation != RoleOperation::Bootstrap ||
			next.audit.back().target.valid())))
		return false;
	return true;
}

bool WorldRoles::identitiesMatch(const State &state) const
{
	const auto matches = [this](sim::PlayerId id, const PlayerKey &key) {
		const auto *registered = m_identities.keyFor(id);
		return registered && *registered == key;
	};
	return (!state.owner.valid() || matches(state.owner, state.ownerProfile)) &&
		std::ranges::all_of(state.admins, [&](const auto &entry) { return matches(entry.first, entry.second); }) &&
		std::ranges::all_of(state.access, [&](const auto &entry) { return m_identities.contains(entry.first); });
}

bool WorldRoles::promotePending()
{
	std::error_code error;
	fs::rename(m_pending, m_path, error);
	if (error)
		return false;
	ws::syncDirForCheckpoint(m_directory);
	return true;
}

bool WorldRoles::commit(State next, RoleOperation operation, sim::PlayerId target, std::string &error)
{
	const bool ownerCommand = operation == RoleOperation::GrantAdmin || operation == RoleOperation::RevokeAdmin ||
		operation == RoleOperation::Transfer;
	return commit(std::move(next), operation, target, ownerCommand ? m_state.owner : sim::PlayerId{}, error);
}

bool WorldRoles::commit(State next, RoleOperation operation, sim::PlayerId target, sim::PlayerId actor, std::string &error)
{
	if (next.revision == UINT64_MAX)
		return fail(error, "role revision space exhausted");
	++next.revision;
	if (next.audit.size() == localcontrol::kMaxAudit)
		next.audit.erase(next.audit.begin());
	next.audit.push_back({next.revision, operation, actor, target});
	ws::ByteWriter writer;
	writer.raw(kMagic.data(), kMagic.size());
	writer.u32(kVersion);
	writer.u64(m_identities.worldId());
	writer.u64(next.revision);
	writer.u32(next.owner.value);
	writer.raw(next.ownerProfile.data(), next.ownerProfile.size());
	writer.raw(next.control.data(), next.control.size());
	writer.u32(static_cast<uint32_t>(next.admins.size()));
	writer.u32(static_cast<uint32_t>(next.audit.size()));
	writer.u32(static_cast<uint32_t>(next.delegates.size()));
	writer.u32(static_cast<uint32_t>(next.access.size()));
	writer.u32(next.allowlist);
	for (const auto &[id, key] : next.admins)
	{
		writer.u32(id.value);
		writer.raw(key.data(), key.size());
	}
	for (const auto &[id, key] : next.delegates)
	{
		writer.u32(id.value);
		writer.raw(key.data(), key.size());
	}
	for (const auto &[id, access] : next.access)
	{
		writer.u32(id.value);
		writer.u8((access.banned ? 1 : 0) | (access.allowed ? 2 : 0));
		writer.u64(access.kickedUntil);
	}
	for (const auto &event : next.audit)
	{
		writer.u64(event.revision);
		writer.u8(static_cast<uint8_t>(event.operation));
		writer.u32(event.actor.value);
		writer.u32(event.target.value);
	}
	auto bytes = writer.take();
	ws::ByteWriter check;
	check.u64(ws::fnv1a64(bytes.data(), bytes.size()));
	const auto checksum = check.take();
	bytes.insert(bytes.end(), checksum.begin(), checksum.end());

	std::error_code ec;
	if (fs::symlink_status(m_directory, ec).type() == fs::file_type::not_found)
	{
		fs::create_directory(m_directory, ec);
		if (!ec)
			ws::syncDirForCheckpoint(m_directory.parent_path());
	}
	if (fs::symlink_status(m_directory, ec).type() != fs::file_type::directory || ec ||
		!localcontrol::writePrivateFile(m_pending, bytes, true))
		return fail(error, "could not stage private role authority; check the saves root and its file permissions");
	if (!m_identities.advanceRoleRevision(next.revision))
		return fail(error, "could not advance the role revision in the identity registry");
	if (!promotePending())
	{
		m_load = Load::Invalid;
		return fail(error, "role revision recorded but publication could not finish; inspect offline status before retrying");
	}
	m_state = std::move(next);
	m_load = Load::Present;
	return true;
}

bool WorldRoles::prepareStart(bool fresh, const std::optional<localcontrol::Bootstrap> &bootstrap, std::string &error)
{
	if (m_load == Load::Unowned)
	{
		if (!bootstrap)
			return true;
		if (!fresh)
			return fail(error, "existing world has no owner; open it with the CLI first if a checkpoint is needed, then use offline roles provision");
		if (!localcontrol::validBootstrap(*bootstrap))
			return fail(error, "invalid local owner bootstrap");
		State next;
		next.ownerProfile = bootstrap->player;
		next.control = bootstrap->control;
		if (!commit(std::move(next), RoleOperation::Bootstrap, {}, error))
			return false;
	}
	if (m_load != Load::Present || !identitiesMatch(m_state))
		return fail(error, "role authority missing, corrupt, insecure or mismatched; inspect it with the offline roles command");
	if (bootstrap && bootstrap->player == m_state.ownerProfile && !localcontrol::matches(bootstrap->control, m_state.control))
		return fail(error, "owner credential missing, unreadable or mismatched; restore the private owner-control.key or use offline roles recover");
	if (!m_identities.advanceRoleRevision(m_state.revision))
		return fail(error, "could not reconcile the role revision in the identity registry");
	return true;
}

sim::PlayerId WorldRoles::enroll(const PlayerKey &profile)
{
	if (auto id = m_identities.find(profile); id.valid())
		return id;
	if (m_identities.size() >= ws::kMaxPersistentPlayers)
		return {};
	const auto id = m_identities.allocatePlayer();
	return id.valid() && m_identities.enroll(id, profile) ? id : sim::PlayerId{};
}

bool WorldRoles::finishStart(std::string &error)
{
	if (m_load != Load::Present || m_state.owner.valid())
		return true;
	State next = m_state;
	next.owner = enroll(next.ownerProfile);
	if (!next.owner.valid())
		return fail(error, "could not enroll the reserved owner profile");
	const auto owner = next.owner;
	return commit(std::move(next), RoleOperation::Bootstrap, owner, error);
}

Role WorldRoles::role(sim::PlayerId id) const
{
	if (m_load == Load::Present && m_state.owner.valid())
	{
		if (id == m_state.owner)
			return Role::Owner;
		if (m_state.admins.contains(id))
			return Role::Admin;
	}
	return Role::Player;
}

sim::PlayerId WorldRoles::controller(const std::optional<localcontrol::Bootstrap> &proof) const
{
	if (m_load != Load::Present || !proof)
		return {};
	if (proof->player == m_state.ownerProfile && localcontrol::matches(proof->control, m_state.control))
		return m_state.owner;
	const auto actor = m_identities.find(proof->player);
	const auto delegate = m_state.delegates.find(actor);
	return delegate != m_state.delegates.end() && localcontrol::matches(proof->control, delegate->second) &&
		admits(actor, localcontrol::unixSeconds()) ? actor : sim::PlayerId{};
}

bool WorldRoles::isControlKey(const localcontrol::ControlKey &key) const
{
	return m_load == Load::Present && (localcontrol::matches(key, m_state.control) ||
		std::ranges::any_of(m_state.delegates, [&](const auto &entry) { return localcontrol::matches(key, entry.second); }));
}

bool WorldRoles::admits(sim::PlayerId id, uint64_t now) const
{
	if (!healthy())
		return false;
	if (role(id) == Role::Owner)
		return true;
	const auto access = m_state.access.find(id);
	if (access == m_state.access.end())
		return !m_state.allowlist;
	return !access->second.banned && access->second.kickedUntil <= now &&
		(!m_state.allowlist || access->second.allowed);
}

void WorldRoles::fillAdminState(localcontrol::Reply &reply, sim::PlayerId actor) const
{
	reply.role = role(actor);
	if (reply.role == Role::Player)
		return;
	reply.revision = m_state.revision;
	reply.allowlist = m_state.allowlist;
	reply.audit = m_state.audit;
	reply.players.reserve(m_identities.size());
	for (const auto &[id, key] : m_identities.profiles())
	{
		const auto position = m_state.access.find(id);
		const auto access = position == m_state.access.end() ? Access{} : position->second;
		reply.players.push_back({id, role(id), access.banned, access.allowed, m_state.delegates.contains(id), access.kickedUntil});
	}
}

localcontrol::Result WorldRoles::moderate(sim::PlayerId actor, const localcontrol::Request &request, bool online, uint64_t now)
{
	using localcontrol::Operation;
	using localcontrol::Result;
	const auto operation = request.operation;
	const auto actorRole = role(actor);
	if (!localcontrol::permitted(actorRole, operation))
		return Result::Forbidden;
	if (operation == Operation::List)
		return Result::Applied;
	const auto targetRole = role(request.target);
	if (!localcontrol::permitted(actorRole, operation, targetRole))
		return Result::ProtectedRole;
	const auto *profile = m_identities.keyFor(request.target);
	if (localcontrol::targeted(operation) && !profile)
		return Result::UnknownPlayer;
	if (operation == Operation::Kick && !online)
		return Result::Offline;

	State next = m_state;
	if (operation == Operation::GrantAdmin)
	{
		const bool added = next.admins.emplace(request.target, *profile).second;
		if (request.delegation)
		{
			const auto &proof = *request.delegation;
			const auto existing = next.delegates.find(request.target);
			if (proof.player == *profile && existing != next.delegates.end() &&
				localcontrol::matches(proof.control, existing->second))
				return Result::NoChange;
			if (!localcontrol::validBootstrap(proof) || proof.player != *profile ||
				m_identities.find(proof.control).valid() || isControlKey(proof.control))
				return Result::InvalidCredential;
			next.delegates.insert_or_assign(request.target, proof.control);
		}
		else if (!added)
			return Result::NoChange;
	}
	else if (operation == Operation::RevokeAdmin)
	{
		if (!next.admins.erase(request.target))
			return Result::NoChange;
		next.delegates.erase(request.target);
	}
	else if (operation >= Operation::Kick && operation <= Operation::Unallow)
	{
		auto &access = next.access[request.target];
		if (access.kickedUntil <= now)
			access.kickedUntil = 0;
		switch (operation)
		{
		case Operation::Kick:
			if (access.banned || access.kickedUntil)
				return Result::NoChange;
			access.kickedUntil = now + localcontrol::kKickSeconds;
			break;
		case Operation::Ban:
		case Operation::Unban:
			if (access.banned == (operation == Operation::Ban))
				return Result::NoChange;
			access.banned = operation == Operation::Ban;
			access.kickedUntil = 0;
			break;
		case Operation::Allow:
		case Operation::Unallow:
			if (access.allowed == (operation == Operation::Allow))
				return Result::NoChange;
			access.allowed = operation == Operation::Allow;
			break;
		default: break;
		}
		if (!access.banned && !access.allowed && !access.kickedUntil)
			next.access.erase(request.target);
	}
	else if (operation == Operation::EnableAllowlist || operation == Operation::DisableAllowlist)
	{
		const bool enabled = operation == Operation::EnableAllowlist;
		if (next.allowlist == enabled)
			return Result::NoChange;
		next.allowlist = enabled;
	}

	std::string error;
	if (commit(std::move(next), operation, request.target, actor, error))
		return Result::Applied;
	std::cerr << "[server] " << error << '\n';
	return Result::StorageError;
}

bool WorldRoles::apply(const RoleRequest &request, std::string &error)
{
	if (request.worldId != m_identities.worldId() || m_load == Load::WrongWorld)
		return fail(error, "role request belongs to a different world");
	const auto revision = m_load == Load::Present ? m_state.revision : m_identities.roleRevision();
	if (request.revision != revision)
		return fail(error, "stale role revision; query offline status again");
	switch (request.operation)
	{
	case RoleOperation::Provision:
	case RoleOperation::Recover:
		return operatorChange(request, error);
	case RoleOperation::GrantAdmin:
	case RoleOperation::RevokeAdmin:
	case RoleOperation::Transfer:
		return ownerChange(request, error);
	default:
		return fail(error, "invalid offline role operation");
	}
}

bool WorldRoles::operatorChange(const RoleRequest &request, std::string &error)
{
	const bool provisioning = request.operation == RoleOperation::Provision;
	if (provisioning != (m_load == Load::Unowned))
		return fail(error, provisioning ? "world already has a role authority" : "unowned world requires provision");
	if (request.target.valid() || request.authorization || !request.profile || !request.newControl ||
		!localcontrol::validBootstrap({*request.profile, *request.newControl}) ||
		m_identities.find(*request.newControl).valid() ||
		(m_state.revision && localcontrol::matches(m_state.control, *request.newControl)) ||
		isControlKey(*request.newControl))
		return fail(error, "operation requires a profile key and a distinct new local owner credential");
	State next;
	if (m_load == Load::Present)
	{
		next.revision = m_state.revision;
		next.audit = m_state.audit;
		next.access = m_state.access;
		next.allowlist = m_state.allowlist;
	}
	else
		next.revision = m_identities.roleRevision();
	if (next.revision == UINT64_MAX)
		return fail(error, "role revision space exhausted");
	if (!m_identities.markCheckpointRequired())
		return fail(error, "could not seal the dedicated registry");
	next.owner = enroll(*request.profile);
	if (!next.owner.valid())
		return fail(error, "could not enroll the requested owner identity");
	next.ownerProfile = *request.profile;
	next.control = *request.newControl;
	next.access.erase(next.owner);
	const auto owner = next.owner;
	return commit(std::move(next), request.operation, owner, error);
}

bool WorldRoles::ownerChange(const RoleRequest &request, std::string &error)
{
	if (m_load != Load::Present || !m_state.owner.valid() || !identitiesMatch(m_state) ||
		!request.authorization || !localcontrol::matches(m_state.control, *request.authorization))
		return fail(error, "a valid local owner credential is required");
	const auto *target = m_identities.keyFor(request.target);
	if (!target || request.target == m_state.owner || request.profile)
		return fail(error, "choose a registered player other than the current owner");
	if ((request.operation == RoleOperation::Transfer) != request.newControl.has_value())
		return fail(error, "only transfer requires a new local owner credential");

	State next = m_state;
	if (request.operation == RoleOperation::GrantAdmin)
	{
		if (!next.admins.emplace(request.target, *target).second)
			return fail(error, "player is already an admin");
	}
	else if (request.operation == RoleOperation::RevokeAdmin)
	{
		if (!next.admins.erase(request.target))
			return fail(error, "player is not an admin");
		next.delegates.erase(request.target);
	}
	else
	{
		if (*request.newControl == localcontrol::ControlKey{} ||
			isControlKey(*request.newControl) || m_identities.find(*request.newControl).valid())
			return fail(error, "transfer requires a distinct new local owner credential");
		next.owner = request.target;
		next.ownerProfile = *target;
		next.control = *request.newControl;
		next.admins.erase(request.target);
		next.delegates.erase(request.target);
		next.access.erase(request.target);
	}
	return commit(std::move(next), request.operation, request.target, error);
}

void WorldRoles::printStatus(std::ostream &output) const
{
	const auto status = m_load == Load::Unowned ? "unowned" : m_load == Load::Missing ? "missing" :
		m_load == Load::Invalid ? "invalid" : m_load == Load::WrongWorld ? "wrong_world" :
		m_load == Load::Stale ? "stale" : !identitiesMatch(m_state) ? "identity_mismatch" :
		m_state.owner.valid() ? "owned" : "pending";
	output << "ROLES world_id=" << m_identities.worldId() << " revision="
		   << (m_load == Load::Present ? m_state.revision : m_identities.roleRevision()) << " status=" << status << '\n';
	if (m_load != Load::Present)
		return;
	output << "ROLES owner_id=" << m_state.owner.value << '\n';
	for (const auto &entry : m_state.admins)
		output << "ROLES admin_id=" << entry.first.value << '\n';
	for (const auto &event : m_state.audit)
		output << "ROLE_AUDIT revision=" << event.revision << " operation=" << localcontrol::label(event.operation)
			   << " actor_id=" << event.actor.value << " target_id=" << event.target.value << '\n';
}
}
