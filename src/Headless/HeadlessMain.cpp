// ft_vox_headless — standalone CPU simulation runner (issue #210, Epic #204
// Phase F). A process with NO SDL3 window, Vulkan context, ImGui UI or GPU
// resource pool: it links only ft_vox_core (glm + FastNoise2 + Threads) and
// drives the SAME sim::GameSimulation facade the graphical Engine hosts.
//
//   ft_vox_headless --seed 42 --ticks 600 --script tests/fixtures/sim-actions.txt
//   ft_vox_headless --world demo --saves-root <temp-path> --ticks 600
//
// Prints one machine-readable SUMMARY line (key=value, or JSON with --json)
// and exits non-zero on any error (distinct codes per failure class).
// See docs/headless-runner.md for the full reference.

#include <Headless/HeadlessRunner.hpp>
#include <Headless/SimScript.hpp>

#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace
{

void printUsage(std::ostream &os)
{
	os <<
		"ft_vox_headless — standalone CPU simulation runner (no SDL3/Vulkan/ImGui)\n"
		"\n"
		"Usage: ft_vox_headless [options]\n"
		"  --seed N              world seed, 1..2147483647 (default 42; an existing save keeps its stored seed)\n"
		"  --ticks N             host frames to simulate at fixed dt=1/60 (default 600)\n"
		"  --script PATH         deterministic action script (see docs/headless-runner.md)\n"
		"  --world NAME          open/create a persistent world under --saves-root\n"
		"  --saves-root PATH     saves root directory (default 'saves'; use a temp dir in tests)\n"
		"  --interest X,Y,Z      pin the headless interest probe (default: follow the player)\n"
		"  --interest-radius N   streaming radius in blocks, 32..1024 (default 96)\n"
		"  --workers N           generation worker threads, 0..64 (default 0 = synchronous)\n"
		"  --digest-area X0,Y0,Z0,X1,Y1,Z1\n"
		"                        inclusive observed-area cell bounds for the state digest\n"
		"                        (default: 32x32 columns around the bootstrap chunk, full height)\n"
		"  --expect-digest HEX   fail (exit 5) unless the final digest equals HEX (16 hex digits)\n"
		"  --timeout SEC         wall-clock budget for the run, 1..3600 (default 120)\n"
		"  --json                emit the summary as a JSON object instead of key=value\n"
		"  --help                this text\n"
		"\n"
		"Exit codes: 0 ok, 2 usage, 3 script, 4 world/save I/O, 5 assertion, 6 timeout.\n";
}

bool parseIntArg(const std::string &text, long long &out)
{
	if (text.empty())
		return false;
	const auto *begin = text.data();
	const auto *end = text.data() + text.size();
	const auto result = std::from_chars(begin, end, out);
	return result.ec == std::errc{} && result.ptr == end;
}

bool parseDoubleArg(const std::string &text, double &out)
{
	if (text.empty())
		return false;
	char *end = nullptr;
	out = std::strtod(text.c_str(), &end);
	// Reject NaN/inf: a non-finite --timeout would silently disable the
	// wall-clock safety net (NaN comparisons are always false), and non-finite
	// interest/teleport coordinates are never meaningful (issue #210 review).
	return end != text.c_str() && *end == '\0' && std::isfinite(out);
}

/// Split "a,b,c" into exactly `count` finite numeric components.
bool parseList(const std::string &text, size_t count, std::vector<double> &out)
{
	out.clear();
	size_t start = 0;
	while (start <= text.size())
	{
		const size_t comma = text.find(',', start);
		const std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		double value = 0.0;
		if (!parseDoubleArg(part, value))
			return false;
		out.push_back(value);
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return out.size() == count;
}

/// Split "a,b,c" into exactly `count` INTEGER components (cell coordinates).
/// Rejects fractional/out-of-range values cleanly instead of truncating or
/// invoking UB on a double->int cast (issue #210 review).
bool parseIntList(const std::string &text, size_t count, std::vector<int> &out)
{
	out.clear();
	size_t start = 0;
	while (start <= text.size())
	{
		const size_t comma = text.find(',', start);
		const std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
		long long value = 0;
		if (!parseIntArg(part, value) || value < -2147483647LL || value > 2147483647LL)
			return false;
		out.push_back(static_cast<int>(value));
		if (comma == std::string::npos)
			break;
		start = comma + 1;
	}
	return out.size() == count;
}

std::string formatDouble(double value)
{
	std::ostringstream os;
	os << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
	return os.str();
}

std::string formatVec3(const glm::dvec3 &v)
{
	return "(" + formatDouble(v.x) + "," + formatDouble(v.y) + "," + formatDouble(v.z) + ")";
}

std::string jsonEscape(const std::string &in)
{
	// Full JSON string escaping (review §4.10): control bytes (< 0x20)
	// must be \u-escaped — an I/O error message containing a newline used
	// to produce invalid JSON for strict parsers.
	std::string out;
	for (const unsigned char c : in)
	{
		switch (c)
		{
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		default:
			if (c < 0x20)
			{
				constexpr char hex[] = "0123456789abcdef";
				out += "\\u00";
				out += hex[c >> 4];
				out += hex[c & 0xF];
			}
			else
			{
				out.push_back(static_cast<char>(c));
			}
		}
	}
	return out;
}

void printSummary(const headless::RunOutcome &out, bool json)
{
	const bool ok = out.code == headless::ExitCode::Ok;
	if (json)
	{
		std::cout << "{\"status\":\"" << (ok ? "ok" : "error") << "\""
				  << ",\"exit_code\":" << static_cast<int>(out.code)
				  << ",\"seed\":" << out.seed
				  << ",\"generator\":" << out.generatorVersion
				  << ",\"ticks\":" << out.ticksRun
				  << ",\"workers\":" << out.workers
				  << ",\"interest_radius\":" << out.interestRadius
				  << ",\"chunks\":" << out.chunkCount
				  << ",\"edits\":" << out.committedEdits
				  << ",\"refused\":" << out.refusedEdits
				  << ",\"rejected\":" << out.rejectedEdits
				  << ",\"edit_revision\":" << out.editRevision
				  << ",\"mobs\":" << out.mobCount
				  << ",\"items\":" << out.itemCount
				  << ",\"inventory\":" << out.inventoryTotal
				  << ",\"item_high_water\":" << out.itemIdHighWater
				  << ",\"player\":{\"x\":" << formatDouble(out.playerPos.x)
				  << ",\"y\":" << formatDouble(out.playerPos.y)
				  << ",\"z\":" << formatDouble(out.playerPos.z) << "}"
				  << ",\"area\":{\"min\":[" << out.area.min.x << "," << out.area.min.y << "," << out.area.min.z
				  << "],\"max\":[" << out.area.max.x << "," << out.area.max.y << "," << out.area.max.z << "]}"
				  << ",\"digest\":\"" << out.digest << "\""
				  << ",\"save\":\"" << out.saveStatus << "\"";
		if (!ok)
			std::cout << ",\"error\":\"" << jsonEscape(out.message) << "\"";
		std::cout << "}\n";
		return;
	}
	std::cout << "SUMMARY status=" << (ok ? "ok" : "error")
			  << " exit_code=" << static_cast<int>(out.code)
			  << " seed=" << out.seed
			  << " generator=" << out.generatorVersion
			  << " ticks=" << out.ticksRun
			  << " workers=" << out.workers
			  << " interest_radius=" << out.interestRadius
			  << " chunks=" << out.chunkCount
			  << " edits=" << out.committedEdits
			  << " refused=" << out.refusedEdits
			  << " rejected=" << out.rejectedEdits
			  << " edit_revision=" << out.editRevision
			  << " mobs=" << out.mobCount
			  << " items=" << out.itemCount
			  << " inventory=" << out.inventoryTotal
			  << " item_high_water=" << out.itemIdHighWater
			  << " player=" << formatVec3(out.playerPos)
			  << " area=" << formatVec3(glm::dvec3(out.area.min)) << "-" << formatVec3(glm::dvec3(out.area.max))
			  << " digest=" << (out.digest.empty() ? "-" : out.digest)
			  << " save=" << out.saveStatus
			  << "\n";
}

} // namespace

int main(int argc, char **argv)
{
	headless::RunnerOptions options;
	std::string error;

	const auto needValue = [&](int &i) -> const char * {
		if (i + 1 >= argc)
		{
			error = std::string("missing value for ") + argv[i];
			return nullptr;
		}
		return argv[++i];
	};

	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		const auto intRange = [&](const char *value, long long lo, long long hi, auto &field) {
			long long parsed = 0;
			if (!parseIntArg(value, parsed) || parsed < lo || parsed > hi)
			{
				error = arg + " expects an integer in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]";
				return false;
			}
			field = static_cast<std::remove_reference_t<decltype(field)>>(parsed);
			return true;
		};

		if (arg == "--help" || arg == "-h")
		{
			printUsage(std::cout);
			return 0;
		}
		else if (arg == "--seed")
		{
			const char *value = needValue(i);
			if (!value || !intRange(value, 1, 2147483647, options.seed))
				break;
		}
		else if (arg == "--ticks")
		{
			const char *value = needValue(i);
			if (!value || !intRange(value, 1, 1000000, options.ticks))
				break;
		}
		else if (arg == "--script")
		{
			const char *value = needValue(i);
			if (!value)
				break;
			options.scriptPath = value;
		}
		else if (arg == "--world")
		{
			const char *value = needValue(i);
			if (!value)
				break;
			options.worldName = value;
		}
		else if (arg == "--saves-root")
		{
			const char *value = needValue(i);
			if (!value)
				break;
			options.savesRoot = value;
		}
		else if (arg == "--interest")
		{
			const char *value = needValue(i);
			if (!value)
				break;
			std::vector<double> xyz;
			if (!parseList(value, 3, xyz))
			{
				error = "--interest expects X,Y,Z";
				break;
			}
			options.fixedInterest = true;
			options.interest = glm::dvec3(xyz[0], xyz[1], xyz[2]);
		}
		else if (arg == "--interest-radius")
		{
			const char *value = needValue(i);
			if (!value || !intRange(value, 32, 1024, options.interestRadius))
				break;
		}
		else if (arg == "--workers")
		{
			const char *value = needValue(i);
			if (!value || !intRange(value, 0, 64, options.workers))
				break;
		}
		else if (arg == "--digest-area")
		{
			const char *value = needValue(i);
			if (!value)
				break;
			std::vector<int> bounds;
			if (!parseIntList(value, 6, bounds))
			{
				error = "--digest-area expects six integer cell coordinates X0,Y0,Z0,X1,Y1,Z1";
				break;
			}
			options.hasDigestArea = true;
			options.digestArea.min = glm::ivec3(bounds[0], bounds[1], bounds[2]);
			options.digestArea.max = glm::ivec3(bounds[3], bounds[4], bounds[5]);
			std::string areaError;
			if (!headless::validateDigestArea(options.digestArea, areaError))
			{
				error = "--digest-area: " + areaError;
				break;
			}
		}
		else if (arg == "--expect-digest")
		{
			const char *value = needValue(i);
			if (!value)
				break;
			options.expectedDigest = value;
			// Accept both 0x and 0X prefixes (review §4.10): SimScript's
			// expect-digest grammar accepts both; the CLI must not diverge.
			if (options.expectedDigest.rfind("0x", 0) == 0 || options.expectedDigest.rfind("0X", 0) == 0)
				options.expectedDigest.erase(0, 2);
			if (options.expectedDigest.size() != 16)
			{
				error = "--expect-digest expects 16 hex digits";
				break;
			}
			for (char &c : options.expectedDigest)
			{
				if (!std::isxdigit(static_cast<unsigned char>(c)))
				{
					error = "--expect-digest expects 16 hex digits";
					break;
				}
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			}
			if (!error.empty())
				break;
		}
		else if (arg == "--timeout")
		{
			const char *value = needValue(i);
			if (!value)
				break;
			double parsed = 0.0;
			if (!parseDoubleArg(value, parsed) || parsed < 1.0 || parsed > 3600.0)
			{
				error = "--timeout expects seconds in [1, 3600]";
				break;
			}
			options.timeoutSec = parsed;
		}
		else if (arg == "--json")
		{
			options.json = true;
		}
		else
		{
			error = "unknown option '" + arg + "'";
			break;
		}
	}

	if (!error.empty())
	{
		std::cerr << "ERROR: " << error << "\n\n";
		printUsage(std::cerr);
		return static_cast<int>(headless::ExitCode::Usage);
	}

	const headless::RunOutcome outcome = headless::runHeadless(options);
	if (outcome.code != headless::ExitCode::Ok)
		std::cerr << "ERROR: " << outcome.message << "\n";
	printSummary(outcome, options.json);
	return static_cast<int>(outcome.code);
}
