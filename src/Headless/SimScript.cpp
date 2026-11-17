// SimScript — bounded deterministic action-script parser (issue #210, Epic
// #204 Phase F; gameplay grammar issue #230, Epic #223 Phase G). Pure parsing:
// no simulation state, fail-fast diagnostics.
#include "Headless/SimScript.hpp"

#include <Items/ItemEntitySystem.hpp>
#include <Items/ItemRegistry.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <sstream>

namespace headless
{
namespace
{

std::string upperUnderscore(const std::string &in)
{
	std::string out;
	out.reserve(in.size());
	for (const char c : in)
	{
		if (c == ' ' || c == '-')
			out.push_back('_');
		else
			out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
	}
	return out;
}

bool parseInt(const std::string &token, int &out)
{
	if (token.empty())
		return false;
	char *end = nullptr;
	const long value = std::strtol(token.c_str(), &end, 10);
	if (end == token.c_str() || *end != '\0')
		return false;
	if (value < -2147483647L || value > 2147483647L)
		return false;
	out = static_cast<int>(value);
	return true;
}

bool parseDouble(const std::string &token, double &out)
{
	if (token.empty())
		return false;
	char *end = nullptr;
	const double value = std::strtod(token.c_str(), &end);
	if (end == token.c_str() || *end != '\0' || !std::isfinite(value))
		return false;
	out = value;
	return true;
}

bool parseOnOff(const std::string &token, bool &out)
{
	const std::string t = upperUnderscore(token);
	if (t == "ON" || t == "1" || t == "TRUE")
	{
		out = true;
		return true;
	}
	if (t == "OFF" || t == "0" || t == "FALSE")
	{
		out = false;
		return true;
	}
	return false;
}

std::vector<std::string> tokenize(const std::string &line)
{
	std::vector<std::string> tokens;
	std::istringstream stream(line);
	std::string token;
	while (stream >> token)
		tokens.push_back(token);
	return tokens;
}

std::string failAt(int line, const std::string &message)
{
	return "script line " + std::to_string(line) + ": " + message;
}

} // namespace

bool parseBlockName(const std::string &token, TextureType &out)
{
	const std::string normalized = upperUnderscore(token);
	if (normalized == "AIR")
	{
		out = AIR;
		return true;
	}
	// Numeric ordinal (append-only enum; AIR sits after COUNT).
	int ordinal = 0;
	if (parseInt(token, ordinal) && ordinal >= 0 && ordinal <= static_cast<int>(AIR))
	{
		out = static_cast<TextureType>(ordinal);
		return true;
	}
	for (int i = 0; i < static_cast<int>(COUNT); ++i)
	{
		if (upperUnderscore(std::string(textureTypeString[static_cast<size_t>(i)])) == normalized)
		{
			out = static_cast<TextureType>(i);
			return true;
		}
	}
	return false;
}

bool parseSimScript(const std::filesystem::path &path, SimScript &out, std::string &error)
{
	std::ifstream file(path);
	if (!file.is_open())
	{
		error = "cannot open script '" + path.string() + "'";
		return false;
	}

	out = SimScript{};
	std::string line;
	int lineNo = 0;
	while (std::getline(file, line))
	{
		++lineNo;
		const auto comment = line.find('#');
		if (comment != std::string::npos)
			line.erase(comment);
		const std::vector<std::string> tokens = tokenize(line);
		if (tokens.empty())
			continue;
		if (static_cast<int>(out.actions.size()) >= kMaxScriptActions)
		{
			error = failAt(lineNo, "too many actions (limit " + std::to_string(kMaxScriptActions) + ")");
			return false;
		}

		ScriptAction action;
		action.line = lineNo;
		const std::string op = upperUnderscore(tokens[0]);
		const size_t argc = tokens.size();
		const auto needArgs = [&](size_t n) {
			if (argc != n)
				error = failAt(lineNo, "'" + tokens[0] + "' expects " + std::to_string(n - 1) + " argument(s)");
			return argc == n;
		};
		// Shared cell-coordinate parse (place/delete/break/placeSelected):
		// integers, y inside the world column (issue #230).
		const auto parseCell = [&](glm::ivec3 &cell) {
			if (!parseInt(tokens[1], cell.x) || !parseInt(tokens[2], cell.y) ||
				!parseInt(tokens[3], cell.z))
			{
				error = failAt(lineNo, op + " expects integer cell coordinates <x> <y> <z>");
				return false;
			}
			if (cell.y < 0 || cell.y >= static_cast<int>(WORLD_HEIGHT))
			{
				error = failAt(lineNo, "cell y must be in [0, " + std::to_string(WORLD_HEIGHT - 1) + "]");
				return false;
			}
			return true;
		};
		// ItemStack counts are uint16_t (issue #230): bound at parse time.
		const auto parseCount = [&](const std::string &token, int &count) {
			if (!parseInt(token, count) || count < 0 || count > 65535)
			{
				error = failAt(lineNo, "count must be an integer in [0, 65535]");
				return false;
			}
			return true;
		};

		if (op == "WAIT")
		{
			if (!needArgs(2))
				return false;
			if (!parseInt(tokens[1], action.frames) || action.frames < 1 || action.frames > kMaxWaitFrames)
			{
				error = failAt(lineNo, "wait frames must be an integer in [1, " + std::to_string(kMaxWaitFrames) + "]");
				return false;
			}
			action.op = ScriptOp::Wait;
			out.totalWaitFrames += action.frames;
		}
		else if (op == "MOVE")
		{
			if (!needArgs(3))
				return false;
			if (!parseDouble(tokens[1], action.move.x) || !parseDouble(tokens[2], action.move.y))
			{
				error = failAt(lineNo, "move expects two numbers <x> <z>");
				return false;
			}
			// Input contract enforced at parse (review §4.10): PlayerInput.move
			// is a direction of length <= 1. MOVE 1e9 1e9 used to be accepted
			// and silently clamped by the controller; the grammar now refuses
			// it with a numbered diagnostic like every other invalid op.
			const double len = std::sqrt(action.move.x * action.move.x + action.move.y * action.move.y);
			if (!std::isfinite(len) || len > 1.0 + 1e-9)
			{
				error = failAt(lineNo, "move vector length must be <= 1");
				return false;
			}
			action.op = ScriptOp::Move;
		}
		else if (op == "SPRINT")
		{
			if (!needArgs(2))
				return false;
			if (!parseOnOff(tokens[1], action.enabled))
			{
				error = failAt(lineNo, "sprint expects on|off");
				return false;
			}
			action.op = ScriptOp::Sprint;
		}
		else if (op == "JUMP")
		{
			if (!needArgs(1))
				return false;
			action.op = ScriptOp::Jump;
		}
		else if (op == "SWIM")
		{
			if (!needArgs(2))
				return false;
			const std::string dir = upperUnderscore(tokens[1]);
			if (dir == "UP")
				action.swimDirection = 1;
			else if (dir == "DOWN")
				action.swimDirection = -1;
			else if (dir == "OFF")
				action.swimDirection = 0;
			else
			{
				error = failAt(lineNo, "swim expects up|down|off");
				return false;
			}
			action.op = ScriptOp::Swim;
		}
		else if (op == "FLIGHT")
		{
			if (!needArgs(2))
				return false;
			if (!parseOnOff(tokens[1], action.enabled))
			{
				error = failAt(lineNo, "flight expects on|off");
				return false;
			}
			action.op = ScriptOp::Flight;
		}
		else if (op == "TELEPORT")
		{
			if (!needArgs(4))
				return false;
			if (!parseDouble(tokens[1], action.position.x) || !parseDouble(tokens[2], action.position.y) ||
				!parseDouble(tokens[3], action.position.z))
			{
				error = failAt(lineNo, "teleport expects three numbers <x> <y> <z>");
				return false;
			}
			action.op = ScriptOp::Teleport;
		}
		else if (op == "PLACE" || op == "DELETE")
		{
			const bool place = op == "PLACE";
			if (!needArgs(place ? 5 : 4))
				return false;
			if (!parseCell(action.cell))
				return false;
			if (place && !parseBlockName(tokens[4], action.block))
			{
				error = failAt(lineNo, "unknown block '" + tokens[4] + "'");
				return false;
			}
			action.op = place ? ScriptOp::Place : ScriptOp::Delete;
		}
		else if (op == "GIVE")
		{
			if (!needArgs(3))
				return false;
			const std::optional<items::ItemId> item = items::parseItemId(tokens[1]);
			if (!item)
			{
				error = failAt(lineNo, "unknown item '" + tokens[1] + "'");
				return false;
			}
			if (!parseCount(tokens[2], action.count))
				return false;
			action.item = *item;
			action.op = ScriptOp::Give;
		}
		else if (op == "SELECT")
		{
			if (!needArgs(2))
				return false;
			if (!parseInt(tokens[1], action.slot) || action.slot < 0 || action.slot >= items::kHotbarSize)
			{
				error = failAt(lineNo, "select slot must be an integer in [0, " +
										   std::to_string(items::kHotbarSize - 1) + "]");
				return false;
			}
			action.op = ScriptOp::Select;
		}
		else if (op == "DROP")
		{
			if (!needArgs(2))
				return false;
			if (!parseCount(tokens[1], action.count))
				return false;
			action.op = ScriptOp::Drop;
		}
		else if (op == "BREAK" || op == "PLACESELECTED")
		{
			if (!needArgs(4))
				return false;
			if (!parseCell(action.cell))
				return false;
			action.op = op == "BREAK" ? ScriptOp::Break : ScriptOp::PlaceSelected;
		}
		else if (op == "INTEREST")
		{
			if (argc == 2 && upperUnderscore(tokens[1]) == "FOLLOW")
			{
				action.op = ScriptOp::InterestFollow;
			}
			else
			{
				if (!needArgs(4))
					return false;
				if (!parseDouble(tokens[1], action.position.x) || !parseDouble(tokens[2], action.position.y) ||
					!parseDouble(tokens[3], action.position.z))
				{
					error = failAt(lineNo, "interest expects <x> <y> <z> or 'follow'");
					return false;
				}
				action.op = ScriptOp::Interest;
			}
		}
		else if (op == "SAVE")
		{
			if (!needArgs(1))
				return false;
			action.op = ScriptOp::Save;
		}
		else if (op == "EXPECT")
		{
			if (argc < 2)
			{
				error = failAt(lineNo, "expect expects 'voxel', 'inventory', 'items' or 'digest'");
				return false;
			}
			const std::string what = upperUnderscore(tokens[1]);
			if (what == "VOXEL")
			{
				if (!needArgs(6))
					return false;
				if (!parseInt(tokens[2], action.cell.x) || !parseInt(tokens[3], action.cell.y) ||
					!parseInt(tokens[4], action.cell.z))
				{
					error = failAt(lineNo, "expect voxel expects integer cell coordinates");
					return false;
				}
				if (action.cell.y < 0 || action.cell.y >= static_cast<int>(WORLD_HEIGHT))
				{
					error = failAt(lineNo, "cell y must be in [0, " + std::to_string(WORLD_HEIGHT - 1) + "]");
					return false;
				}
				if (upperUnderscore(tokens[5]) == "UNKNOWN")
				{
					action.expectedVoxel = kExpectUnknown;
				}
				else
				{
					TextureType block = AIR;
					if (!parseBlockName(tokens[5], block))
					{
						error = failAt(lineNo, "unknown block '" + tokens[5] + "'");
						return false;
					}
					action.expectedVoxel = static_cast<int>(block);
				}
				action.op = ScriptOp::ExpectVoxel;
			}
			else if (what == "INVENTORY")
			{
				if (!needArgs(5))
					return false;
				if (!parseInt(tokens[2], action.slot) || action.slot < 0 || action.slot >= items::kInventorySize)
				{
					error = failAt(lineNo, "expect inventory slot must be an integer in [0, " +
											   std::to_string(items::kInventorySize - 1) + "]");
					return false;
				}
				if (upperUnderscore(tokens[3]) == "EMPTY")
				{
					// The canonical empty is {None, 0}: 'empty' pins count 0.
					action.expectEmpty = true;
					if (!parseInt(tokens[4], action.count) || action.count != 0)
					{
						error = failAt(lineNo, "expect inventory 'empty' requires count 0");
						return false;
					}
				}
				else
				{
					const std::optional<items::ItemId> item = items::parseItemId(tokens[3]);
					if (!item)
					{
						error = failAt(lineNo, "unknown item '" + tokens[3] + "'");
						return false;
					}
					action.item = *item;
					if (!parseCount(tokens[4], action.count))
						return false;
				}
				action.op = ScriptOp::ExpectInventory;
			}
			else if (what == "ITEMS")
			{
				if (!needArgs(3))
					return false;
				if (!parseInt(tokens[2], action.expectedItems) || action.expectedItems < 0 ||
					action.expectedItems > static_cast<int>(items::ItemEntitySettings::capacity))
				{
					error = failAt(lineNo, "expect items count must be an integer in [0, " +
											   std::to_string(items::ItemEntitySettings::capacity) + "]");
					return false;
				}
				action.op = ScriptOp::ExpectItems;
			}
			else if (what == "DIGEST")
			{
				if (!needArgs(3))
					return false;
				std::string hex = tokens[2];
				if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0)
					hex.erase(0, 2);
				if (hex.size() != 16 ||
					!std::all_of(hex.begin(), hex.end(), [](char c) {
						return std::isxdigit(static_cast<unsigned char>(c)) != 0;
					}))
				{
					error = failAt(lineNo, "expect digest expects 16 hex digits");
					return false;
				}
				std::transform(hex.begin(), hex.end(), hex.begin(),
							   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
				action.expectedDigest = hex;
				action.op = ScriptOp::ExpectDigest;
			}
			else
			{
				error = failAt(lineNo, "unknown expectation '" + tokens[1] + "'");
				return false;
			}
		}
		else
		{
			error = failAt(lineNo, "unknown action '" + tokens[0] + "'");
			return false;
		}
		out.actions.push_back(std::move(action));
	}
	return true;
}

} // namespace headless
