#include "graphics/shader/recompiler/frontend/translate/ProbeConfig.h"

#include "common/logging/log.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

namespace {

// `kind == Copy` reads a register from the middle field (<pc>:<src>:<dst>); every other kind takes
// <pc>:<dst>. Malformed entries stop the list rather than throwing - this is a debug path, and a
// half-parsed list is visible immediately in the probe's own output.
void ParseTapList(const char* cursor, ProbeTap::Kind kind, bool masked,
                  std::vector<ProbeTap>& taps) {
	while (cursor != nullptr && *cursor != '\0') {
		while (*cursor == ',' || *cursor == ' ' || *cursor == '\t') {
			++cursor;
		}
		if (*cursor == '\0') {
			break;
		}
		char*      end = nullptr;
		const auto pc  = std::strtoul(cursor, &end, 16);
		if (end == cursor || *end != ':') {
			break;
		}
		cursor         = end + 1;
		unsigned long src = 0;
		if (kind == ProbeTap::Kind::Copy) {
			src = std::strtoul(cursor, &end, 10);
			if (end == cursor || *end != ':' || src >= IR::NumVectorRegs) {
				break;
			}
			cursor = end + 1;
		}
		const auto dst = std::strtoul(cursor, &end, 10);
		if (end == cursor || dst >= IR::NumVectorRegs) {
			break;
		}
		cursor = end;
		taps.push_back({.pc     = static_cast<uint32_t>(pc),
		                .kind   = kind,
		                .src    = static_cast<uint32_t>(src),
		                .dst    = static_cast<uint32_t>(dst),
		                .masked = masked});
	}
}

void ParseVgprList(const char* cursor, std::array<int, 4>& list) {
	list = {-1, -1, -1, -1};
	for (uint32_t index = 0; cursor != nullptr && *cursor != '\0' && index < 4u; index++) {
		while (*cursor == ',' || *cursor == ' ' || *cursor == '\t') {
			++cursor;
		}
		char*      end   = nullptr;
		const auto value = std::strtol(cursor, &end, 10);
		if (end == cursor) {
			break;
		}
		list[index] = static_cast<int>(value);
		cursor      = end;
	}
}

uint64_t ParseHash(const char* text) {
	return text == nullptr ? 0ull : std::strtoull(text, nullptr, 16);
}

ProbeConfig ConfigFromEnvironment() {
	ProbeConfig config;
	config.hash     = ParseHash(std::getenv("KYTY_PROBE_HASH"));
	config.store_pc = static_cast<uint32_t>(ParseHash(std::getenv("KYTY_PROBE_PC")));
	ParseVgprList(std::getenv("KYTY_PROBE_VGPR"), config.vgpr);
	ParseTapList(std::getenv("KYTY_PROBE_TAP"), ProbeTap::Kind::Copy, false, config.taps);
	ParseTapList(std::getenv("KYTY_PROBE_MARK"), ProbeTap::Kind::Marker, false, config.taps);
	ParseTapList(std::getenv("KYTY_PROBE_MARKX"), ProbeTap::Kind::Marker, true, config.taps);
	ParseTapList(std::getenv("KYTY_PROBE_EXECLO"), ProbeTap::Kind::ExecLo, false, config.taps);
	ParseTapList(std::getenv("KYTY_PROBE_EXECZ"), ProbeTap::Kind::ExecZ, false, config.taps);
	return config;
}

// key=value lines; '#' starts a comment. Keys mirror the environment variables:
//   hash=e17349e0d437757b   pc=16fc   vgpr=90,91,92,93
//   mark=1140:91,148c:92    markx=113c:93   tap=1138:87:92
//   execlo=113c:92          execz=113c:90
// An absent key means "none", so the file is the whole configuration.
ProbeConfig ConfigFromText(const std::string& text) {
	ProbeConfig config;
	size_t      begin = 0;
	// Windows PowerShell's `Set-Content -Encoding utf8` writes a BOM, which would otherwise become
	// part of the first key and make the whole first line silently unparseable.
	if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
		begin = 3;
	}
	uint32_t unknown = 0;
	while (begin < text.size()) {
		auto end = text.find('\n', begin);
		if (end == std::string::npos) {
			end = text.size();
		}
		std::string line = text.substr(begin, end - begin);
		begin            = end + 1;
		if (const auto comment = line.find('#'); comment != std::string::npos) {
			line.erase(comment);
		}
		const auto separator = line.find('=');
		if (separator == std::string::npos) {
			continue;
		}
		auto trim = [](std::string value) {
			const auto first = value.find_first_not_of(" \t\r");
			const auto last  = value.find_last_not_of(" \t\r");
			return first == std::string::npos ? std::string {} : value.substr(first, last - first + 1);
		};
		const auto key   = trim(line.substr(0, separator));
		const auto value = trim(line.substr(separator + 1));
		if (key == "hash") {
			config.hash = ParseHash(value.c_str());
		} else if (key == "pc") {
			config.store_pc = static_cast<uint32_t>(ParseHash(value.c_str()));
		} else if (key == "vgpr") {
			ParseVgprList(value.c_str(), config.vgpr);
		} else if (key == "tap") {
			ParseTapList(value.c_str(), ProbeTap::Kind::Copy, false, config.taps);
		} else if (key == "mark") {
			ParseTapList(value.c_str(), ProbeTap::Kind::Marker, false, config.taps);
		} else if (key == "markx") {
			ParseTapList(value.c_str(), ProbeTap::Kind::Marker, true, config.taps);
		} else if (key == "execlo") {
			ParseTapList(value.c_str(), ProbeTap::Kind::ExecLo, false, config.taps);
		} else if (key == "execz") {
			ParseTapList(value.c_str(), ProbeTap::Kind::ExecZ, false, config.taps);
		} else {
			unknown++;
		}
	}
	if (unknown != 0) {
		// Silence here would look exactly like "the probe is configured and found nothing".
		LOGF("ProbeConfig: %u unrecognised key(s) ignored\n", unknown);
	}
	return config;
}

const char* ProbeFilePath() {
	static const char* path = [] {
		const char* env = std::getenv("KYTY_PROBE_FILE");
		return env != nullptr ? env : "D:/PS5/dumps/PROBE";
	}();
	return path;
}

std::mutex& ConfigLock() {
	static std::mutex lock;
	return lock;
}

std::shared_ptr<const ProbeConfig>& CurrentConfig() {
	static std::shared_ptr<const ProbeConfig> config =
	    std::make_shared<const ProbeConfig>(ConfigFromEnvironment());
	return config;
}

// Read without the lock: this is consulted once per shader cache lookup, i.e. per draw.
std::atomic<uint32_t> g_generation {0};

} // namespace

uint32_t ProbeConfig::ScratchVectorLimit() const {
	uint32_t limit = 0;
	for (const auto& tap: taps) {
		limit = std::max(limit, tap.dst + 1u);
	}
	return limit;
}

std::shared_ptr<const ProbeConfig> GetProbeConfig() {
	std::scoped_lock guard {ConfigLock()};
	return CurrentConfig();
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend

namespace Libs::Graphics::ShaderRecompiler {

uint32_t ProbeConfigGeneration() {
	return Frontend::g_generation.load(std::memory_order_relaxed);
}

void ReloadProbeConfig() {
	static std::filesystem::file_time_type stamp {};
	static bool                            existed = false;
	const auto*                            path    = Frontend::ProbeFilePath();
	std::error_code                        error;
	if (!std::filesystem::exists(path, error)) {
		// The file going away restores whatever the environment asked for, so a run can be put
		// back to its launch configuration by deleting it.
		if (!existed) {
			return;
		}
		existed = false;
		{
			std::scoped_lock guard {Frontend::ConfigLock()};
			Frontend::CurrentConfig() =
			    std::make_shared<const Frontend::ProbeConfig>(Frontend::ConfigFromEnvironment());
		}
		LOGF("ProbeConfig: %s removed, reverted to the environment\n", path);
		Frontend::g_generation.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const auto current = std::filesystem::last_write_time(path, error);
	if (error || (existed && current == stamp)) {
		return;
	}
	stamp   = current;
	existed = true;
	std::string   text;
	std::ifstream file {path};
	std::getline(file, text, '\0');
	auto config = Frontend::ConfigFromText(text);
	// Publish the generation inside the config so a dump can say which probe produced it.
	config.generation = Frontend::g_generation.load(std::memory_order_relaxed) + 1u;
	const auto taps   = config.taps.size();
	const auto hash   = config.hash;
	const auto pc     = config.store_pc;
	const auto vgpr   = config.vgpr;
	{
		std::scoped_lock guard {Frontend::ConfigLock()};
		Frontend::CurrentConfig() = std::make_shared<const Frontend::ProbeConfig>(std::move(config));
	}
	// Bump last: a shader translated between the swap and this store would be cached under the old
	// generation and reused with the new probe.
	Frontend::g_generation.fetch_add(1, std::memory_order_relaxed);
	LOGF("ProbeConfig: reloaded generation=%u hash=0x%016" PRIx64
	     " store_pc=0x%x vgpr=%d,%d,%d,%d taps=%zu\n",
	     Frontend::g_generation.load(std::memory_order_relaxed), hash, pc, vgpr[0], vgpr[1], vgpr[2],
	     vgpr[3], taps);
}

} // namespace Libs::Graphics::ShaderRecompiler
