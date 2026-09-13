#pragma once

#include "graphics/shader/recompiler/ir/Reg.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

// Diagnostic shader probe, default inert. See Dispatch.cpp for what each kind emits.
//
// The configuration is RELOADABLE AT RUNTIME from D:/PS5/dumps/PROBE (override with
// KYTY_PROBE_FILE), because the cost that dominates this investigation is not compiling the
// emulator - it is that changing what is measured used to require a restart, and a restart costs a
// full manual re-navigation into a fight. The file is seeded from the KYTY_PROBE_* environment
// variables at startup, and when it changes it REPLACES the configuration wholesale rather than
// merging, so what is in the file is what is running.
//
// Changing the configuration bumps `generation`, which is part of the pipeline cache's program key
// - so the next dispatch of an already-translated shader misses the cache and is re-translated with
// the new probe. Nothing is erased: the stale permutation keeps its shader module and its pipeline
// (pipelines are keyed by the shader id, and ids are never reused), so no pointer handed out
// earlier is invalidated. A re-aim leaks one module and one pipeline, which is the right trade for
// a debug path that saves a navigation.
struct ProbeTap {
	enum class Kind : uint8_t {
		Copy,    // v<dst> = v<src>
		Marker,  // v<dst> = 1.0
		ExecLo,  // v<dst> = the wave-wide EXEC mask word
		ExecZ,   // v<dst> = MaskIsZero(exec) ? 1.0 : 0.0
	};
	uint32_t pc     = 0;
	Kind     kind   = Kind::Marker;
	uint32_t src    = 0;
	uint32_t dst    = 0;
	bool     masked = false;  // honour the per-lane EXEC bit instead of writing unconditionally
};

struct ProbeConfig {
	uint64_t              hash     = 0;  // 0 = every shader (dangerous: replaces every image store)
	uint32_t              store_pc = 0;  // 0 = every image store
	std::array<int, 4>    vgpr {-1, -1, -1, -1};
	std::vector<ProbeTap> taps;
	// Hash-scoped one-shot execution controls for a compute shader that is normally skipped.
	// They share the PROBE file so changing a loop cap also bumps the shader-cache generation.
	uint32_t loop_header     = 0;
	uint32_t loop_iterations = 0;
	uint32_t gds_limit_cap   = 0;
	uint32_t group_cap       = 0;
	uint32_t execute_count   = 1;
	bool     sync_once       = false;
	bool     execute_once    = false;
	uint32_t generation      = 0;

	[[nodiscard]] bool AppliesTo(uint64_t shader_hash) const {
		return hash == 0 || hash == shader_hash;
	}
	// Highest scratch register the taps write, so the translation-wide vector limit can cover
	// registers the shader itself never allocates. 0 when nothing is configured.
	[[nodiscard]] uint32_t ScratchVectorLimit() const;
};

// A snapshot, stable for as long as the caller holds it. Take it once per Translator rather than
// per instruction; a reload swaps the pointer and leaves existing readers on the old snapshot.
std::shared_ptr<const ProbeConfig> GetProbeConfig();

// File-backed controls used by the diagnostic panel. The PROBE file remains authoritative: the
// panel only edits or removes the same file that headless tooling uses.
[[nodiscard]] const char* GetProbeConfigFilePath();
[[nodiscard]] std::string GetProbeConfigText();
bool WriteProbeConfigText(std::string_view text, std::string* error);
bool RemoveProbeConfigFile(std::string* error);

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
