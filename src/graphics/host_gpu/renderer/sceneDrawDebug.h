#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SCENEDRAWDEBUG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SCENEDRAWDEBUG_H_

#include "common/common.h"

#include <cstdint>
#include <vector>

namespace Libs::Graphics::SceneDrawDebug {

// Live bisection of the draws that write the scene colour target.
//
// Why this exists: the fight round shows a black shape over the centre that rotates with the
// camera - something is drawn ON TOP of the arena, which is visible around its edges. Finding
// which draw does that by env-var + rebuild + re-navigate costs a full cycle per bisection step.
// This moves the control to live keys so a ~10-step binary search happens in one sitting.
//
// CAPTURE FIRST. Draw IDs are positional within a frame, and draw order is not guaranteed stable
// between frames (LOD, visibility, sorting). Capture freezes the list and records per-draw
// identity; the reported order stability says how much of the order held between the two most
// recent frames. Below ~95%, positional ids are unreliable - use the (vs_hash, ps_hash) printed
// with each selection instead, which is stable regardless of order.
//
// This target carries only ~10 draws per frame, so stepping one at a time is faster and clearer
// than a binary search.

struct Entry {
	uint64_t vs_hash        = 0;
	uint64_t ps_hash        = 0;
	uint32_t index_count    = 0;
	uint32_t instance_count = 0;
	uint32_t topology       = 0;
	uint32_t target_width   = 0;
	uint32_t target_height  = 0;
	uint32_t image_id       = 0;
	bool     depth_test     = false;
	bool     depth_write    = false;
};

enum class Mode : uint32_t {
	Skip = 0, // drop the draw entirely (also removes its depth writes)
	Hide = 1, // colour writes off, depth/stencil behaviour preserved
};

// --- render thread -----------------------------------------------------------------------
// True when `id` (this frame's scene-draw index) is inside the active selection.
[[nodiscard]] bool ShouldSuppress(uint32_t id) noexcept;
[[nodiscard]] Mode CurrentMode() noexcept;
// Called once per scene-target draw, in submission order. Returns the assigned id.
uint32_t NoteDraw(uint32_t frame_num, const Entry& entry);
// Guest address of the colour target being bisected (KYTY_CENSUS_TARGET, default the in-fight
// HDR scene target).
[[nodiscard]] uint64_t Target() noexcept;

// --- ui thread ---------------------------------------------------------------------------
void Capture();        // arm a one-frame capture; call again to report
void ReportCapture();  // print the most recent capture
void NextDraw();       // isolate the next single draw (wraps); prints its identity
void PrevDraw();
void ToggleActive();   // suppression on/off, so you can A/B the same draw instantly
void SelectAll();
void ClearSelection();
void ToggleMode();     // Skip <-> Hide
void LogState(const char* reason);

struct Snapshot {
	uint32_t captured_draws = 0;
	uint32_t sel_start      = 0;
	uint32_t sel_end        = 0;
	bool     active         = false;
	Mode     mode           = Mode::Skip;
	float    stability      = 0.0f; // % of ids whose (vs,ps) matched the previous frame
};
[[nodiscard]] Snapshot Get() noexcept;

} // namespace Libs::Graphics::SceneDrawDebug

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_SCENEDRAWDEBUG_H_
