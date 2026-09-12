#include "graphics/host_gpu/renderer/sceneDrawDebug.h"

#include "common/logging/log.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Libs::Graphics::SceneDrawDebug {

namespace {

// The draw path reads these once per draw, so they are atomics rather than mutex-guarded state.
// Taking a lock per draw is the mistake that made an earlier texture-GC change 20x worse.
std::atomic<uint32_t> g_sel_start {1};
std::atomic<uint32_t> g_sel_end {0}; // start > end means "nothing selected"
std::atomic<uint32_t> g_mode {static_cast<uint32_t>(Mode::Skip)};
std::atomic<int32_t>  g_cursor {-1}; // step-through position; -1 = nothing isolated
std::atomic<bool>     g_suppress_on {true};
// Per-draw id generation is lock-free. Recording the entry list costs a mutex, so it only happens
// for the single frame after Capture() arms it - the draw path must not take a lock per draw on a
// target that carries thousands of them.
std::atomic<uint32_t> g_seq {0};
std::atomic<uint32_t> g_seq_frame {UINT32_MAX};
std::atomic<bool>     g_recording {false};

std::mutex         g_mutex;
std::vector<Entry> g_current;  // this frame, being filled
std::vector<Entry> g_previous; // last completed frame, for the stability check
std::vector<Entry> g_captured; // frozen list the selection indexes into
uint32_t           g_frame     = UINT32_MAX;
float              g_stability = 0.0f;

uint64_t ParseTarget() {
	const char* env = std::getenv("KYTY_CENSUS_TARGET");
	if (env == nullptr || env[0] == '\0') {
		return 0x1168360000ull;
	}
	if (env[0] == '0' && (env[1] == 'x' || env[1] == 'X')) {
		env += 2;
	}
	return std::strtoull(env, nullptr, 16);
}

// Fraction of ids whose (vs,ps) pair is unchanged from the previous frame. Positional ids are
// only meaningful when this is high; a changed draw count counts against it.
float ComputeStability(const std::vector<Entry>& a, const std::vector<Entry>& b) {
	if (a.empty() || b.empty()) {
		return 0.0f;
	}
	const auto n    = std::min(a.size(), b.size());
	size_t     same = 0;
	for (size_t i = 0; i < n; i++) {
		if (a[i].vs_hash == b[i].vs_hash && a[i].ps_hash == b[i].ps_hash) {
			same++;
		}
	}
	return 100.0f * static_cast<float>(same) / static_cast<float>(std::max(a.size(), b.size()));
}

uint32_t CapturedCount() {
	std::scoped_lock lock {g_mutex};
	return static_cast<uint32_t>(g_captured.size());
}

} // namespace

uint64_t Target() noexcept {
	static const uint64_t target = ParseTarget();
	return target;
}

Mode CurrentMode() noexcept { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }

bool ShouldSuppress(uint32_t id) noexcept {
	const auto start = g_sel_start.load(std::memory_order_relaxed);
	const auto end   = g_sel_end.load(std::memory_order_relaxed);
	return start <= end && id >= start && id <= end;
}

uint32_t NoteDraw(uint32_t frame_num, const Entry& entry) {
	// Lock-free id assignment: reset the sequence on the first draw of a new frame.
	auto seen = g_seq_frame.load(std::memory_order_relaxed);
	if (seen != frame_num &&
	    g_seq_frame.compare_exchange_strong(seen, frame_num, std::memory_order_relaxed)) {
		g_seq.store(0, std::memory_order_relaxed);
		if (g_recording.load(std::memory_order_relaxed)) {
			// One frame of recording is enough; publish it and disarm.
			std::scoped_lock lock {g_mutex};
			g_stability = ComputeStability(g_current, g_previous);
			g_previous  = g_current;
			g_captured  = std::move(g_current);
			g_current.clear();
			g_recording.store(false, std::memory_order_relaxed);
		}
	}
	const auto id = g_seq.fetch_add(1, std::memory_order_relaxed);
	if (g_recording.load(std::memory_order_relaxed)) {
		std::scoped_lock lock {g_mutex};
		g_current.push_back(entry);
	}
	return id;
}

Snapshot Get() noexcept {
	Snapshot s;
	s.sel_start = g_sel_start.load(std::memory_order_relaxed);
	s.sel_end   = g_sel_end.load(std::memory_order_relaxed);
	s.active    = s.sel_start <= s.sel_end;
	s.mode      = CurrentMode();
	std::scoped_lock lock {g_mutex};
	s.captured_draws = static_cast<uint32_t>(g_captured.size());
	s.stability      = g_stability;
	return s;
}

void LogState(const char* reason) {
	const Snapshot s = Get();
	if (!s.active) {
		std::printf("[SceneDraw] %-13s showing everything (%u captured)\n", reason,
		            s.captured_draws);
		LOGF("[SceneDraw] %s selection=none captured=%u\n", reason, s.captured_draws);
		std::fflush(stdout);
		return;
	}
	const auto count = s.sel_end - s.sel_start + 1u;
	std::printf("[SceneDraw] %-13s %s ids %u..%u  (%u of %u)\n", reason,
	            s.mode == Mode::Skip ? "SKIP" : "HIDE", s.sel_start, s.sel_end, count,
	            s.captured_draws);
	// A single isolated draw is the actual answer, so print its identity. (vs,ps) is what
	// survives a change of draw order, which positional ids do not.
	if (count == 1) {
		std::scoped_lock lock {g_mutex};
		if (s.sel_start < g_captured.size()) {
			const auto& e = g_captured[s.sel_start];
			std::printf("[SceneDraw]   >>> id=%u vs=0x%016llx ps=0x%016llx idx=%u inst=%u topo=%u "
			            "target=%ux%u img=%u depth_test=%d write=%d\n",
			            s.sel_start, static_cast<unsigned long long>(e.vs_hash),
			            static_cast<unsigned long long>(e.ps_hash), e.index_count,
			            e.instance_count, e.topology, e.target_width, e.target_height, e.image_id,
			            e.depth_test ? 1 : 0, e.depth_write ? 1 : 0);
			LOGF("[SceneDraw] isolated id=%u vs=0x%016" PRIx64 " ps=0x%016" PRIx64
			     " idx=%u inst=%u topo=%u depth_test=%d write=%d\n",
			     s.sel_start, e.vs_hash, e.ps_hash, e.index_count, e.instance_count, e.topology,
			     e.depth_test ? 1 : 0, e.depth_write ? 1 : 0);
		}
	}
	std::fflush(stdout);
}

namespace {

void ApplyCursor(const char* reason) {
	const auto n      = CapturedCount();
	const auto cursor = g_cursor.load(std::memory_order_relaxed);
	if (n == 0 || cursor < 0 || !g_suppress_on.load(std::memory_order_relaxed)) {
		g_sel_start.store(1, std::memory_order_relaxed);
		g_sel_end.store(0, std::memory_order_relaxed);
	} else {
		const auto id = static_cast<uint32_t>(cursor) % n;
		g_sel_start.store(id, std::memory_order_relaxed);
		g_sel_end.store(id, std::memory_order_relaxed);
	}
	LogState(reason);
}

bool RequireCapture() {
	if (CapturedCount() != 0) {
		return true;
	}
	std::printf("[SceneDraw] nothing captured yet - press Capture first\n");
	std::fflush(stdout);
	return false;
}

} // namespace

void Capture() {
	// Arm recording for one frame. The draw path stays lock-free until then; the list is
	// published at the next frame boundary and ReportCapture() prints it.
	{
		std::scoped_lock lock {g_mutex};
		g_current.clear();
	}
	g_cursor.store(-1, std::memory_order_relaxed);
	g_suppress_on.store(true, std::memory_order_relaxed);
	g_sel_start.store(1, std::memory_order_relaxed);
	g_sel_end.store(0, std::memory_order_relaxed);
	g_recording.store(true, std::memory_order_relaxed);
	std::printf("[SceneDraw] arming one-frame capture on 0x%016llx\n",
	            static_cast<unsigned long long>(Target()));
	std::fflush(stdout);
	ReportCapture();
}

void ReportCapture() {
	size_t count     = 0;
	float  stability = 0.0f;
	{
		std::scoped_lock lock {g_mutex};
		count     = g_captured.size();
		stability = g_stability;
	}
	if (count == 0) {
		return;
	}
	std::printf("[SceneDraw] CAPTURED %zu draws on 0x%016llx | order stability %.1f%%%s\n", count,
	            static_cast<unsigned long long>(Target()), static_cast<double>(stability),
	            stability < 95.0f ? "  <-- LOW: prefer the (vs,ps) identity over ids" : "");
	std::fflush(stdout);
	LOGF("[SceneDraw] CAPTURED %zu draws target=0x%016" PRIx64 " stability=%.1f%%\n", count,
	     Target(), static_cast<double>(stability));
	// Log the whole list, not just what gets stepped. Relying on the console means one closed
	// window loses the evidence - and the per-draw target extent is exactly what distinguishes
	// "draws write a 1600x900 image" from "the compositor reads a 400x225 twin at that address".
	std::scoped_lock lock {g_mutex};
	for (size_t i = 0; i < g_captured.size(); i++) {
		const auto& e = g_captured[i];
		LOGF("[SceneDraw]   id=%zu vs=0x%016" PRIx64 " ps=0x%016" PRIx64
		     " idx=%u inst=%u topo=%u target=%ux%u img=%u depth_test=%d write=%d\n",
		     i, e.vs_hash, e.ps_hash, e.index_count, e.instance_count, e.topology, e.target_width,
		     e.target_height, e.image_id, e.depth_test ? 1 : 0, e.depth_write ? 1 : 0);
	}
}

void NextDraw() {
	if (!RequireCapture()) {
		return;
	}
	const auto n    = static_cast<int32_t>(CapturedCount());
	const auto next = (g_cursor.load(std::memory_order_relaxed) + 1) % n;
	g_cursor.store(next, std::memory_order_relaxed);
	g_suppress_on.store(true, std::memory_order_relaxed);
	ApplyCursor("NEXT");
}

void PrevDraw() {
	if (!RequireCapture()) {
		return;
	}
	const auto n    = static_cast<int32_t>(CapturedCount());
	auto       prev = g_cursor.load(std::memory_order_relaxed) - 1;
	if (prev < 0) {
		prev = n - 1;
	}
	g_cursor.store(prev, std::memory_order_relaxed);
	g_suppress_on.store(true, std::memory_order_relaxed);
	ApplyCursor("PREV");
}

// Flip the current selection on/off without losing it - the fastest way to confirm a suspect is
// to toggle the same draw back and forth and watch the screen.
void ToggleActive() {
	const bool on = !g_suppress_on.load(std::memory_order_relaxed);
	g_suppress_on.store(on, std::memory_order_relaxed);
	ApplyCursor(on ? "SUPPRESS ON" : "SUPPRESS OFF");
}

void SelectAll() {
	if (!RequireCapture()) {
		return;
	}
	const auto n = CapturedCount();
	g_cursor.store(-1, std::memory_order_relaxed);
	g_suppress_on.store(true, std::memory_order_relaxed);
	g_sel_start.store(0, std::memory_order_relaxed);
	g_sel_end.store(n - 1u, std::memory_order_relaxed);
	LogState("ALL");
}

void ClearSelection() {
	g_cursor.store(-1, std::memory_order_relaxed);
	g_sel_start.store(1, std::memory_order_relaxed);
	g_sel_end.store(0, std::memory_order_relaxed);
	LogState("CLEAR");
}

void ToggleMode() {
	const auto next = CurrentMode() == Mode::Skip ? Mode::Hide : Mode::Skip;
	g_mode.store(static_cast<uint32_t>(next), std::memory_order_relaxed);
	LogState(next == Mode::Skip ? "MODE SKIP" : "MODE HIDE");
}

} // namespace Libs::Graphics::SceneDrawDebug
