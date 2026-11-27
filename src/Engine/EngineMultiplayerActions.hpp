#pragma once

#include <Engine/MultiplayerJoin.hpp>
#include <Hosting/ServerSupervisor.hpp>

#include <functional>
#include <string>
#include <string_view>

// Short-lived Engine adapters used to validate and dispatch native intents.
struct MultiplayerActions
{
	std::function<void()> joinServer;
	std::function<void()> leaveServer;
	std::function<void()> returnToSolo;
	bool joinPreferencesLoaded{};
	std::function<void(client::JoinRequest)> connectServer;
	std::function<void(std::string_view)> selectJoinProfile, createJoinProfile, toggleFavorite;
	bool switchingSession{};
	std::function<void(hosting::Options, bool, std::string_view)> startHostedServer;
	std::function<void()> stopHostedServer, forceStopHostedServer;
	std::function<void(std::string_view)> joinHostedServer;
	std::function<std::string(uint64_t, localcontrol::Operation, sim::PlayerId, std::string_view)> administerHostedServer;
};
