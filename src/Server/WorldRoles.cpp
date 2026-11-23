#include <Server/WorldRoles.hpp>

#include <World/WorldCheckpoint.hpp>

#include <algorithm>
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
constexpr uint32_t kVersion = 1;
constexpr size_t kFixedBytes = 108; // header, owner binding, counts and checksum
constexpr size_t kAdminBytes = 36, kAuditBytes = 17, kMaxAudit = 64;
constexpr size_t kMaxState = kFixedBytes + ws::kMaxPersistentPlayers * kAdminBytes + kMaxAudit * kAuditBytes;

bool isActor(sim::PlayerId id)
{
	return id.value >= ws::kFirstRemotePlayerId && id.value <= sim::kMaxPlayerId;
}

bool fail(std::string &error, std::string_view message)
{
	error = message;
	return false;
}

std::string_view name(RoleOperation operation)
{
	switch (operation)
	{
	case RoleOperation::Bootstrap: return "bootstrap";
	case RoleOperation::Provision: return "provision";
	case RoleOperation::GrantAdmin: return "grant-admin";
	case RoleOperation::RevokeAdmin: return "revoke-admin";
	case RoleOperation::Transfer: return "transfer";
	case RoleOperation::Recover: return "recover";
	}
	return "invalid";
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
#ifndef _WIN32
	if ((file.permissions() & (fs::perms::group_all | fs::perms::others_all)) != fs::perms::none)
		return Load::Invalid;
#endif
	std::vector<uint8_t> bytes;
	uint64_t worldId = 0;
	if (ws::readFileInto(path, bytes, kMaxState) != ws::SaveStatus::Ok || !decode(bytes, worldId, state))
		return Load::Invalid;
	return worldId == m_identities.worldId() ? Load::Present : Load::WrongWorld;
}

bool WorldRoles::decode(const std::vector<uint8_t> &bytes, uint64_t &worldId, State &next) const
{
	if (bytes.size() < kFixedBytes)
		return false;
	const auto payloadSize = bytes.size() - sizeof(uint64_t);
	ws::ByteReader checksum(bytes.data() + payloadSize, sizeof(uint64_t));
	if (checksum.u64() != ws::fnv1a64(bytes.data(), payloadSize))
		return false;
	ws::ByteReader reader(bytes.data(), payloadSize);
	std::array<uint8_t, kMagic.size()> foundMagic{};
	reader.raw(foundMagic.data(), foundMagic.size());
	if (foundMagic != kMagic || reader.u32() != kVersion)
		return false;
	worldId = reader.u64();
	next.revision = reader.u64();
	next.owner = {reader.u32()};
	reader.raw(next.ownerProfile.data(), next.ownerProfile.size());
	reader.raw(next.control.data(), next.control.size());
	const auto admins = reader.u32(), events = reader.u32();
	if (!worldId || !next.revision || (next.owner.valid() && !isActor(next.owner)) ||
		!localcontrol::validBootstrap({next.ownerProfile, next.control}) ||
		admins >= ws::kMaxPersistentPlayers || (admins && !next.owner.valid()) || !events || events > kMaxAudit ||
		bytes.size() != kFixedBytes + admins * kAdminBytes + events * kAuditBytes)
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
	for (uint32_t i = 0; i < events; ++i)
	{
		const Audit event{reader.u64(), static_cast<RoleOperation>(reader.u8()), {reader.u32()}, {reader.u32()}};
		if (!event.revision || event.revision > next.revision || name(event.operation) == "invalid" ||
			(event.previousOwner.valid() && !isActor(event.previousOwner)) ||
			(!isActor(event.target) && (event.target.valid() || event.operation != RoleOperation::Bootstrap)) ||
			(i && event.revision != next.audit.back().revision + 1))
			return false;
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
		std::ranges::all_of(state.admins, [&](const auto &entry) { return matches(entry.first, entry.second); });
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
	if (next.revision == UINT64_MAX)
		return fail(error, "role revision space exhausted");
	++next.revision;
	if (next.audit.size() == kMaxAudit)
		next.audit.erase(next.audit.begin());
	next.audit.push_back({next.revision, operation, m_load == Load::Present ? m_state.owner : sim::PlayerId{}, target});
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
	for (const auto &[id, key] : next.admins)
	{
		writer.u32(id.value);
		writer.raw(key.data(), key.size());
	}
	for (const auto &event : next.audit)
	{
		writer.u64(event.revision);
		writer.u8(static_cast<uint8_t>(event.operation));
		writer.u32(event.previousOwner.value);
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
		return fail(error, "role revision recorded but publication could not finish; inspect offline status before retrying");
	m_state = std::move(next);
	m_load = Load::Present;
	return true;
}

bool WorldRoles::prepareStart(bool fresh, const std::optional<localcontrol::Bootstrap> &bootstrap, std::string &error)
{
	if (m_load == Load::Unowned)
	{
		if (!fresh || !bootstrap)
			return true;
		if (!localcontrol::validBootstrap(*bootstrap))
			return fail(error, "invalid local owner bootstrap");
		State next;
		next.ownerProfile = bootstrap->player;
		next.control = bootstrap->control;
		if (!commit(std::move(next), RoleOperation::Bootstrap, {}, error))
			return false;
	}
	if (m_load != Load::Present || !identitiesMatch(m_state))
		return fail(error, "role authority missing, corrupt or mismatched; inspect it with the offline roles command");
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
		(m_state.revision && localcontrol::matches(m_state.control, *request.newControl)))
		return fail(error, "operation requires a profile key and a distinct new local owner credential");
	State next;
	if (m_load == Load::Present)
	{
		next.revision = m_state.revision;
		next.audit = m_state.audit;
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
	}
	else
	{
		if (*request.newControl == localcontrol::ControlKey{} ||
			localcontrol::matches(next.control, *request.newControl) || m_identities.find(*request.newControl).valid())
			return fail(error, "transfer requires a distinct new local owner credential");
		next.owner = request.target;
		next.ownerProfile = *target;
		next.control = *request.newControl;
		next.admins.erase(request.target);
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
		output << "ROLE_AUDIT revision=" << event.revision << " operation=" << name(event.operation)
			   << " previous_owner_id=" << event.previousOwner.value << " target_id=" << event.target.value << '\n';
}
}
