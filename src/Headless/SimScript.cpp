// SimScript — bounded deterministic action-script parser (issue #210, Epic
// #204 Phase F). Pure parsing: no simulation state, fail-fast diagnostics.
#include "Headless/SimScript.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
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
	if (end == token.c_str() || *end != '\0')
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
			if (!parseInt(tokens[1], action.cell.x) || !parseInt(tokens[2], action.cell.y) ||
				!parseInt(tokens[3], action.cell.z))
			{
				error = failAt(lineNo, op + " expects integer cell coordinates <x> <y> <z>");
				return false;
			}
			if (action.cell.y < 0 || action.cell.y >= static_cast<int>(WORLD_HEIGHT))
			{
				error = failAt(lineNo, "cell y must be in [0, " + std::to_string(WORLD_HEIGHT - 1) + "]");
				return false;
			}
			if (place && !parseBlockName(tokens[4], action.block))
			{
				error = failAt(lineNo, "unknown block '" + tokens[4] + "'");
				return false;
			}
			action.op = place ? ScriptOp::Place : ScriptOp::Delete;
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
				error = failAt(lineNo, "expect expects 'voxel' or 'digest'");
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
