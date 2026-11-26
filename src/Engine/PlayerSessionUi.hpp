#pragma once

#include <string>
#include <vector>

namespace gameui
{
enum class SessionAction
{
	List, Create, Open, Save, Leave, Quit, Retry, Cancel, Multiplayer, Settings,
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
