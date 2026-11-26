#pragma once

#include <string>
#include <vector>

namespace gameui
{
// Native screens submit these intents. Engine validates them against live ownership.
enum class SessionAction
{
	List, Create, Open, Save, Leave, Quit, Retry, Cancel,
	ForceStop, ConfirmFailedQuit
};

struct SessionRequest
{
	SessionAction action{};
	std::string name, seed;
};

struct WorldCatalog
{
	std::vector<std::string> names;
	std::string notice;
};
} // namespace gameui
