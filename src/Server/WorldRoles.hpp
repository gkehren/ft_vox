#pragma once

#include <Hosting/RoleCredentials.hpp>
#include <Server/WorldIdentities.hpp>

#include <iosfwd>

namespace server
{
enum class Role { Player, Admin, Owner };
enum class RoleOperation : uint8_t { Bootstrap = 1, Provision, GrantAdmin, RevokeAdmin, Transfer, Recover };

struct RoleRequest
{
	RoleOperation operation{};
	uint64_t worldId{}, revision{};
	sim::PlayerId target;
	std::optional<net::wire::PlayerKey> profile;
	std::optional<localcontrol::ControlKey> authorization, newControl;
};

// Only callers holding the world's native server lock may open this store.
// Requests are supplied by trusted local process entry points, never UDP.
class WorldRoles
{
public:
	WorldRoles(const std::filesystem::path &worldDir, WorldIdentities &identities);
	bool prepareStart(bool fresh, const std::optional<localcontrol::Bootstrap> &bootstrap, std::string &error);
	bool finishStart(std::string &error);
	bool apply(const RoleRequest &request, std::string &error);
	void printStatus(std::ostream &output) const;
	Role role(sim::PlayerId id) const;

private:
	enum class Load { Unowned, Present, Missing, Invalid, Stale, WrongWorld };
	struct Audit
	{
		uint64_t revision;
		RoleOperation operation;
		sim::PlayerId previousOwner, target;
	};
	struct State
	{
		uint64_t revision{};
		sim::PlayerId owner;
		net::wire::PlayerKey ownerProfile{};
		localcontrol::ControlKey control{};
		std::map<sim::PlayerId, net::wire::PlayerKey> admins;
		std::vector<Audit> audit;
	};

	Load load();
	Load read(const std::filesystem::path &path, State &state) const;
	bool decode(const std::vector<uint8_t> &bytes, uint64_t &worldId, State &next) const;
	bool identitiesMatch(const State &state) const;
	bool promotePending();
	bool commit(State next, RoleOperation operation, sim::PlayerId target, std::string &error);
	sim::PlayerId enroll(const net::wire::PlayerKey &profile);
	bool operatorChange(const RoleRequest &request, std::string &error);
	bool ownerChange(const RoleRequest &request, std::string &error);

	std::filesystem::path m_directory, m_path, m_pending;
	WorldIdentities &m_identities;
	State m_state;
	Load m_load;
};
}
