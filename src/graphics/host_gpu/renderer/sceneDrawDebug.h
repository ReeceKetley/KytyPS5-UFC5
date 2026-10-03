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
[[nodiscard]] bool ShouldSuppress(uint32_t id, const Entry& entry) noexcept;
[[nodiscard]] Mode CurrentMode() noexcept;
// Called once per scene-target draw, in submission order. Returns the assigned id.
uint32_t NoteDraw(uint32_t frame_num, const Entry& entry);
// Guest address of the colour target being bisected. The tonemapper reports its input every
// frame, so this follows address relocation automatically. KYTY_CENSUS_TARGET pins an explicit
// address when a different surface is under investigation.
[[nodiscard]] uint64_t Target() noexcept;
void                   ObserveSceneTarget(uint64_t address) noexcept;

// --- post bypass --------------------------------------------------------------------------
// Ctrl+F11 presents Target() directly instead of the completed post chain. This is diagnostic:
// the source is linear HDR and deliberately receives no game exposure/tonemapping.
[[nodiscard]] bool PostBypassEnabled() noexcept;
void               TogglePostBypass();

// --- wireframe ---------------------------------------------------------------------------
// Global polygonMode override, toggled live on F12. Costs nothing when off.
//
// It self-invalidates the pipeline cache for free: polygon_mode is already part of
// GraphicsPipelineKey::static_params, so flipping it produces a different key and the
// cache builds a second permutation rather than serving the filled one.
//
// fillModeNonSolid is already a required device feature and lineWidth is already 1.0f,
// so eLine needs nothing new from the device.
//
// CAVEAT: this is global. The UI is drawn with the same pipelines, so the menus go
// wireframe too and become hard to read - toggle it off to navigate.
[[nodiscard]] bool WireframeEnabled() noexcept;
void               ToggleWireframe();

// Arm a capture of the scene colour buffer (the tonemapper's i0) by writing the same trigger
// files the panel writes. Bound to Ctrl+F12.
//
// Why this exists: on-screen wireframe cannot show the scene in this title. The scene is
// rasterised into a 1068x600 HDR buffer that the broken post chain destroys before present,
// so only fullscreen post triangles and UI quads survive to the screen. Capturing that buffer
// and decoding it offline sidesteps post entirely - see tools/scene_shot.py.
//
// Override the captured shader with KYTY_SCENE_SHOT_HASH (hex, no 0x).
void DumpSceneShot();

// --- ui thread ---------------------------------------------------------------------------
void Capture();       // arm a one-frame capture; call again to report
void ReportCapture(); // print the most recent capture
void NextDraw();      // isolate the next single draw (wraps); prints its identity
void PrevDraw();
void ToggleActive(); // suppression on/off, so you can A/B the same draw instantly
void SelectAll();
void ClearSelection();
void ToggleMode(); // Skip <-> Hide
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
