#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace Libs::Graphics {

// Single-launch A-B harness.
//
// WHY THIS EXISTS. The single-run present-rate variance floor is about 8% (EXP-0027), and that
// noise is dominated by scene variation between paused fights rather than by the sampler: three
// local optimizations returned at most 1.7% and could not be resolved end to end at all
// (EXP-0019). So an A-B that relaunches the emulator and re-drives to a fight cannot settle a
// 10% question -- which is exactly the state EXP-0029 was left in. The fix is to flip the
// feature WITHOUT relaunching, inside one paused fight, so the scene is held fixed and only the
// code path varies.
//
// HOW. KYTY_AB_CONTROL_FILE=<path> names a file holding one integer, the active variant. Every
// consumer polls it through AbVariant() at most once per 250 ms and maps the integer onto its own
// behaviour; a feature with no control file in use keeps its launch default. The present-rate
// sampler in swapchain.cpp also reads the variant, tags each logged window with it, and RESETS
// the window whenever it changes, so no sample straddles a flip and no window can be
// misattributed. The variant is the single source of truth for both sides, so there is no second
// file to get out of step.
//
// The poll is a file open every 250 ms on whichever thread arrives first, which is why this is
// safe to call from the per-draw path: Image::Transit runs about 10300 times per epoch and pays
// only an atomic load on all but one call in a quarter-second.
[[nodiscard]] inline const char* AbControlPath() noexcept {
	static const char* path = std::getenv("KYTY_AB_CONTROL_FILE");
	return path;
}

// -1 means no variant has been read, i.e. every consumer is on its launch default.
[[nodiscard]] inline std::atomic<int32_t>& AbVariantSlot() noexcept {
	static std::atomic<int32_t> variant {-1};
	return variant;
}

[[nodiscard]] inline int32_t AbVariant(int32_t fallback) noexcept {
	const char* path = AbControlPath();
	if (path == nullptr) {
		return fallback;
	}
	static std::atomic<uint64_t> next_poll {0};
	const auto now = static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(
	        std::chrono::steady_clock::now().time_since_epoch())
	        .count());
	auto due = next_poll.load(std::memory_order_relaxed);
	if (now >= due && next_poll.compare_exchange_strong(due, now + 250000000ull,
	                                                    std::memory_order_relaxed)) {
		int32_t requested = -1;
		if (FILE* file = std::fopen(path, "r")) {
			char text[16] {};
			if (std::fgets(text, sizeof(text), file) != nullptr) {
				requested = static_cast<int32_t>(std::strtol(text, nullptr, 10));
			}
			std::fclose(file);
		}
		AbVariantSlot().store(requested, std::memory_order_relaxed);
	}
	const auto current = AbVariantSlot().load(std::memory_order_relaxed);
	return current < 0 ? fallback : current;
}

} // namespace Libs::Graphics
