#include "graphics/host_gpu/renderer/gpuTimestamps.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <utility>

namespace Libs::Graphics {

GpuTimestamps& GpuTimestamps::Instance() {
	static GpuTimestamps timestamps;
	return timestamps;
}

bool GpuTimestamps::Enabled() {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_GPU_TIMESTAMPS");
		return value != nullptr && std::strcmp(value, "0") != 0;
	}();
	return enabled;
}

void GpuTimestamps::NoteFrame(GraphicContext& ctx, uint32_t frame) {
	if (!Enabled() || frame == m_frame) {
		return;
	}
	m_frame = frame;
	switch (m_state) {
		case State::Recording:
			// Stop opening new regions at the frame boundary; the submissions still have to
			// retire before the results can be read. Deliberately keep m_pending_* so a
			// region opened in this window still writes its end timestamp - its slot is
			// already allocated, and dropping it would leave a permanent hole in the pool.
			m_state = State::Reading;
			return;
		case State::Reading: Resolve(ctx); return;
		case State::Idle: Arm(ctx); return;
	}
}

bool GpuTimestamps::Arm(GraphicContext& ctx) {
	if (m_pool == nullptr) {
		if (ctx.physical_device_properties.limits.timestampComputeAndGraphics == VK_FALSE) {
			LOGF("GpuTimestamps: device reports no compute/graphics timestamp support\n");
			return false;
		}
		vk::QueryPoolCreateInfo info {};
		info.queryType    = vk::QueryType::eTimestamp;
		info.queryCount   = kSlots;
		const auto result = ctx.device.createQueryPool(&info, nullptr, &m_pool);
		if (result != vk::Result::eSuccess) {
			LOGF("GpuTimestamps: createQueryPool failed (%d)\n", static_cast<int>(result));
			m_pool = nullptr;
			return false;
		}
		m_period = ctx.physical_device_properties.limits.timestampPeriod;
	}
	ctx.device.resetQueryPool(m_pool, 0, kSlots);
	m_next             = 0;
	m_pending_dispatch = UINT32_MAX;
	m_pending_buffers.clear();
	m_entries.clear();
	m_poll_attempts = 0;
	m_state         = State::Recording;
	return true;
}

static uint32_t DrawSampleRate() {
	static const uint32_t rate = [] {
		const char* value = std::getenv("KYTY_GPU_TIMESTAMP_DRAWS");
		if (value == nullptr) {
			return 16u;
		}
		const auto parsed = std::strtoul(value, nullptr, 10);
		return parsed == 0 ? 0u : static_cast<uint32_t>(parsed);
	}();
	return rate;
}

uint32_t GpuTimestamps::Open(vk::CommandBuffer buffer, Kind kind, uint64_t hash, uint32_t wave,
                             uint64_t hash2) {
	if (!Enabled() || m_state != State::Recording || m_pool == nullptr || buffer == nullptr ||
	    m_next + 2 > kSlots) {
		return UINT32_MAX;
	}
	buffer.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, m_pool, m_next);
	const auto index = static_cast<uint32_t>(m_entries.size());
	m_entries.push_back({hash, hash2, wave, m_next, kind});
	m_next += 2;
	return index;
}

void GpuTimestamps::Close(vk::CommandBuffer buffer, uint32_t entry_index) {
	if (entry_index >= m_entries.size() || m_pool == nullptr || buffer == nullptr) {
		return;
	}
	buffer.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, m_pool,
	                       m_entries[entry_index].begin + 1);
}

void GpuTimestamps::BeginDispatch(vk::CommandBuffer buffer, uint64_t hash, uint32_t wave) {
	m_pending_dispatch = Open(buffer, Kind::Dispatch, hash, wave);
}

void GpuTimestamps::EndDispatch(vk::CommandBuffer buffer) {
	if (m_pending_dispatch == UINT32_MAX) {
		return;
	}
	Close(buffer, m_pending_dispatch);
	m_pending_dispatch = UINT32_MAX;
}

// Draws binding a watched pixel shader are ALWAYS bracketed; everything else is sampled.
// 1-in-16 sampling on draws this rare gave per-frame extrapolations from 214ms to 1284ms -
// the per-draw cost was solid but the total share was not. Defaults to the three UFC
// ubershaders that fall back to DispatcherFull; override with a comma-separated hex list.
bool IsWatchedDrawPixelShader(uint64_t hash) {
	static const std::vector<uint64_t> watched = [] {
		const char* env = std::getenv("KYTY_GPU_TIMESTAMP_PS");
		if (env == nullptr) {
			return std::vector<uint64_t> {0x1bcc68ffb7b0469eull, 0xb808b3887f76ff0cull,
			                              0xf7030726b9470dd8ull};
		}
		std::vector<uint64_t> list;
		const char*           cursor = env;
		while (*cursor != 0) {
			while (*cursor == ',' || *cursor == ' ') {
				++cursor;
			}
			if (*cursor == 0) {
				break;
			}
			char*      end   = nullptr;
			const auto value = std::strtoull(cursor, &end, 16);
			if (end == cursor) {
				break;
			}
			list.push_back(value);
			cursor = end;
		}
		return list;
	}();
	return std::find(watched.begin(), watched.end(), hash) != watched.end();
}

void GpuTimestamps::BeginDraw(vk::CommandBuffer buffer, uint64_t vs_hash, uint64_t ps_hash) {
	const auto rate    = DrawSampleRate();
	const bool watched = ps_hash != 0 && IsWatchedDrawPixelShader(ps_hash);
	const bool sampled = rate != 0 && (m_draw_counter % rate) == 0;
	m_draw_counter++;
	if (!watched && !sampled) {
		m_pending_draw = UINT32_MAX;
		return;
	}
	m_pending_draw = Open(buffer, Kind::Draw, ps_hash, watched ? 1u : 0u, vs_hash);
}

void GpuTimestamps::EndDraw(vk::CommandBuffer buffer) {
	if (m_pending_draw == UINT32_MAX) {
		return;
	}
	Close(buffer, m_pending_draw);
	m_pending_draw = UINT32_MAX;
}

void GpuTimestamps::BeginCommandBuffer(vk::CommandBuffer buffer) {
	const auto index = Open(buffer, Kind::CommandBuffer, 0, 0);
	if (index != UINT32_MAX) {
		m_pending_buffers.push_back(index);
	}
}

void GpuTimestamps::EndCommandBuffer(vk::CommandBuffer buffer) {
	if (m_pending_buffers.empty()) {
		return;
	}
	Close(buffer, m_pending_buffers.back());
	m_pending_buffers.pop_back();
}

void GpuTimestamps::Resolve(GraphicContext& ctx) {
	if (m_pool == nullptr || m_next < 2) {
		m_state = State::Idle;
		return;
	}
	// Availability per query, not just the values. Without it a single never-written slot -
	// a region opened just before the window closed, or a command buffer that was reset
	// rather than submitted - makes the whole range return eNotReady forever and the
	// collector wedges silently. With it, holes are simply skipped.
	std::vector<uint64_t> values(static_cast<size_t>(m_next) * 2);
	const auto            result = ctx.device.getQueryPoolResults(
        m_pool, 0, m_next, values.size() * sizeof(uint64_t), values.data(), sizeof(uint64_t) * 2,
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
	if (result == vk::Result::eNotReady) {
		if (++m_poll_attempts < 4) {
			return; // genuinely still in flight; poll again next frame
		}
		// eWithAvailability still reports NotReady when anything is missing, but the values
		// it did fill are valid. After a few frames everything submitted has retired, so
		// stop waiting and use whatever is marked available.
	} else if (result != vk::Result::eSuccess) {
		LOGF("GpuTimestamps: getQueryPoolResults failed (%d)\n", static_cast<int>(result));
		m_state = State::Idle;
		return;
	}

	struct Total {
		double   ms    = 0.0;
		uint32_t count = 0;
		uint32_t wave  = 0;
	};
	std::unordered_map<uint64_t, Total> shaders;
	std::unordered_map<uint64_t, Total> draw_shaders;
	double                              draw_ms      = 0.0;
	uint32_t                            draw_samples = 0;
	double                              draw_max     = 0.0;
	double                              watched_ms   = 0.0;
	uint32_t                            watched_draws = 0;
	double                              dispatch_ms  = 0.0;
	double                              wave64_ms    = 0.0;
	double                              buffer_ms    = 0.0;
	uint32_t                            dispatches   = 0;
	uint32_t                            buffer_count = 0;

	for (const auto& entry: m_entries) {
		if (entry.begin + 1 >= m_next) {
			continue;
		}
		const auto begin_available = values[(static_cast<size_t>(entry.begin) * 2) + 1] != 0;
		const auto end_available   = values[((static_cast<size_t>(entry.begin) + 1) * 2) + 1] != 0;
		if (!begin_available || !end_available) {
			continue; // region never completed inside this window
		}
		const auto begin = values[static_cast<size_t>(entry.begin) * 2];
		const auto end   = values[(static_cast<size_t>(entry.begin) + 1) * 2];
		if (end < begin) {
			continue;
		}
		const auto ms = static_cast<double>(end - begin) * m_period / 1000000.0;
		if (entry.kind == Kind::CommandBuffer) {
			buffer_ms += ms;
			buffer_count++;
			continue;
		}
		if (entry.kind == Kind::Draw) {
			if (entry.wave != 0) {
				watched_ms += ms;
				watched_draws++;
			} else {
				draw_ms += ms;
				draw_samples++;
			}
			draw_max    = std::max(draw_max, ms);
			auto& dslot = draw_shaders[entry.hash];
			dslot.ms += ms;
			dslot.count++;
			dslot.wave = entry.wave;
			continue;
		}
		dispatch_ms += ms;
		dispatches++;
		if (entry.wave == 64u) {
			wave64_ms += ms;
		}
		auto& slot = shaders[entry.hash];
		slot.ms += ms;
		slot.count++;
		slot.wave = entry.wave;
	}

	LOGF("GpuBusy: cmdbuffers=%.1fms over %u buffers | dispatches=%.1fms over %u "
	     "(wave64=%.1fms) | graphics=%.1fms\n",
	     buffer_ms, buffer_count, dispatch_ms, dispatches, wave64_ms,
	     std::max(buffer_ms - dispatch_ms, 0.0));

	if (draw_samples != 0) {
		LOGF("GpuDraws: watched=%u draws %.2fms (%.1fus each) | sampled=%u every %u %.2fms "
		     "(%.1fus each) | max=%.2fms\n",
		     watched_draws, watched_ms,
		     watched_draws == 0 ? 0.0 : watched_ms * 1000.0 / watched_draws, draw_samples,
		     DrawSampleRate(), draw_ms,
		     draw_samples == 0 ? 0.0 : draw_ms * 1000.0 / draw_samples, draw_max);
		std::vector<std::pair<uint64_t, Total>> draws(draw_shaders.begin(), draw_shaders.end());
		std::sort(draws.begin(), draws.end(),
		          [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
		for (size_t i = 0; i < draws.size() && i < 6; i++) {
			LOGF("  ps=0x%016" PRIx64 " %.2fms over %u sampled draws (%.1fus each)\n",
			     draws[i].first, draws[i].second.ms, draws[i].second.count,
			     draws[i].second.ms * 1000.0 / std::max(draws[i].second.count, 1u));
		}
	}

	std::vector<std::pair<uint64_t, Total>> ranked(shaders.begin(), shaders.end());
	std::sort(ranked.begin(), ranked.end(),
	          [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
	for (size_t i = 0; i < ranked.size() && i < 6; i++) {
		LOGF("  cs=0x%016" PRIx64 " wave=%u %.2fms over %u dispatches (%.3fms each)\n",
		     ranked[i].first, ranked[i].second.wave, ranked[i].second.ms, ranked[i].second.count,
		     ranked[i].second.ms / std::max(ranked[i].second.count, 1u));
	}
	m_poll_attempts = 0;
	m_state         = State::Idle;
}

} // namespace Libs::Graphics
