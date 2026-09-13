#include "graphics/shader/recompiler/frontend/translate/ProbeConfig.h"

#include "common/logging/log.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
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

uint32_t ParseU32(const char* text) {
	return text == nullptr ? 0u : static_cast<uint32_t>(std::strtoul(text, nullptr, 0));
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
	config.loop_header     = ParseU32(std::getenv("KYTY_PROBE_LOOP_HEADER"));
	config.loop_iterations = ParseU32(std::getenv("KYTY_PROBE_LOOP_ITERATIONS"));
	config.gds_limit_cap   = ParseU32(std::getenv("KYTY_PROBE_GDS_LIMIT"));
	config.group_cap       = ParseU32(std::getenv("KYTY_PROBE_GROUP_CAP"));
	if (const auto count = ParseU32(std::getenv("KYTY_PROBE_EXECUTE_COUNT")); count != 0) {
		config.execute_count = count;
	}
	config.sync_once       = ParseU32(std::getenv("KYTY_PROBE_SYNC")) != 0;
	config.execute_once    = ParseU32(std::getenv("KYTY_PROBE_EXECUTE_ONCE")) != 0;
	return config;
}

// key=value lines; '#' starts a comment. Keys mirror the environment variables:
//   hash=e17349e0d437757b   pc=16fc   vgpr=90,91,92,93
//   mark=1140:91,148c:92    markx=113c:93   tap=1138:87:92
//   execlo=113c:92          execz=113c:90
//   loop_header=50          loop_iterations=1
//   gds_limit=1             group_cap=1       sync=1       execute_once=1 execute_count=2
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
		} else if (key == "loop_header") {
			config.loop_header = ParseU32(value.c_str());
		} else if (key == "loop_iterations") {
			config.loop_iterations = ParseU32(value.c_str());
		} else if (key == "gds_limit") {
			config.gds_limit_cap = ParseU32(value.c_str());
		} else if (key == "group_cap") {
			config.group_cap = ParseU32(value.c_str());
		} else if (key == "execute_count") {
			if (const auto count = ParseU32(value.c_str()); count != 0) {
				config.execute_count = count;
			}
		} else if (key == "sync") {
			config.sync_once = ParseU32(value.c_str()) != 0;
		} else if (key == "execute_once") {
			config.execute_once = ParseU32(value.c_str()) != 0;
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

const char* GetProbeConfigFilePath() { return ProbeFilePath(); }

std::string GetProbeConfigText() {
	std::ifstream file {ProbeFilePath(), std::ios::binary};
	if (file) {
		std::string text;
		std::getline(file, text, '\0');
		return text;
	}

	const auto config = GetProbeConfig();
	std::string text = fmt::format("hash={:016x}\npc={:x}\nvgpr={},{},{},{}\n", config->hash,
	                               config->store_pc, config->vgpr[0], config->vgpr[1],
	                               config->vgpr[2], config->vgpr[3]);
	text += fmt::format("loop_header={}\nloop_iterations={}\ngds_limit={}\ngroup_cap={}\n"
	                    "sync={}\nexecute_once={}\nexecute_count={}\n",
	                    config->loop_header, config->loop_iterations, config->gds_limit_cap,
	                    config->group_cap, config->sync_once ? 1 : 0,
	                    config->execute_once ? 1 : 0, config->execute_count);
	auto append_taps = [&](const char* key, ProbeTap::Kind kind, bool masked) {
		bool first = true;
		for (const auto& tap: config->taps) {
			if (tap.kind != kind || tap.masked != masked) continue;
			text += first ? fmt::format("{}=", key) : ",";
			text += kind == ProbeTap::Kind::Copy ? fmt::format("{:x}:{}:{}", tap.pc, tap.src, tap.dst)
			                                    : fmt::format("{:x}:{}", tap.pc, tap.dst);
			first = false;
		}
		if (!first) text += '\n';
	};
	append_taps("tap", ProbeTap::Kind::Copy, false);
	append_taps("mark", ProbeTap::Kind::Marker, false);
	append_taps("markx", ProbeTap::Kind::Marker, true);
	append_taps("execlo", ProbeTap::Kind::ExecLo, false);
	append_taps("execz", ProbeTap::Kind::ExecZ, false);
	return text;
}

bool WriteProbeConfigText(std::string_view text, std::string* error) {
	std::error_code ec;
	const std::filesystem::path path {ProbeFilePath()};
	if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
	if (ec) {
		if (error != nullptr) *error = fmt::format("could not create PROBE directory: {}", ec.message());
		return false;
	}
	std::ofstream file {path, std::ios::binary | std::ios::trunc};
	if (!file) {
		if (error != nullptr) *error = fmt::format("could not open {}", path.string());
		return false;
	}
	file.write(text.data(), static_cast<std::streamsize>(text.size()));
	file.close();
	if (!file.good()) {
		if (error != nullptr) *error = fmt::format("could not finish writing {}", path.string());
		return false;
	}
	return true;
}

bool RemoveProbeConfigFile(std::string* error) {
	std::error_code ec;
	std::filesystem::remove(ProbeFilePath(), ec);
	if (ec) {
		if (error != nullptr) *error = fmt::format("could not remove PROBE: {}", ec.message());
		return false;
	}
	return true;
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
	const auto taps       = config.taps.size();
	const auto hash       = config.hash;
	const auto pc         = config.store_pc;
	const auto vgpr       = config.vgpr;
	const auto loop       = config.loop_header;
	const auto iterations = config.loop_iterations;
	const auto execute    = config.execute_once;
	const auto count      = config.execute_count;
	const auto limit      = config.gds_limit_cap;
	const auto groups     = config.group_cap;
	const auto sync       = config.sync_once;
	{
		std::scoped_lock guard {Frontend::ConfigLock()};
		Frontend::CurrentConfig() = std::make_shared<const Frontend::ProbeConfig>(std::move(config));
	}
	// Bump last: a shader translated between the swap and this store would be cached under the old
	// generation and reused with the new probe.
	Frontend::g_generation.fetch_add(1, std::memory_order_relaxed);
	LOGF("ProbeConfig: reloaded generation=%u hash=0x%016" PRIx64
	     " store_pc=0x%x vgpr=%d,%d,%d,%d taps=%zu loop=%u/%u execute_once=%u/%u "
	     "gds_limit=%u group_cap=%u sync=%u\n",
	     Frontend::g_generation.load(std::memory_order_relaxed), hash, pc, vgpr[0], vgpr[1], vgpr[2],
	     vgpr[3], taps, loop, iterations, execute ? 1u : 0u, count, limit, groups,
	     sync ? 1u : 0u);
}

} // namespace Libs::Graphics::ShaderRecompiler
