#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include "common/assert.h"
#include "common/timer.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return "vertex";
		case ShaderType::Pixel: return "pixel";
		case ShaderType::Fetch: return "fetch";
		case ShaderType::Compute: return "compute";
		default: return "unknown";
	}
}

std::string Diagnostic(const ResourcePlan& program, uint32_t pc, const std::string& message) {
	return fmt::format("shader SRT: hash=0x{:016x} stage={} pc=0x{:08x} {}", program.shader_hash,
	                   StageName(program.stage), pc, message);
}

bool AddSignedAddress(uint64_t base, int64_t offset, uint64_t& result) {
	if (base > AddressMask) {
		return false;
	}
	if (offset < 0) {
		const auto magnitude = uint64_t {0} - static_cast<uint64_t>(offset);
		if (magnitude > base) {
			return false;
		}
		result = base - magnitude;
		return true;
	}
	const auto magnitude = static_cast<uint64_t>(offset);
	if (magnitude > AddressMask - base) {
		return false;
	}
	result = base + magnitude;
	return true;
}

bool IsRawRead(const ResourcePlan& values, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= values.memory_info.size()) {
		return false;
	}
	const auto kind = values.memory_info[index].kind;
	return (op == ValueOpcode::LoadAddressU32 && kind == ResourceKind::ScalarAddress) ||
	       (op == ValueOpcode::ReadConstBuffer && kind == ResourceKind::ScalarBuffer);
}

bool IsDescriptorHandle(ValueOpcode opcode) {
	switch (opcode) {
		case ValueOpcode::GetBufferResource:
		case ValueOpcode::GetAddressResource:
		case ValueOpcode::GetImageResource:
		case ValueOpcode::GetSamplerResource: return true;
		default: return false;
	}
}

bool IsRuntimeSelect(ValueOpcode op) {
	return op == ValueOpcode::SelectU1 || op == ValueOpcode::SelectU32 ||
	       op == ValueOpcode::SelectF32;
}

bool IsRuntimeUniformOp(ValueOpcode op) {
	switch (op) {
		case ValueOpcode::BitCastU32F32:
		case ValueOpcode::BitCastF32U32:
		case ValueOpcode::ConvertU32F32:
		case ValueOpcode::ConvertF32U32:
		case ValueOpcode::CompositeConstructU64:
		case ValueOpcode::CompositeExtractU64:
		case ValueOpcode::CompositeConstructU32x2:
		case ValueOpcode::CompositeExtractU32x2:
		case ValueOpcode::BitFieldInsert:
		case ValueOpcode::BitFieldUExtract:
		case ValueOpcode::BitFieldSExtract:
		case ValueOpcode::IAdd32:
		case ValueOpcode::IAdd64:
		case ValueOpcode::IAddCarry32:
		case ValueOpcode::ISub32:
		case ValueOpcode::ISub64:
		case ValueOpcode::IMul32:
		case ValueOpcode::IMul64:
		case ValueOpcode::UMin32:
		case ValueOpcode::ShiftLeftLogical32:
		case ValueOpcode::ShiftLeftLogical64:
		case ValueOpcode::ShiftRightLogical32:
		case ValueOpcode::ShiftRightLogical64:
		case ValueOpcode::ShiftRightArithmetic32:
		case ValueOpcode::ShiftRightArithmetic64:
		case ValueOpcode::BitwiseAnd32:
		case ValueOpcode::BitwiseAnd64:
		case ValueOpcode::BitwiseOr32:
		case ValueOpcode::BitwiseXor32:
		case ValueOpcode::BitwiseNot32:
		case ValueOpcode::SelectU1:
		case ValueOpcode::SelectU32:
		case ValueOpcode::SelectF32:
		case ValueOpcode::ULessThan32:
		case ValueOpcode::IEqual32:
		case ValueOpcode::UGreaterThan32:
		case ValueOpcode::INotEqual32:
		case ValueOpcode::LogicalOr:
		case ValueOpcode::LogicalAnd:
		case ValueOpcode::LogicalXor:
		case ValueOpcode::LogicalNot:
		case ValueOpcode::FPOrdLessThanEqual32:
		case ValueOpcode::FPOrdGreaterThanEqual32:
		case ValueOpcode::FPIsNan32:
		case ValueOpcode::FPMul32:
		case ValueOpcode::FPTrunc32: return true;
		default: return false;
	}
}

class RuntimeValidator {
public:
	explicit RuntimeValidator(const ResourcePlan& program, RuntimeValueType type)
	    : m_program(program), m_type(type) {}

	bool Run(Value value) { return Validate(value); }

private:
	bool ValidateArguments(const Inst& inst, bool require_uniform) {
		for (size_t index = 0; index < inst.NumArgs(); index++) {
			if (!Validate(inst.Arg(index), require_uniform)) return false;
		}
		return true;
	}

	bool Validate(Value value, bool require_uniform = true) {
		value = value.Resolve();
		// Host floating-point evaluation does not model shader rounding/denormal modes.
		if (m_type == RuntimeValueType::Integer &&
		    TypesOverlap(value.GetType(), Type::F16 | Type::F32 | Type::F32x2)) {
			return false;
		}
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			if (!require_uniform) return true;
			switch (value.GetType()) {
				case Type::U1:
				case Type::U8:
				case Type::U16:
				case Type::U32:
				case Type::U64:
				case Type::F32: return true;
				default: return false;
			}
		}
		// Integer-only dependency checks do not depend on the active EXEC mask.
		if (!require_uniform && m_validated_dependencies.contains(inst)) return true;
		if (!m_visiting.insert(inst).second) {
			return !require_uniform;
		}
		const auto finish = [&](bool valid) {
			m_visiting.erase(inst);
			if (valid && !require_uniform) m_validated_dependencies.insert(inst);
			return valid;
		};
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::ReadConst) {
			const auto slot = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (inst->NumArgs() != 2 || inst->Arg(0).Resolve().TryInstruction() == nullptr ||
			    inst->Arg(0).Resolve().TryInstruction()->GetOpcode() !=
			        ValueOpcode::GetSrtResource ||
			    !slot.IsImmediate() || slot.GetType() != Type::U32 ||
			    slot.U32() >= m_program.srt_reads.size()) {
				return finish(false);
			}
			if (m_type == RuntimeValueType::Integer) {
				const auto active_mask = m_active_mask;
				m_active_mask          = {};
				const bool valid       = Validate(m_program.srt_reads[slot.U32()].value);
				m_active_mask          = active_mask;
				if (!valid) return finish(false);
			}
		}
		if (!require_uniform) return finish(ValidateArguments(*inst, false));
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(op) && inst->NumArgs() == 3 &&
		    inst->Arg(0).Resolve() == m_active_mask) {
			// Empty EXEC reads lane zero, so ignored operands still require integer types.
			if (m_type == RuntimeValueType::Integer && !Validate(inst->Arg(2), false)) {
				return finish(false);
			}
			return finish(Validate(inst->Arg(1)));
		}
		if (op == ValueOpcode::UndefU1 || op == ValueOpcode::UndefU8 ||
		    op == ValueOpcode::UndefU16 || op == ValueOpcode::UndefU32 ||
		    op == ValueOpcode::UndefU64 || op == ValueOpcode::Void) {
			return finish(false);
		}
		if (op == ValueOpcode::GetUserData) {
			if (inst->NumArgs() != 1 || inst->Arg(0).GetType() != Type::ScalarReg) {
				return finish(false);
			}
			const auto reg = RegIndex(inst->Arg(0).ScalarRegister());
			if (reg < m_program.user_data_base ||
			    reg - m_program.user_data_base >= m_program.user_data_count) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::GetShaderBase) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::Phi) {
			if (m_type == RuntimeValueType::Integer && !ValidateArguments(*inst, false)) {
				return finish(false);
			}
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (invariant.IsEmpty()) {
				return finish(false);
			}
			return finish(Validate(invariant));
		}
		if (op == ValueOpcode::ReadFirstLane) {
			if (inst->NumArgs() != 2 || inst->Arg(0).GetType() != Type::U32 ||
			    inst->Arg(1).GetType() != Type::U1) {
				return finish(false);
			}
			if (m_type == RuntimeValueType::Integer && !Validate(inst->Arg(1), false)) {
				return finish(false);
			}
			const auto active_mask = m_active_mask;
			m_active_mask          = inst->Arg(1).Resolve();
			const bool valid       = Validate(inst->Arg(0));
			m_active_mask          = active_mask;
			return finish(valid);
		}
		if (op == ValueOpcode::GetSrtResource) {
			if (inst->NumArgs() != 0) {
				return finish(false);
			}
			return finish(true);
		}
		if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
			const auto  expected = op == ValueOpcode::LoadAddressU32
			                           ? ValueOpcode::GetAddressResource
			                           : ValueOpcode::GetBufferResource;
			const auto* handle = inst->NumArgs() != 0 ? inst->Arg(0).ResolveInstruction() : nullptr;
			if (!IsRawRead(m_program, *inst) || handle == nullptr ||
			    handle->GetOpcode() != expected) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU64) {
			const auto index = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
				return finish(false);
			}
		} else if (op == ValueOpcode::CompositeExtractU32x2) {
			const auto* source = inst->NumArgs() == 2 ? inst->Arg(0).ResolveInstruction() : nullptr;
			const auto  index  = inst->NumArgs() == 2 ? inst->Arg(1).Resolve() : Value {};
			if (source == nullptr || !index.IsImmediate() || index.GetType() != Type::U32 ||
			    index.U32() >= 2u ||
			    (source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 &&
			     source->GetOpcode() != ValueOpcode::IAddCarry32)) {
				return finish(false);
			}
		}
		if (op == ValueOpcode::GetBufferResource || op == ValueOpcode::GetImageResource ||
		    op == ValueOpcode::GetSamplerResource || op == ValueOpcode::GetAddressResource) {
			size_t expected = 4u;
			if (op == ValueOpcode::GetImageResource) {
				expected = 8u;
			} else if (op == ValueOpcode::GetAddressResource) {
				expected = 2u;
			}
			if (inst->NumArgs() != expected) {
				return finish(false);
			}
		} else if (op != ValueOpcode::ReadConst && op != ValueOpcode::ReadConstBuffer &&
		           op != ValueOpcode::LoadAddressU32 && !IsRuntimeUniformOp(op)) {
			return finish(false);
		}
		return finish(ValidateArguments(*inst, true));
	}

	const ResourcePlan&             m_program;
	RuntimeValueType                m_type;
	Value                           m_active_mask;
	std::unordered_set<const Inst*> m_visiting;
	std::unordered_set<const Inst*> m_validated_dependencies;
};

class PlanBuilder {
public:
	explicit PlanBuilder(Program& program): m_program(program) {}

	void Run() {
		m_program.srt_reads.clear();
		m_program.dynamic_reads.clear();
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				const auto op = inst.GetOpcode();
				if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer) {
					const auto flags = inst.Flags<MemoryFlags>();
					if (flags.index < m_program.memory_info.size()) {
						const auto kind       = m_program.memory_info[flags.index].kind;
						const bool crosswired = (op == ValueOpcode::LoadAddressU32 &&
						                         kind == ResourceKind::ScalarBuffer) ||
						                        (op == ValueOpcode::ReadConstBuffer &&
						                         kind == ResourceKind::ScalarAddress);
						if (crosswired) {
							Fail(flags.pc,
							     fmt::format("{} has incompatible scalar memory metadata",
							                 ValueOpcodeName(op)));
						}
					}
				}
				if (IsDescriptorHandle(inst.GetOpcode())) {
					for (size_t index = 0; index < inst.NumArgs(); index++) {
						Collect(inst.Arg(index), 0);
					}
				}
			}
		}
		for (auto* block: m_program.blocks) {
			for (auto& inst: *block) {
				if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 && IsRawRead(m_program, inst) &&
				    inst.Arg(1).Resolve().IsImmediate() &&
				    ValidateRuntimeValue(m_program, Value(&inst))) {
					Collect(Value(&inst), inst.Flags<MemoryFlags>().pc);
				}
			}
		}
		PatchReads();
	}

private:
	struct Patch {
		Inst*    inst = nullptr;
		uint32_t slot = 0;
		bool     keep = false;
	};

	[[noreturn]] void Fail(uint32_t pc, const std::string& message) const {
		const auto diagnostic = Diagnostic(m_program, pc, message);
		EXIT("shader SRT planning failed: %s", diagnostic.c_str());
		std::abort();
	}

	void Collect(Value value, uint32_t use_pc) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			return;
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			Fail(use_pc, "invalid typed planning value");
		}
		const auto cycle = std::ranges::find(m_visiting, inst);
		if (cycle != m_visiting.end()) {
			const auto contains_phi = std::any_of(cycle, m_visiting.end(), [](const Inst* value) {
				return value->GetOpcode() == ValueOpcode::Phi;
			});
			if (contains_phi) {
				return;
			}
			Fail(use_pc, fmt::format("cyclic typed planning value {} without a phi",
			                         ValueOpcodeName(inst->GetOpcode())));
		}
		if (std::ranges::find(m_visited, inst) != m_visited.end()) {
			return;
		}
		m_visiting.push_back(inst);
		for (size_t index = 0; index < inst->NumArgs(); index++) {
			Collect(inst->Arg(index), use_pc);
		}
		m_visiting.pop_back();
		m_visited.push_back(inst);
		if (!IsRawRead(m_program, *inst)) {
			return;
		}
		const auto offset = inst->Arg(1).Resolve();
		if (!offset.IsImmediate() || offset.GetType() != Type::U32) {
			if (std::ranges::find(m_program.dynamic_reads, value) ==
			    m_program.dynamic_reads.end()) {
				m_program.dynamic_reads.push_back(value);
			}
			return;
		}
		for (uint32_t slot = 0; slot < m_program.srt_reads.size(); slot++) {
			if (EquivalentValue(m_program, value, m_program.srt_reads[slot].value)) {
				m_patches.push_back({inst, slot, false});
				return;
			}
		}
		const auto slot = static_cast<uint32_t>(m_program.srt_reads.size());
		m_program.srt_reads.push_back({value, slot});
		m_patches.push_back({inst, slot, true});
	}

	void PatchReads() {
		for (const auto& patch: m_patches) {
			auto* block = patch.inst->Parent();
			auto& list  = block->Instructions();
			auto  where =
			    std::ranges::find_if(list, [&](const Inst& inst) { return &inst == patch.inst; });
			const auto resource =
			    Value(&*block->PrependNewInst(where, ValueOpcode::GetSrtResource));
			const auto flat = Value(&*block->PrependNewInst(where, ValueOpcode::ReadConst,
			                                                {resource, Value(patch.slot)}));
			const auto uses = patch.inst->Uses();
			for (const auto& use: uses) {
				use.user->SetArg(use.operand, flat);
			}
			for (auto& info: m_program.block_info) {
				if (info.condition.Resolve() == Value(patch.inst)) {
					info.condition = flat;
				}
				if (info.indirect_target.Resolve() == Value(patch.inst)) {
					info.indirect_target = flat;
				}
			}
			if (patch.keep) {
				const auto memory = patch.inst->Flags<MemoryFlags>().index;
				if (memory < m_program.memory_info.size()) {
					m_program.memory_info[memory].planning_only = true;
				}
				block->AppendNewInst(ValueOpcode::ReferenceU32, {Value(patch.inst)});
			}
		}
	}

	Program&           m_program;
	std::vector<Inst*> m_visiting;
	std::vector<Inst*> m_visited;
	std::vector<Patch> m_patches;
};

struct EvaluatorScratch {
	std::unordered_map<const Inst*, std::pair<uint32_t, uint64_t>> cache;
	std::vector<const Inst*>                                       visiting;
	uint32_t                                                       generation = 0;
	bool                                                           in_use     = false;

	void Begin() {
		visiting.clear();
		if (++generation == 0) {
			cache.clear();
			generation = 1;
		}
	}
};

class ScratchLease final {
public:
	ScratchLease() {
		auto& pool = Pool();
		for (auto& slot: pool) {
			if (!slot->in_use) {
				m_slot = slot.get();
				break;
			}
		}
		if (m_slot == nullptr) {
			pool.push_back(std::make_unique<EvaluatorScratch>());
			m_slot = pool.back().get();
		}
		m_slot->in_use = true;
		m_slot->Begin();
	}

	~ScratchLease() { m_slot->in_use = false; }

	ScratchLease(const ScratchLease&)            = delete;
	ScratchLease& operator=(const ScratchLease&) = delete;

	[[nodiscard]] EvaluatorScratch& Get() const { return *m_slot; }

private:
	static std::vector<std::unique_ptr<EvaluatorScratch>>& Pool() {
		static thread_local std::vector<std::unique_ptr<EvaluatorScratch>> pool;
		return pool;
	}

	EvaluatorScratch* m_slot = nullptr;
};

// Accumulates time spent inside the guest-memory read callback, reported alongside the
// per-call SrtEval split so the walk can be divided into interpreter dispatch vs the reads
// themselves. See EvaluateRawRead for why that division decides the JIT's shape.
std::atomic<uint64_t> g_guest_read_ticks {0};
std::atomic<uint64_t> g_guest_read_count {0};

void NoteGuestRead(uint64_t ticks) {
	g_guest_read_ticks.fetch_add(ticks, std::memory_order_relaxed);
	g_guest_read_count.fetch_add(1, std::memory_order_relaxed);
}

// Opcode census for the SRT JIT scoping decision (see EvaluateInst). `hot` counts the
// pointer-chase / integer-address subset that a shadPS4-shaped code generator covers; the
// rest would have to fall back to this interpreter, and a high `cold` share means a JIT has
// to grow into a general expression backend before it pays.
void NoteEvaluatedOpcode(ValueOpcode opcode) {
	const bool hot = [opcode] {
		switch (opcode) {
			case ValueOpcode::GetUserData:
			case ValueOpcode::ReadConst:
			case ValueOpcode::GetShaderBase:
			case ValueOpcode::IAdd32:
			case ValueOpcode::IAdd64:
			case ValueOpcode::ISub32:
			case ValueOpcode::IMul32:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftLeftLogical64:
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::ShiftRightLogical64:
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseAnd64:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::CompositeConstructU64:
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeConstructU32x2:
			case ValueOpcode::CompositeExtractU32x2: return true;
			default: return false;
		}
	}();

	static std::atomic<uint64_t> total {0};
	static std::atomic<uint64_t> hot_count {0};
	static std::atomic<uint32_t> cold_histogram[256] {};
	const auto                   raw = static_cast<uint32_t>(opcode);
	if (hot) {
		hot_count.fetch_add(1, std::memory_order_relaxed);
	} else if (raw < 256) {
		cold_histogram[raw].fetch_add(1, std::memory_order_relaxed);
	}
	const auto n = total.fetch_add(1, std::memory_order_relaxed) + 1;
	if ((n % 1048576) != 0) {
		return;
	}
	const auto hot_total = hot_count.exchange(0);
	std::string worst;
	for (int rank = 0; rank < 5; rank++) {
		uint32_t best_index = 0;
		uint32_t best_value = 0;
		for (uint32_t i = 0; i < 256; i++) {
			const auto value = cold_histogram[i].load(std::memory_order_relaxed);
			if (value > best_value) {
				best_value = value;
				best_index = i;
			}
		}
		if (best_value == 0) {
			break;
		}
		cold_histogram[best_index].store(0, std::memory_order_relaxed);
		worst += fmt::format(" op{}={}", best_index, best_value);
	}
	for (auto& slot: cold_histogram) {
		slot.store(0, std::memory_order_relaxed);
	}
	LOGF("SrtOps/1M: hot=%.1f%% cold_top5:%s\n", hot_total * 100.0 / 1048576.0, worst.c_str());
}

class Evaluator {
public:
	Evaluator(const ResourcePlan& program, const SrtRuntime& runtime,
	          std::span<const uint8_t> clean_flat_slots = {}, Evaluator* clean_evaluator = nullptr,
	          Value active_mask = {})
	    : m_program(program), m_runtime(runtime), m_clean_flat_slots(clean_flat_slots),
	      m_clean_evaluator(clean_evaluator), m_active_mask(active_mask.Resolve()) {}

	bool Evaluate(Value value, uint32_t& result) {
		uint64_t wide = 0;
		if (!EvaluateWide(value, wide)) {
			return false;
		}
		result = static_cast<uint32_t>(wide);
		return true;
	}

private:
	static float Float32(uint64_t bits) {
		return std::bit_cast<float>(static_cast<uint32_t>(bits));
	}

	static uint64_t Float32Bits(float value) { return std::bit_cast<uint32_t>(value); }

	bool EvaluateWide(Value value, uint64_t& result) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			switch (value.GetType()) {
				case Type::U1: result = value.U1(); return true;
				case Type::U8: result = value.U8(); return true;
				case Type::U16: result = value.U16(); return true;
				case Type::U32: result = value.U32(); return true;
				case Type::U64: result = value.U64(); return true;
				case Type::F32: result = Float32Bits(value.F32Value()); return true;
				default: return false;
			}
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return false;
		}
		if (!m_active_mask.IsEmpty() && IsRuntimeSelect(inst->GetOpcode()) &&
		    inst->NumArgs() == 3 && inst->Arg(0).Resolve() == m_active_mask) {
			return EvaluateWide(inst->Arg(1), result);
		}
		if (const auto found = m_cache.find(inst);
		    found != m_cache.end() && found->second.first == m_generation) {
			result = found->second.second;
			return true;
		}
		if (std::ranges::find(m_visiting, inst) != m_visiting.end()) {
			return false;
		}
		m_visiting.push_back(inst);
		uint64_t out = 0;
		const bool evaluated = EvaluateInst(*inst, out);
		m_visiting.pop_back();
		if (!evaluated) {
			return false;
		}
		m_cache.insert_or_assign(inst, std::pair {m_generation, out});
		result = out;
		return true;
	}

	bool Arg(const Inst& inst, size_t index, uint64_t& result) {
		return EvaluateWide(inst.Arg(index), result);
	}

	bool EvaluatePhi(const Inst& inst, uint64_t& result) {
		const auto value = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
		return !value.IsEmpty() && EvaluateWide(value, result);
	}

	bool EvaluateExtract(const Inst& inst, uint64_t& result) {
		const auto index = inst.Arg(1).Resolve();
		if (!index.IsImmediate() || index.GetType() != Type::U32) {
			return false;
		}
		const auto component = index.U32();
		if (component >= 2u) {
			return false;
		}
		if (inst.GetOpcode() == ValueOpcode::CompositeExtractU64) {
			uint64_t packed = 0;
			if (!Arg(inst, 0, packed)) {
				return false;
			}
			result = static_cast<uint32_t>(packed >> (component * 32u));
			return true;
		}
		const auto* source = inst.Arg(0).ResolveInstruction();
		if (source == nullptr) {
			return false;
		}
		if (source->GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
			return EvaluateWide(source->Arg(component), result);
		}
		if (source->GetOpcode() == ValueOpcode::IAddCarry32) {
			uint64_t lhs = 0;
			uint64_t rhs = 0;
			if (!Arg(*source, 0, lhs) || !Arg(*source, 1, rhs)) {
				return false;
			}
			const auto sum =
			    static_cast<uint64_t>(static_cast<uint32_t>(lhs)) + static_cast<uint32_t>(rhs);
			result =
			    component == 0u ? static_cast<uint32_t>(sum) : static_cast<uint32_t>(sum >> 32u);
			return true;
		}
		return false;
	}

	bool EvaluateRawRead(const Inst& inst, uint64_t& result) {
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) {
			return false;
		}
		const auto& mem    = m_program.memory_info[flags.index];
		const auto* handle = inst.Arg(0).ResolveInstruction();
		if (handle == nullptr) {
			return false;
		}
		uint64_t low    = 0;
		uint64_t high   = 0;
		uint64_t offset = 0;
		if (!Arg(*handle, 0, low) || !Arg(*handle, 1, high) || !Arg(inst, 1, offset)) {
			return false;
		}
		const auto base      = ((high << 32u) | static_cast<uint32_t>(low)) & AddressMask;
		const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
		uint64_t   address   = 0;
		if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
			uint64_t records = 0;
			uint64_t word3   = 0;
			if (handle->NumArgs() != 4u || !Arg(*handle, 2, records) || !Arg(*handle, 3, word3)) {
				return false;
			}
			if (immediate < 0) {
				return false;
			}
			const auto byte_offset =
			    static_cast<uint64_t>(immediate) + static_cast<uint32_t>(offset);
			const auto aligned = byte_offset & ~uint64_t {3};
			const auto stride  = (static_cast<uint32_t>(high) >> 16u) & 0x3fffu;
			const auto size = stride == 0u
			                      ? static_cast<uint64_t>(static_cast<uint32_t>(records))
			                      : static_cast<uint64_t>(stride) * static_cast<uint32_t>(records);
			if (aligned > size || size - aligned < sizeof(uint32_t)) {
				return false;
			}
			address = ((base & ~uint64_t {3}) + byte_offset) & ~uint64_t {3};
		} else {
			const auto relative = (immediate & ~int64_t {3}) +
			                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
			if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
				return false;
			}
		}
		uint32_t word = 0;
		// Splits the walk into "interpreter dispatch" and "the guest read itself". Every load
		// goes through an indirect call into read_memory (~120 per draw across both stages),
		// which a flat/bytecode rewrite would keep and only an Xbyak walker reading raw
		// pointers could remove - at the cost of a vectored exception handler for faults. If
		// this is most of the cost, the cheap rewrite is not worth building.
		const auto rd_t0 = Common::Timer::QueryPerformanceCounter();
		bool       ok    = true;
		if (m_runtime.read_memory != nullptr) {
			ok = m_runtime.read_memory(m_runtime.userdata, address, &word);
		} else {
			std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
		}
		NoteGuestRead(Common::Timer::QueryPerformanceCounter() - rd_t0);
		if (!ok) {
			return false;
		}
		result = word;
		return true;
	}

	bool EvaluateInst(const Inst& inst, uint64_t& result) {
		// Scope check for the SRT JIT. shadPS4's walker compiles because their SRT walk is a
		// pure ReadConst pointer chase - a chain of loads. Kyty's evaluator is a general
		// expression interpreter over ~45 opcodes including f32 and 64-bit arithmetic, so a
		// code generator must either cover all of them or bail to the interpreter. Which of
		// those it is depends entirely on which opcodes UFC5 actually evaluates, so count
		// them: `hot` is the pointer-chase subset a shadPS4-shaped JIT would handle.
		NoteEvaluatedOpcode(inst.GetOpcode());
		uint64_t   a       = 0;
		uint64_t   b       = 0;
		uint64_t   c       = 0;
		const auto binary  = [&]() { return Arg(inst, 0, a) && Arg(inst, 1, b); };
		const auto ternary = [&]() {
			return Arg(inst, 0, a) && Arg(inst, 1, b) && Arg(inst, 2, c);
		};
		switch (inst.GetOpcode()) {
			case ValueOpcode::GetUserData: {
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base ||
				    reg - m_program.user_data_base >= m_runtime.user_data.size()) {
					return false;
				}
				result = m_runtime.user_data[reg - m_program.user_data_base];
				return true;
			}
			case ValueOpcode::GetShaderBase: result = m_runtime.shader_base; return true;
			case ValueOpcode::Phi: return EvaluatePhi(inst, result);
			case ValueOpcode::ReadFirstLane: {
				Evaluator active(m_program, m_runtime, m_clean_flat_slots, m_clean_evaluator,
				                 inst.Arg(1));
				return active.EvaluateWide(inst.Arg(0), result);
			}
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32: return Arg(inst, 0, result);
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: return EvaluateExtract(inst, result);
			case ValueOpcode::CompositeConstructU64:
				if (!binary()) {
					return false;
				}
				result = static_cast<uint32_t>(a) |
				         (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
				return true;
			case ValueOpcode::ReadConst: {
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size()) {
					return false;
				}
				if (slot.U32() < m_clean_flat_slots.size() &&
				    m_clean_flat_slots[slot.U32()] != 0u && m_clean_evaluator != nullptr) {
					return m_clean_evaluator->EvaluateWide(m_program.srt_reads[slot.U32()].value,
					                                       result);
				}
				return EvaluateWide(m_program.srt_reads[slot.U32()].value, result);
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
				if (IsRawRead(m_program, inst)) {
					return EvaluateRawRead(inst, result);
				}
				break;
			case ValueOpcode::IAdd32:
				if (binary()) {
					result = static_cast<uint32_t>(a + b);
					return true;
				}
				return false;
			case ValueOpcode::IAdd64:
				if (binary()) {
					result = a + b;
					return true;
				}
				return false;
			case ValueOpcode::ISub32:
				if (binary()) {
					result = static_cast<uint32_t>(a - b);
					return true;
				}
				return false;
			case ValueOpcode::ISub64:
				if (binary()) {
					result = a - b;
					return true;
				}
				return false;
			case ValueOpcode::IMul32:
				if (binary()) {
					result = static_cast<uint32_t>(a * b);
					return true;
				}
				return false;
			case ValueOpcode::IMul64:
				if (binary()) {
					result = a * b;
					return true;
				}
				return false;
			case ValueOpcode::UMin32:
				if (binary()) {
					result = std::min(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
					return true;
				}
				return false;
			case ValueOpcode::ConvertF32U32:
				if (Arg(inst, 0, a)) {
					result = Float32Bits(static_cast<float>(static_cast<uint32_t>(a)));
					return true;
				}
				return false;
			case ValueOpcode::ConvertU32F32:
				if (Arg(inst, 0, a)) {
					const auto value = Float32(a);
					if (!std::isfinite(value) || value < 0.0f ||
					    static_cast<double>(value) > UINT32_MAX) {
						return false;
					}
					result = static_cast<uint32_t>(value);
					return true;
				}
				return false;
			case ValueOpcode::FPMul32:
				if (binary()) {
					result = Float32Bits(Float32(a) * Float32(b));
					return true;
				}
				return false;
			case ValueOpcode::FPTrunc32:
				if (Arg(inst, 0, a)) {
					result = Float32Bits(std::trunc(Float32(a)));
					return true;
				}
				return false;
			case ValueOpcode::FPIsNan32:
				if (Arg(inst, 0, a)) {
					result = std::isnan(Float32(a));
					return true;
				}
				return false;
			case ValueOpcode::FPOrdLessThanEqual32:
				if (binary()) {
					result = Float32(a) <= Float32(b);
					return true;
				}
				return false;
			case ValueOpcode::FPOrdGreaterThanEqual32:
				if (binary()) {
					result = Float32(a) >= Float32(b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseAnd32:
				if (binary()) {
					result = static_cast<uint32_t>(a & b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseAnd64:
				if (binary()) {
					result = a & b;
					return true;
				}
				return false;
			case ValueOpcode::BitwiseOr32:
				if (binary()) {
					result = static_cast<uint32_t>(a | b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseXor32:
				if (binary()) {
					result = static_cast<uint32_t>(a ^ b);
					return true;
				}
				return false;
			case ValueOpcode::BitwiseNot32:
				if (Arg(inst, 0, a)) {
					result = ~static_cast<uint32_t>(a);
					return true;
				}
				return false;
			case ValueOpcode::ShiftLeftLogical32:
				if (binary()) {
					result = static_cast<uint32_t>(a) << (b & 31u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftLeftLogical64:
				if (binary()) {
					result = a << (b & 63u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightLogical32:
				if (binary()) {
					result = static_cast<uint32_t>(a) >> (b & 31u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightLogical64:
				if (binary()) {
					result = a >> (b & 63u);
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightArithmetic32:
				if (binary()) {
					result = static_cast<uint32_t>(
					    std::bit_cast<int32_t>(static_cast<uint32_t>(a)) >> (b & 31u));
					return true;
				}
				return false;
			case ValueOpcode::ShiftRightArithmetic64:
				if (binary()) {
					result = static_cast<uint64_t>(std::bit_cast<int64_t>(a) >> (b & 63u));
					return true;
				}
				return false;
			case ValueOpcode::BitFieldUExtract:
				if (ternary()) {
					const auto offset = static_cast<uint32_t>(b);
					const auto width  = static_cast<uint32_t>(c);
					if (offset > 32u || width > 32u - offset) {
						return false;
					}
					const auto mask = width == 32u  ? UINT32_MAX
					                  : width == 0u ? 0u
					                                : (uint32_t {1} << width) - 1u;
					result = width == 0u ? 0u : (static_cast<uint32_t>(a) >> offset) & mask;
					return true;
				}
				return false;
			case ValueOpcode::BitFieldSExtract:
				if (ternary()) {
					const auto offset = static_cast<uint32_t>(b);
					const auto width  = static_cast<uint32_t>(c);
					if (offset > 32u || width > 32u - offset) {
						return false;
					}
					if (width == 0u) {
						result = 0;
						return true;
					}
					const auto mask = width == 32u ? UINT32_MAX : (uint32_t {1} << width) - 1u;
					auto       bits = (static_cast<uint32_t>(a) >> offset) & mask;
					if (width < 32u && (bits & (uint32_t {1} << (width - 1u))) != 0u) {
						bits |= ~mask;
					}
					result = bits;
					return true;
				}
				return false;
			case ValueOpcode::BitFieldInsert: {
				uint64_t d = 0;
				if (!ternary() || !Arg(inst, 3, d)) {
					return false;
				}
				const auto offset = static_cast<uint32_t>(c);
				const auto width  = static_cast<uint32_t>(d);
				if (offset > 32u || width > 32u - offset) {
					return false;
				}
				if (width == 0u) {
					result = static_cast<uint32_t>(a);
					return true;
				}
				const auto mask =
				    width == 32u ? UINT32_MAX : ((uint32_t {1} << width) - 1u) << offset;
				result = (static_cast<uint32_t>(a) & ~mask) |
				         ((static_cast<uint32_t>(b) << offset) & mask);
				return true;
			}
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectF32:
				if (ternary()) {
					result = a != 0u ? b : c;
					return true;
				}
				return false;
			case ValueOpcode::IEqual32:
				if (binary()) {
					result = static_cast<uint32_t>(a) == static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::INotEqual32:
				if (binary()) {
					result = static_cast<uint32_t>(a) != static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::ULessThan32:
				if (binary()) {
					result = static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::UGreaterThan32:
				if (binary()) {
					result = static_cast<uint32_t>(a) > static_cast<uint32_t>(b);
					return true;
				}
				return false;
			case ValueOpcode::LogicalAnd:
				if (binary()) {
					result = (a != 0u) && (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalOr:
				if (binary()) {
					result = (a != 0u) || (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalXor:
				if (binary()) {
					result = (a != 0u) != (b != 0u);
					return true;
				}
				return false;
			case ValueOpcode::LogicalNot:
				if (Arg(inst, 0, a)) {
					result = a == 0u;
					return true;
				}
				return false;
			case ValueOpcode::UndefU1:
			case ValueOpcode::UndefU8:
			case ValueOpcode::UndefU16:
			case ValueOpcode::UndefU32:
			case ValueOpcode::UndefU64: return false;
			default: break;
		}
		return false;
	}

	const ResourcePlan&                                             m_program;
	const SrtRuntime&                                               m_runtime;
	std::span<const uint8_t>                                        m_clean_flat_slots;
	Evaluator*                                                      m_clean_evaluator = nullptr;
	Value                                                           m_active_mask;
	ScratchLease                                                    m_scratch;
	std::unordered_map<const Inst*, std::pair<uint32_t, uint64_t>>& m_cache = m_scratch.Get().cache;
	std::vector<const Inst*>& m_visiting   = m_scratch.Get().visiting;
	uint32_t                  m_generation = m_scratch.Get().generation;
};

// ---------------------------------------------------------------------------------------
// SRT linear program: compile the value DAG once, evaluate it as a flat array per draw.
//
// Measured motivation (in-fight, this build): SrtEval sources=13.14us srtreads=6.93us per call
// against SrtReads guest_read=1.52us - the guest memory access is 7% of the walk and the
// tree-walking interpreter is the other 93%, at ~195ns per evaluated node. SrtOps says the
// opcode mix is narrow: LoadAddressU32 alone is 57% of all evaluated nodes, the declared "hot"
// pointer-chase set is 41%, and everything else is ~1.7%. So a flat evaluator over a small
// opcode set covers essentially all traffic, and anything it does not cover marks the program
// unusable and falls back to the interpreter with no behaviour change.
// ---------------------------------------------------------------------------------------
constexpr uint32_t kNoSlot = UINT32_MAX;
// DescriptorSource::dwords is std::array<Value, 8> - an image or sampler descriptor is 8 dwords.
// Hardcoding 4 here truncated every one of them and zeroed the upper half, which surfaced as
// "unsupported storage texture ... dwords=...,00000000,00000000,00000000,00000000".
constexpr uint32_t kMaxDescriptorDwords = 8;

class LinearCompiler final {
public:
	explicit LinearCompiler(ResourcePlan& program): m_program(program) {}

	void Run() {
		auto& out = m_program.srt_linear;
		out = {};
		// The conditional-source CFG and the clean/specialization evaluator both change which
		// nodes are evaluated per draw; the flat program evaluates a fixed set, so it only
		// applies when neither is in play. SrtShape measures that at ~78% of in-fight calls.
		if (!m_program.control_flow.empty() ||
		    std::ranges::any_of(m_program.clean_flat_slots, [](uint8_t c) { return c != 0u; })) {
			return;
		}

		out.read_slots.assign(m_program.srt_reads.size(), kNoSlot);
		out.source_slots.assign(m_program.descriptor_sources.size() * kMaxDescriptorDwords, kNoSlot);

		for (uint32_t i = 0; i < m_program.srt_reads.size(); i++) {
			const auto slot = Compile(m_program.srt_reads[i].value);
			if (slot == kNoSlot) {
				out = {};
				return;
			}
			out.read_slots[i] = slot;
		}
		for (uint32_t i = 0; i < m_program.descriptor_sources.size(); i++) {
			const auto& source = m_program.descriptor_sources[i];
			for (uint32_t d = 0; d < source.dword_count && d < kMaxDescriptorDwords; d++) {
				const auto slot = Compile(source.dwords[d]);
				if (slot == kNoSlot) {
					out = {};
					return;
				}
				out.source_slots[i * kMaxDescriptorDwords + d] = slot;
			}
		}
		out.usable = true;
	}

private:
	uint32_t Emit(ValueOpcode opcode, std::span<const uint32_t> operands, uint64_t immediate) {
		auto&        out = m_program.srt_linear;
		SrtLinearOp  op;
		op.opcode        = opcode;
		op.immediate     = immediate;
		op.operand_begin = static_cast<uint32_t>(out.operands.size());
		op.operand_count = static_cast<uint32_t>(operands.size());
		out.operands.insert(out.operands.end(), operands.begin(), operands.end());
		out.ops.push_back(op);
		return static_cast<uint32_t>(out.ops.size() - 1u);
	}

	uint32_t EmitConstant(uint64_t value) { return Emit(ValueOpcode::Void, {}, value); }

	// Returns the slot holding `value`, or kNoSlot if anything in its subtree is unsupported.
	uint32_t Compile(Value value) {
		value = value.Resolve();
		if (value.IsImmediate()) {
			switch (value.GetType()) {
				case Type::U1: return EmitConstant(value.U1());
				case Type::U8: return EmitConstant(value.U8());
				case Type::U16: return EmitConstant(value.U16());
				case Type::U32: return EmitConstant(value.U32());
				case Type::U64: return EmitConstant(value.U64());
				case Type::F32: return EmitConstant(std::bit_cast<uint32_t>(value.F32Value()));
				default: return kNoSlot;
			}
		}
		auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			return kNoSlot;
		}
		if (const auto found = m_slots.find(inst); found != m_slots.end()) {
			return found->second;
		}
		// Depth guard: a cycle would otherwise recurse forever. The interpreter uses a visiting
		// stack for this; here an in-progress marker is enough because we never revisit.
		if (!m_active.insert(inst).second) {
			return kNoSlot;
		}
		const auto slot = CompileInst(*inst);
		m_active.erase(inst);
		if (slot != kNoSlot) {
			m_slots.emplace(inst, slot);
		}
		return slot;
	}

	uint32_t CompileArgs(const Inst& inst, size_t count, ValueOpcode opcode, uint64_t immediate) {
		std::array<uint32_t, 5> slots {};
		if (inst.NumArgs() < count || count > slots.size()) {
			return kNoSlot;
		}
		for (size_t i = 0; i < count; i++) {
			slots[i] = Compile(inst.Arg(i));
			if (slots[i] == kNoSlot) {
				return kNoSlot;
			}
		}
		return Emit(opcode, std::span {slots.data(), count}, immediate);
	}

	uint32_t CompileInst(const Inst& inst) {
		const auto opcode = inst.GetOpcode();
		switch (opcode) {
			case ValueOpcode::GetUserData: {
				const auto reg = RegIndex(inst.Arg(0).ScalarRegister());
				if (reg < m_program.user_data_base) {
					return kNoSlot;
				}
				// The bound check against runtime.user_data.size() stays at evaluation time.
				return Emit(ValueOpcode::GetUserData, {}, reg - m_program.user_data_base);
			}
			case ValueOpcode::GetShaderBase: return Emit(opcode, {}, 0);
			case ValueOpcode::Phi: {
				const auto resolved = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
				return resolved.IsEmpty() ? kNoSlot : Compile(resolved);
			}
			case ValueOpcode::ReadConst: {
				// Resolves at compile time to another node in this same DAG.
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size()) {
					return kNoSlot;
				}
				return Compile(m_program.srt_reads[slot.U32()].value);
			}
			case ValueOpcode::LoadAddressU32: {
				// 57% of all evaluated nodes. EvaluateRawRead reaches THROUGH the handle to its
				// low/high args rather than evaluating the handle itself, so mirror that here.
				if (!IsRawRead(m_program, inst)) {
					return kNoSlot;
				}
				const auto flags = inst.Flags<MemoryFlags>();
				if (flags.index >= m_program.memory_info.size()) {
					return kNoSlot;
				}
				const auto* handle = inst.Arg(0).ResolveInstruction();
				if (handle == nullptr || handle->NumArgs() < 2u || inst.NumArgs() < 2u) {
					return kNoSlot;
				}
				const std::array<uint32_t, 3> slots {Compile(handle->Arg(0)), Compile(handle->Arg(1)),
				                                     Compile(inst.Arg(1))};
				if (std::ranges::find(slots, kNoSlot) != slots.end()) {
					return kNoSlot;
				}
				return Emit(opcode, slots, flags.index);
			}
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32: return CompileArgs(inst, 1, ValueOpcode::Identity, 0);
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: {
				const auto index = inst.Arg(1).Resolve();
				if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
					return kNoSlot;
				}
				const auto component = index.U32();
				if (opcode == ValueOpcode::CompositeExtractU64) {
					const auto packed = Compile(inst.Arg(0));
					return packed == kNoSlot
					           ? kNoSlot
					           : Emit(ValueOpcode::CompositeExtractU64, {&packed, 1}, component);
				}
				// CompositeExtractU32x2 of a CompositeConstructU32x2 picks the component's
				// source directly; the interpreter never evaluates the construct node.
				const auto* source = inst.Arg(0).ResolveInstruction();
				if (source == nullptr ||
				    source->GetOpcode() != ValueOpcode::CompositeConstructU32x2 ||
				    source->NumArgs() <= component) {
					return kNoSlot;
				}
				return Compile(source->Arg(component));
			}
			case ValueOpcode::CompositeConstructU64:
			case ValueOpcode::IAdd32:
			case ValueOpcode::IAdd64:
			case ValueOpcode::ISub32:
			case ValueOpcode::ISub64:
			case ValueOpcode::IMul32:
			case ValueOpcode::IMul64:
			case ValueOpcode::UMin32:
			case ValueOpcode::ShiftLeftLogical32:
			case ValueOpcode::ShiftLeftLogical64:
			case ValueOpcode::ShiftRightLogical32:
			case ValueOpcode::ShiftRightLogical64:
			case ValueOpcode::BitwiseAnd32:
			case ValueOpcode::BitwiseAnd64:
			case ValueOpcode::BitwiseOr32:
			case ValueOpcode::ULessThan32:
			case ValueOpcode::IEqual32:
			case ValueOpcode::INotEqual32: return CompileArgs(inst, 2, opcode, 0);
			case ValueOpcode::LogicalNot: return CompileArgs(inst, 1, opcode, 0);
			default: return kNoSlot;
		}
	}

	ResourcePlan&                             m_program;
	std::unordered_map<const Inst*, uint32_t> m_slots;
	std::unordered_set<const Inst*>           m_active;
};

// Evaluate the compiled program into `slots`. Returns false if any op fails, matching the
// interpreter: in the no-CFG case every source is active and every srt_read is required, so a
// failure anywhere fails the whole call either way.
bool RunLinearProgram(const ResourcePlan& program, const SrtRuntime& runtime,
                      std::vector<uint64_t>& slots) {
	const auto& lin = program.srt_linear;
	slots.assign(lin.ops.size(), 0);
	const auto* operands = lin.operands.data();

	for (size_t i = 0; i < lin.ops.size(); i++) {
		const auto& op  = lin.ops[i];
		const auto* arg = operands + op.operand_begin;
		const auto  a   = op.operand_count > 0 ? slots[arg[0]] : uint64_t {0};
		const auto  b   = op.operand_count > 1 ? slots[arg[1]] : uint64_t {0};
		uint64_t    out = 0;
		switch (op.opcode) {
			case ValueOpcode::Void: out = op.immediate; break;
			case ValueOpcode::Identity: out = a; break;
			case ValueOpcode::GetUserData:
				if (op.immediate >= runtime.user_data.size()) {
					return false;
				}
				out = runtime.user_data[op.immediate];
				break;
			case ValueOpcode::GetShaderBase: out = runtime.shader_base; break;
			case ValueOpcode::LoadAddressU32: {
				const auto  base   = ((b << 32u) | static_cast<uint32_t>(a)) & AddressMask;
				const auto& mem    = program.memory_info[op.immediate];
				const auto  offset = slots[arg[2]];
				// Must match EvaluateRawRead exactly: base, the memory_info immediate and the
				// runtime offset are EACH aligned down to 4 bytes independently, not summed and
				// then aligned. Getting this wrong was caught by TestSrtWalkerRealSmemTranslation.
				const auto immediate = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
				const auto relative  = (immediate & ~int64_t {3}) +
				                      static_cast<int64_t>(static_cast<uint32_t>(offset) & ~3u);
				uint64_t address = 0;
				if (!AddSignedAddress(base & ~uint64_t {3}, relative, address)) {
					return false;
				}
				uint32_t   value = 0;
				const auto t0    = Common::Timer::QueryPerformanceCounter();
				bool       ok    = true;
				if (runtime.read_memory != nullptr) {
					ok = runtime.read_memory(runtime.userdata, address, &value);
				} else {
					std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
				}
				NoteGuestRead(Common::Timer::QueryPerformanceCounter() - t0);
				if (!ok) {
					return false;
				}
				out = value;
				break;
			}
			case ValueOpcode::CompositeExtractU64:
				out = static_cast<uint32_t>(a >> (op.immediate * 32u));
				break;
			case ValueOpcode::CompositeConstructU64:
				out = static_cast<uint32_t>(a) |
				      (static_cast<uint64_t>(static_cast<uint32_t>(b)) << 32u);
				break;
			case ValueOpcode::IAdd32: out = static_cast<uint32_t>(a + b); break;
			case ValueOpcode::IAdd64: out = a + b; break;
			case ValueOpcode::ISub32: out = static_cast<uint32_t>(a - b); break;
			case ValueOpcode::ISub64: out = a - b; break;
			case ValueOpcode::IMul32: out = static_cast<uint32_t>(a * b); break;
			case ValueOpcode::IMul64: out = a * b; break;
			case ValueOpcode::UMin32:
				out = std::min(static_cast<uint32_t>(a), static_cast<uint32_t>(b));
				break;
			case ValueOpcode::ShiftLeftLogical32:
				out = static_cast<uint32_t>(static_cast<uint32_t>(a) << (b & 31u));
				break;
			case ValueOpcode::ShiftLeftLogical64: out = a << (b & 63u); break;
			case ValueOpcode::ShiftRightLogical32:
				out = static_cast<uint32_t>(a) >> (b & 31u);
				break;
			case ValueOpcode::ShiftRightLogical64: out = a >> (b & 63u); break;
			case ValueOpcode::BitwiseAnd32: out = static_cast<uint32_t>(a) & static_cast<uint32_t>(b); break;
			case ValueOpcode::BitwiseAnd64: out = a & b; break;
			case ValueOpcode::BitwiseOr32: out = static_cast<uint32_t>(a) | static_cast<uint32_t>(b); break;
			case ValueOpcode::ULessThan32:
				out = static_cast<uint32_t>(a) < static_cast<uint32_t>(b) ? 1u : 0u;
				break;
			case ValueOpcode::IEqual32:
				out = static_cast<uint32_t>(a) == static_cast<uint32_t>(b) ? 1u : 0u;
				break;
			case ValueOpcode::INotEqual32:
				out = static_cast<uint32_t>(a) != static_cast<uint32_t>(b) ? 1u : 0u;
				break;
			case ValueOpcode::LogicalNot: out = (a & 1u) != 0u ? 0u : 1u; break;
			default: return false;
		}
		slots[i] = out;
	}
	return true;
}

// Per-call cost of the flat path, reported next to SrtEval so the two are directly comparable.
std::atomic<uint64_t> g_linear_ticks {0};
std::atomic<uint32_t> g_linear_calls {0};

void NoteLinearCall(uint64_t ticks) {
	g_linear_ticks.fetch_add(ticks, std::memory_order_relaxed);
	g_linear_calls.fetch_add(1, std::memory_order_relaxed);
}

bool LinearPathEnabled() {
	static const int mode = [] {
		const char* env = std::getenv("KYTY_SRT_LINEAR");
		if (env == nullptr || env[0] == '\0' || env[0] == '0') {
			return 0;
		}
		return env[0] == 'v' ? 2 : 1; // "verify" runs both and compares
	}();
	return mode != 0;
}

bool LinearPathVerify() {
	static const bool verify = [] {
		const char* env = std::getenv("KYTY_SRT_LINEAR");
		return env != nullptr && env[0] == 'v';
	}();
	return verify;
}

const DescriptorSource* Source(const ResourcePlan& program, uint32_t source) {
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
}

bool EvaluateRuntimeSourcesImpl(const ResourcePlan& program, std::span<const uint32_t> sources,
                                const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                                std::vector<uint32_t>& flat, bool evaluate_flat,
                                std::span<const uint8_t> clean_flat_slots,
                                std::vector<uint8_t>& active_sources) {
	if (!program.srt_plan_complete) {
		return false;
	}
	if (std::ranges::any_of(clean_flat_slots, [](uint8_t clean) { return clean != 0u; }) &&
	    runtime.read_specialization_memory == nullptr) {
		return false;
	}
	// Before porting shadPS4's Xbyak SRT walker, establish what fraction of draws it could
	// actually compile. Their JIT handles a straight pointer chase; Kyty's evaluator also has
	// (a) a CFG walk that activates sources conditionally and (b) a second "clean" evaluator
	// reading specialization memory. A draw needing either is not a candidate, and a JIT that
	// only covers a small minority is not worth the plumbing - the same mistake the previous
	// four optimisation attempts made by assuming the expensive path was the common one.
	{
		const bool has_cfg = !program.control_flow.empty();
		const bool has_clean =
		    std::ranges::any_of(clean_flat_slots, [](uint8_t clean) { return clean != 0u; });
		static std::atomic<uint32_t> calls {0};
		static std::atomic<uint32_t> cfg_calls {0}, clean_calls {0}, simple_calls {0};
		static std::atomic<uint64_t> srcs {0}, reads {0}, blocks {0};
		if (has_cfg) {
			cfg_calls.fetch_add(1, std::memory_order_relaxed);
		}
		if (has_clean) {
			clean_calls.fetch_add(1, std::memory_order_relaxed);
		}
		if (!has_cfg && !has_clean) {
			simple_calls.fetch_add(1, std::memory_order_relaxed);
		}
		srcs.fetch_add(sources.size(), std::memory_order_relaxed);
		reads.fetch_add(program.srt_reads.size(), std::memory_order_relaxed);
		blocks.fetch_add(program.control_flow.size(), std::memory_order_relaxed);
		if ((calls.fetch_add(1, std::memory_order_relaxed) % 8192) == 8191) {
			LOGF("SrtShape/8192: cfg=%u clean=%u jittable=%u | avg_srcs=%.1f avg_reads=%.1f "
			     "avg_blocks=%.1f\n",
			     cfg_calls.exchange(0), clean_calls.exchange(0), simple_calls.exchange(0),
			     srcs.exchange(0) / 8192.0, reads.exchange(0) / 8192.0,
			     blocks.exchange(0) / 8192.0);
		}
	}

	// Flat path: compiled DAG, no recursion, no hash lookups, no cycle check. Only valid when
	// the program compiled (no CFG-conditional sources, no clean slots) and we are doing the
	// full flat evaluation. KYTY_SRT_LINEAR=1 enables it, =verify runs both and compares.
	std::vector<DescriptorValue> verify_results;
	std::vector<uint32_t>        verify_flat;
	bool                         verify_active = false;
	if (evaluate_flat && program.srt_linear.usable && LinearPathEnabled()) {
		const auto  lin_t0 = Common::Timer::QueryPerformanceCounter();
		const auto& lin    = program.srt_linear;
		static thread_local std::vector<uint64_t> slots;
		if (RunLinearProgram(program, runtime, slots)) {
			std::vector<DescriptorValue> lin_results;
			lin_results.reserve(sources.size());
			bool ok = true;
			for (const auto source_index: sources) {
				const auto* source = Source(program, source_index);
				if (source == nullptr ||
				    source_index * kMaxDescriptorDwords >= lin.source_slots.size()) {
					ok = false;
					break;
				}
				DescriptorValue value;
				value.dword_count = source->dword_count;
				for (uint32_t d = 0; d < source->dword_count && d < kMaxDescriptorDwords; d++) {
					const auto slot = lin.source_slots[source_index * kMaxDescriptorDwords + d];
					if (slot == kNoSlot) {
						ok = false;
						break;
					}
					value.dwords[d] = static_cast<uint32_t>(slots[slot]);
				}
				lin_results.push_back(value);
			}
			std::vector<uint32_t> lin_flat;
			if (ok) {
				lin_flat.resize(program.srt_reads.size());
				for (size_t i = 0; i < program.srt_reads.size(); i++) {
					const auto slot = lin.read_slots[i];
					const auto off  = program.srt_reads[i].flat_offset;
					if (slot == kNoSlot || off >= lin_flat.size()) {
						ok = false;
						break;
					}
					lin_flat[off] = static_cast<uint32_t>(slots[slot]);
				}
			}
			if (ok) {
				NoteLinearCall(Common::Timer::QueryPerformanceCounter() - lin_t0);
				if (!LinearPathVerify()) {
					results = std::move(lin_results);
					active_sources.assign(program.descriptor_sources.size(), 1u);
					flat    = std::move(lin_flat);
					return true;
				}
				verify_results = std::move(lin_results);
				verify_flat    = std::move(lin_flat);
				verify_active  = true;
			}
		}
	}

	const auto prof_t0        = Common::Timer::QueryPerformanceCounter();
	SrtRuntime clean_runtime  = runtime;
	clean_runtime.read_memory = runtime.read_specialization_memory;
	Evaluator            clean_evaluator(program, clean_runtime);
	Evaluator            evaluator(program, runtime, clean_flat_slots, &clean_evaluator);
	std::vector<uint8_t> active;
	if (evaluate_flat) {
		active.assign(program.descriptor_sources.size(), 1u);
	}
	const auto prof_t1 = Common::Timer::QueryPerformanceCounter();
	if (evaluate_flat && !program.control_flow.empty()) {
		for (const auto& block: program.control_flow) {
			for (const auto source: block.sources) {
				active.at(source) = 0u;
			}
		}
		std::vector<uint8_t>  visited(program.control_flow.size());
		std::vector<uint32_t> pending {0};
		while (!pending.empty()) {
			const auto index = pending.back();
			pending.pop_back();
			if (visited.at(index)) {
				continue;
			}
			visited[index]    = 1u;
			const auto& block = program.control_flow[index];
			for (const auto source: block.sources) {
				active[source] = 1u;
			}
			uint32_t condition = 0;
			// A missing clean reader must never fall through to the evaluator's raw-memory path.
			if (!block.condition.IsEmpty() && runtime.read_specialization_memory != nullptr &&
			    clean_evaluator.Evaluate(block.condition, condition)) {
				pending.push_back(block.successors[condition != 0u ? 0u : 1u]);
			} else {
				pending.insert(pending.end(), block.successors.begin(), block.successors.end());
			}
		}
	}
	const auto                   prof_t2 = Common::Timer::QueryPerformanceCounter();
	std::vector<DescriptorValue> evaluated;
	evaluated.reserve(sources.size());
	for (const auto source_index: sources) {
		const auto* source = Source(program, source_index);
		if (source == nullptr) {
			return false;
		}
		DescriptorValue value;
		value.dword_count = source->dword_count;
		if (!evaluate_flat || active[source_index]) {
			for (uint32_t index = 0; index < source->dword_count; index++) {
				if (!evaluator.Evaluate(source->dwords[index], value.dwords[index])) {
					return false;
				}
			}
		}
		evaluated.push_back(value);
	}
	const auto            prof_t3 = Common::Timer::QueryPerformanceCounter();
	std::vector<uint32_t> flattened;
	if (evaluate_flat) {
		flattened.resize(program.srt_reads.size());
		for (const auto& read: program.srt_reads) {
			const bool clean    = read.flat_offset < clean_flat_slots.size() &&
			                      clean_flat_slots[read.flat_offset] != 0u;
			auto&      selected = clean ? clean_evaluator : evaluator;
			if (read.flat_offset >= flattened.size() ||
			    !selected.Evaluate(read.value, flattened[read.flat_offset])) {
				return false;
			}
		}
	}
	const auto prof_t4 = Common::Timer::QueryPerformanceCounter();
	if (evaluate_flat) {
		// Per-call cost split. None of these scale with the handful of bound resources:
		// setup builds two Evaluators, cfg walks the whole control-flow graph, and srt
		// re-evaluates the entire flattened SRT table from guest memory every draw.
		static std::atomic<uint32_t> count {0}, cfg_n {0}, srt_n {0}, src_n {0};
		static std::atomic<uint64_t> setup_ns {0}, cfgw_ns {0}, srcs_ns {0}, srtw_ns {0};
		const auto                   freq = Common::Timer::QueryPerformanceFrequency();
		const auto ns = [freq](uint64_t a, uint64_t b) {
			return freq == 0 ? 0ull : (b - a) * 1000000000ull / freq;
		};
		setup_ns.fetch_add(ns(prof_t0, prof_t1), std::memory_order_relaxed);
		cfgw_ns.fetch_add(ns(prof_t1, prof_t2), std::memory_order_relaxed);
		srcs_ns.fetch_add(ns(prof_t2, prof_t3), std::memory_order_relaxed);
		srtw_ns.fetch_add(ns(prof_t3, prof_t4), std::memory_order_relaxed);
		cfg_n.fetch_add(static_cast<uint32_t>(program.control_flow.size()),
		                std::memory_order_relaxed);
		srt_n.fetch_add(static_cast<uint32_t>(program.srt_reads.size()),
		                std::memory_order_relaxed);
		src_n.fetch_add(static_cast<uint32_t>(program.descriptor_sources.size()),
		                std::memory_order_relaxed);
		if ((count.fetch_add(1, std::memory_order_relaxed) % 8192) == 8191) {
			LOGF("SrtEval/8192: setup=%.2fus cfg=%.2fus sources=%.2fus srtreads=%.2fus | "
			     "avg_cfg=%.1f avg_srtreads=%.1f avg_srcs=%.1f\n",
			     setup_ns.exchange(0) / 8192.0 / 1000.0, cfgw_ns.exchange(0) / 8192.0 / 1000.0,
			     srcs_ns.exchange(0) / 8192.0 / 1000.0, srtw_ns.exchange(0) / 8192.0 / 1000.0,
			     cfg_n.exchange(0) / 8192.0, srt_n.exchange(0) / 8192.0,
			     src_n.exchange(0) / 8192.0);
			const auto read_ticks = g_guest_read_ticks.exchange(0);
			const auto read_count = g_guest_read_count.exchange(0);
			const auto lin_ticks = g_linear_ticks.exchange(0);
			const auto lin_calls = g_linear_calls.exchange(0);
			LOGF("SrtLinear/8192: calls=%u linear=%.2fus/call\n", lin_calls,
			     (freq == 0 || lin_calls == 0)
			         ? 0.0
			         : lin_ticks * 1000000.0 / freq / static_cast<double>(lin_calls));
			LOGF("SrtReads/8192: guest_read=%.2fus/call reads=%.1f/call ns_per_read=%.0f\n",
			     freq == 0 ? 0.0 : read_ticks * 1000000.0 / freq / 8192.0, read_count / 8192.0,
			     (freq == 0 || read_count == 0)
			         ? 0.0
			         : read_ticks * 1000000000.0 / freq / static_cast<double>(read_count));
		}
	}
	// KYTY_SRT_LINEAR=verify: both paths ran; the interpreter's answer is authoritative and is
	// what gets returned, so a mismatch is reported without changing behaviour.
	if (verify_active) {
		bool same = verify_results.size() == evaluated.size() && verify_flat == flattened;
		for (size_t i = 0; same && i < evaluated.size(); i++) {
			same = verify_results[i].dword_count == evaluated[i].dword_count;
			for (uint32_t d = 0;
			     same && d < evaluated[i].dword_count && d < kMaxDescriptorDwords; d++) {
				same = verify_results[i].dwords[d] == evaluated[i].dwords[d];
			}
		}
		static std::atomic<uint32_t> checked {0}, mismatched {0};
		checked.fetch_add(1, std::memory_order_relaxed);
		if (!same) {
			mismatched.fetch_add(1, std::memory_order_relaxed);
		}
		if ((checked.load(std::memory_order_relaxed) % 8192) == 0) {
			LOGF("SrtLinearVerify/8192: mismatched=%u (hash=0x%016" PRIx64 ")\n",
			     mismatched.exchange(0), program.shader_hash);
		}
	}

	results = std::move(evaluated);
	active_sources = std::move(active);
	if (evaluate_flat) {
		flat = std::move(flattened);
	}
	return true;
}

} // namespace

bool ValidateRuntimeValue(const ResourcePlan& program, Value value, RuntimeValueType type) {
	return RuntimeValidator(program, type).Run(value);
}

void BuildSrtPlan(Program& program) {
	if (program.resource_tracking_complete) {
		EXIT("shader SRT planning failed: cannot rebuild SRT after resource tracking");
	}
	program.srt_plan_complete = false;
	PlanBuilder(program).Run();
	// Compile the value DAG to a flat op array once, here, where it costs nothing per draw.
	// ExtractResourcePlan() must do the same for its clone - see BuildSrtLinearProgram.
	LinearCompiler(program).Run();
	program.srt_plan_complete = true;
}

void BuildSrtLinearProgram(ResourcePlan& plan) { LinearCompiler(plan).Run(); }

bool EvaluateUniformValues(const ResourcePlan& program, std::span<const Value> values,
                            const SrtRuntime& runtime, std::span<uint32_t> results) {
	if (values.size() != results.size()) {
		return false;
	}
	auto clean = runtime;
	clean.read_memory = runtime.read_specialization_memory != nullptr
	                        ? runtime.read_specialization_memory
	                        : +[](void*, uint64_t, uint32_t*) { return false; };
	Evaluator evaluator(program, clean);
	for (size_t i = 0; i < values.size(); ++i) {
		if (!evaluator.Evaluate(values[i], results[i])) {
			return false;
		}
	}
	return true;
}

bool EvaluateDescriptorSource(const ResourcePlan& program, uint32_t source,
                              const SrtRuntime& runtime, DescriptorValue& result) {
	std::vector<DescriptorValue> results;
	if (!EvaluateDescriptorSources(program, std::span {&source, 1}, runtime, results)) {
		return false;
	}
	result = results.front();
	return true;
}

bool EvaluateDescriptorSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                               const SrtRuntime& runtime, std::vector<DescriptorValue>& results) {
	std::vector<uint32_t> ignored;
	std::vector<uint8_t>  active;
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, ignored, false, {},
	                                  active);
}

bool EvaluateRuntimeSources(const ResourcePlan& program, std::span<const uint32_t> sources,
                            const SrtRuntime& runtime, std::vector<DescriptorValue>& results,
                            std::vector<uint32_t>& flat, std::span<const uint8_t> clean_flat_slots,
                            std::vector<uint8_t>& active_sources) {
	return EvaluateRuntimeSourcesImpl(program, sources, runtime, results, flat, true,
	                                  clean_flat_slots, active_sources);
}

bool WalkSrt(const ResourcePlan& program, const SrtRuntime& runtime, std::vector<uint32_t>& flat) {
	std::vector<DescriptorValue> ignored;
	std::vector<uint8_t>         active;
	return EvaluateRuntimeSources(program, {}, runtime, ignored, flat, {}, active);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
