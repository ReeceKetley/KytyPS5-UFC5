#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_

#include <cstdint>

namespace Libs::Graphics {

struct ShaderVertexInputInfo;

[[nodiscard]] int32_t  ResolveVertexOffset(uint32_t                     index_offset,
                                           const ShaderVertexInputInfo& vs_input_info);
[[nodiscard]] uint32_t ResolveInstanceOffset(const ShaderVertexInputInfo& vs_input_info);

// Diagnostic bridge used by the on-demand surface dumper. Enabled only when
// KYTY_TRACK_DRAW_TARGETS is set, so normal draws do not pay for the map/lock.
[[nodiscard]] bool GetTrackedDrawTarget(uint64_t address, uint32_t& image_index,
                                        uint32_t& image_generation, uint32_t& frame_num);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RENDERDRAW_H_
