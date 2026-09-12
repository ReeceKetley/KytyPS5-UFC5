#include "common/assert.h"
#include "graphics/shader/recompiler/frontend/translate/ProbeConfig.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>

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
//
// The configuration is a live snapshot (see ProbeConfig.h): it can be re-aimed mid-run by writing
// D:/PS5/dumps/PROBE, without restarting and without re-navigating the game. `probe` is taken once
// per Translator, so one translation always sees one consistent configuration.

uint32_t ProbeScratchVectorLimit(uint64_t shader_hash) {
	const auto config = GetProbeConfig();
	return config->AppliesTo(shader_hash) ? config->ScratchVectorLimit() : 0u;
}

void Translator::EmitProbeTaps(uint32_t pc) {
	const auto& taps = probe->taps;
	if (taps.empty() || !probe->AppliesTo(current_shader_hash)) {
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
		switch (tap.kind) {
			case ProbeTap::Kind::Marker: break;
			case ProbeTap::Kind::ExecLo: value = ir.GetExecLo(); break;
			case ProbeTap::Kind::ExecZ:
				value = ir.Select(MaskIsZero(false), IR::U32(IR::Value(0x3f800000u)),
				                  IR::U32(IR::Value(0u)));
				break;
			case ProbeTap::Kind::Copy:
				value = ir.GetVectorReg(static_cast<IR::VectorReg>(tap.src));
				break;
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
