#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;

// Real GPU attribution. Everything else in the profile measures the CPU, so GPU cost has only
// ever been inferred from how long the CPU blocks - which produced a wrong answer once already
// (see the ledger: wave64 was "the ceiling" until this measured it at <5% of the frame).
//
// Two rules learned the hard way:
//  - Bracket every region with its OWN begin/end pair. Diffing consecutive single timestamps
//    measures gap+execution, so GPU idle gets charged to whatever ran before it.
//  - Sanity-check any GPU total against wall time. A total exceeding the frame is the giveaway.
//
// The pool is host-reset (hostQueryReset) only between "results fully read back" and "arm
// again", so it is never reset while a submission still references it.
// KYTY_GPU_TIMESTAMPS=1 to enable; off by default.
class GpuTimestamps {
public:
	static GpuTimestamps& Instance();
	[[nodiscard]] static bool Enabled();

	// Drives the arm -> record -> resolve cycle. Safe to call on every dispatch/command buffer.
	void NoteFrame(GraphicContext& ctx, uint32_t frame);

	void BeginDispatch(vk::CommandBuffer buffer, uint64_t hash, uint32_t wave);
	void EndDispatch(vk::CommandBuffer buffer);

	// Draws are sampled (KYTY_GPU_TIMESTAMP_DRAWS, default every 16th). Bracketing all ~3700
	// draws would add 7400 full-pipeline timestamp writes, which serialise the very frame
	// being measured. A sample is enough to separate "all draws are slow" from "a few draws
	// are catastrophic", which is what distinguishes bad shader codegen from garbage
	// indirect-draw parameters.
	void BeginDraw(vk::CommandBuffer buffer, uint64_t vs_hash, uint64_t ps_hash);
	void EndDraw(vk::CommandBuffer buffer);

	// Brackets a whole command buffer, so the total covers draws as well as dispatches.
	// Subtracting the dispatch total isolates graphics work.
	void BeginCommandBuffer(vk::CommandBuffer buffer);
	void EndCommandBuffer(vk::CommandBuffer buffer);

private:
	enum class Kind : uint8_t { Dispatch, CommandBuffer, Draw };
	enum class State : uint8_t { Idle, Recording, Reading };

	struct Entry {
		uint64_t hash  = 0;   // dispatch: cs hash. draw: ps hash (0 when no pixel stage)
		uint64_t hash2 = 0;   // draw: vs hash
		uint32_t wave  = 0;
		uint32_t begin = 0;
		Kind     kind  = Kind::Dispatch;
	};

	static constexpr uint32_t kSlots = 8192;

	bool Arm(GraphicContext& ctx);
	void Resolve(GraphicContext& ctx);
	// Returns the entry index, or UINT32_MAX when the pool is exhausted or not recording.
	uint32_t Open(vk::CommandBuffer buffer, Kind kind, uint64_t hash, uint32_t wave,
	              uint64_t hash2 = 0);
	void     Close(vk::CommandBuffer buffer, uint32_t entry_index);

	vk::QueryPool         m_pool             = nullptr;
	float                 m_period           = 1.0F;
	uint32_t              m_next             = 0;
	uint32_t              m_frame            = UINT32_MAX;
	State                 m_state            = State::Idle;
	uint32_t              m_pending_dispatch = UINT32_MAX;
	uint32_t              m_pending_draw     = UINT32_MAX;
	uint32_t              m_draw_counter     = 0;
	uint32_t              m_poll_attempts    = 0;
	std::vector<uint32_t> m_pending_buffers;
	std::vector<Entry>    m_entries;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMESTAMPS_H_
