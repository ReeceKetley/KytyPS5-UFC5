#include "common/assert.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

// Diagnostic only (default OFF): snapshot a value, or record that a point in the program was
// reached, at an ARBITRARY pc - not just at an image store. KYTY_PROBE_VGPR can only read
// registers as they are at the store, which cannot distinguish "this block never ran" from "this
// block ran and wrote the same value the previous block left", because the game's shaders reuse
// registers across blocks. A marker answers that directly.
//
//   KYTY_PROBE_MARK=<pc>:<dst>[,...]        at <pc>, set v<dst> = 1.0 for every invocation that
//                                           reaches it, ignoring EXEC  -> "was this block executed"
//   KYTY_PROBE_MARKX=<pc>:<dst>[,...]       same, but only for lanes whose EXEC bit is set
//                                           -> "was this lane active here"
//   KYTY_PROBE_TAP=<pc>:<src>:<dst>[,...]   at <pc>, copy v<src> into v<dst>, ignoring EXEC
//                                           -> the value of a register at a chosen point
//   KYTY_PROBE_EXECLO=<pc>:<dst>[,...]      at <pc>, store the wave-wide EXEC mask WORD
//   KYTY_PROBE_EXECZ=<pc>:<dst>[,...]       at <pc>, store MaskIsZero(exec) as 1.0 / 0.0 - the
//                                           exact condition an S_CBRANCH_EXECZ terminator branches
//                                           on. EXEC has two representations (the per-lane bit
//                                           GetExec() and the mask word GetExecLo()) and they can
//                                           disagree; MARKX measures the first, this the second.
//
// <pc> is hex, the registers decimal. Every destination is zeroed at pc 0, so 0 means "never
// written" rather than "undefined", and destinations should be scratch registers above everything
// the shader allocates (the register file is IR::NumVectorRegs deep). Read them back with
// KYTY_PROBE_VGPR at the store. Scoped by KYTY_PROBE_HASH like the rest of the probe.
namespace {

constexpr int kProbeMarker = -1;  // write 1.0
constexpr int kProbeExecLo = -2;  // write the EXEC mask word
constexpr int kProbeExecZ  = -3;  // write 1.0 when the EXEC mask word is zero, else 0.0

struct ProbeTap {
	uint32_t pc;
	int      src;  // >= 0: copy that register. Otherwise one of the kProbe* kinds.
	uint32_t dst;
	bool     masked;  // honour EXEC (MARKX) instead of writing unconditionally
};

std::vector<ProbeTap> ParseProbeTaps() {
	std::vector<ProbeTap> taps;
	// `kind` >= 0 reads a register named in the middle field; the kProbe* kinds take <pc>:<dst>.
	const auto parse = [&taps](const char* cursor, int kind, bool masked) {
		while (cursor != nullptr && *cursor != '\0') {
			while (*cursor == ',' || *cursor == ' ') {
				++cursor;
			}
			char*      end = nullptr;
			const auto pc  = std::strtoul(cursor, &end, 16);
			if (end == cursor || *end != ':') {
				break;
			}
			cursor   = end + 1;
			long src = kind;
			if (kind >= 0) {
				src = std::strtol(cursor, &end, 10);
				if (end == cursor || *end != ':' || src < 0 || src >= IR::NumVectorRegs) {
					break;
				}
				cursor = end + 1;
			}
			const auto dst = std::strtoul(cursor, &end, 10);
			if (end == cursor || dst >= IR::NumVectorRegs) {
				break;
			}
			cursor = end;
			taps.push_back({static_cast<uint32_t>(pc), static_cast<int>(src),
			                static_cast<uint32_t>(dst), masked});
		}
	};
	parse(std::getenv("KYTY_PROBE_TAP"), 0, false);
	parse(std::getenv("KYTY_PROBE_MARK"), kProbeMarker, false);
	parse(std::getenv("KYTY_PROBE_MARKX"), kProbeMarker, true);
	parse(std::getenv("KYTY_PROBE_EXECLO"), kProbeExecLo, false);
	parse(std::getenv("KYTY_PROBE_EXECZ"), kProbeExecZ, false);
	return taps;
}

const std::vector<ProbeTap>& ProbeTaps() {
	static const std::vector<ProbeTap> taps = ParseProbeTaps();
	return taps;
}

uint64_t ProbeHash() {
	static const uint64_t hash = [] {
		const char* env = std::getenv("KYTY_PROBE_HASH");
		return env == nullptr ? 0ull : std::strtoull(env, nullptr, 16);
	}();
	return hash;
}

} // namespace

uint32_t ProbeScratchVectorLimit(uint64_t shader_hash) {
	if (ProbeHash() != 0 && ProbeHash() != shader_hash) {
		return 0;
	}
	uint32_t limit = 0;
	for (const auto& tap: ProbeTaps()) {
		limit = std::max(limit, tap.dst + 1u);
	}
	return limit;
}

void Translator::EmitProbeTaps(uint32_t pc) {
	const auto& taps = ProbeTaps();
	if (taps.empty() || (ProbeHash() != 0 && ProbeHash() != current_shader_hash)) {
		return;
	}
	const auto write = [this](uint32_t dst, IR::U32 value, bool masked) {
		const auto reg = static_cast<IR::VectorReg>(dst);
		ir.SetVectorReg(reg, masked ? ir.Select(ir.GetExec(), value, ir.GetVectorReg(reg)) : value);
	};
	if (pc == 0) {
		// Zero first, so a destination that is never reached reads back as 0 rather than as an
		// undefined value, and so a tap placed at pc 0 still wins over the initialisation.
		for (const auto& tap: taps) {
			write(tap.dst, IR::U32(IR::Value(0u)), false);
		}
	}
	for (const auto& tap: taps) {
		if (tap.pc != pc) {
			continue;
		}
		IR::U32 value {IR::Value(0x3f800000u)};  // 1.0f
		switch (tap.src) {
			case kProbeMarker: break;
			case kProbeExecLo: value = ir.GetExecLo(); break;
			case kProbeExecZ:
				value = ir.Select(MaskIsZero(false), IR::U32(IR::Value(0x3f800000u)),
				                  IR::U32(IR::Value(0u)));
				break;
			default: value = ir.GetVectorReg(static_cast<IR::VectorReg>(tap.src)); break;
		}
		write(tap.dst, value, tap.masked);
	}
}

void Translator::TranslateInstruction(const Decoder::Instruction& inst) {
	current_opcode = inst.opcode;
	current_pc     = inst.pc;

	EmitProbeTaps(inst.pc);

	switch (inst.opcode) {
		case Decoder::Opcode::UNKNOWN:
		case Decoder::Opcode::COUNT:
			EXIT("decoded opcode has no IR translation at pc 0x%08x", inst.pc);
		case Decoder::Opcode::UNSUPPORTED:
			EXIT("unsupported decoded instruction: %s", Decoder::InstructionToString(inst).c_str());
		default: break;
	}

	bool translated = false;
	switch (inst.family) {
		case Decoder::Family::SOP1:
		case Decoder::Family::SOP2:
		case Decoder::Family::SOPK:
		case Decoder::Family::SOPC:
		case Decoder::Family::SOPP: translated = EmitScalar(inst); break;
		case Decoder::Family::VOP1:
		case Decoder::Family::VOP2:
		case Decoder::Family::VOP3:
		case Decoder::Family::VOP3P:
		case Decoder::Family::VOPC: translated = EmitVector(inst); break;
		case Decoder::Family::SMEM:
		case Decoder::Family::MUBUF:
		case Decoder::Family::MTBUF:
		case Decoder::Family::FLAT:
		case Decoder::Family::DS:
		case Decoder::Family::MIMG: translated = EmitMemory(inst); break;
		case Decoder::Family::VINTRP: translated = EmitInterpolation(inst); break;
		case Decoder::Family::EXP:
			EXP(inst);
			translated = true;
			break;
		default: break;
	}

	if (!translated) {
		EXIT("opcode %s at pc 0x%08x has no IR translation",
		     Decoder::InstructionToString(inst).c_str(), inst.pc);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
