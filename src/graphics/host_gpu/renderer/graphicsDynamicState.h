#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GRAPHICSDYNAMICSTATE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GRAPHICSDYNAMICSTATE_H_

#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics {

// The resolved arguments of every dynamic-state call the graphics draw path makes.
//
// Split out of SetGraphicsDynamicParams so the register maths runs on the producer side and only
// this struct reaches the recorder. FACT-0035 measured HW::Context, ShaderVertexInputInfo and
// RenderDepthInfo as 5408 of the old 6784-byte commit payload, all three read exactly once and
// only by that function, so a queue carrying the raw inputs would hand back EXP-0031's +4.3%.
struct GraphicsDynamicState {
	// Mirrors std::size(HW::ScreenViewport{}.viewports); renderDraw.cpp static_asserts the match
	// rather than including the guest register header here.
	static constexpr uint32_t ViewportSlots = 16;

	uint32_t                                             viewport_count = 0;
	std::array<vk::Viewport, ViewportSlots>              viewports {};
	std::array<vk::Rect2D, ViewportSlots>                scissors {};
	float                                                line_width = 1.0F;
	std::array<float, 4>                                 blend_constants {};
	bool                                                 depth_test_enable  = false;
	bool                                                 depth_write_enable = false;
	vk::CompareOp                                        depth_compare_op {};
	bool                                                 depth_bias_enable    = false;
	float                                                bias_constant_factor = 0.0F;
	float                                                bias_clamp           = 0.0F;
	float                                                bias_slope_factor    = 0.0F;
	bool                                                 stencil_test_enable = false;
	vk::StencilOpState                                   stencil_front {};
	vk::StencilOpState                                   stencil_back {};
	uint32_t                                             color_write_count = 0;
	std::array<vk::Bool32, RENDER_COLOR_ATTACHMENTS_MAX> color_write_enable {};
};

// Last state actually applied to one Vulkan command buffer, so unchanged setters can be skipped.
//
// WHY IT IS PER COMMAND BUFFER: Vulkan dynamic state does not survive vkBeginCommandBuffer, so
// this is reset in CommandBuffer::Begin(). Caching it anywhere with a longer life would skip
// setters a fresh buffer genuinely needs.
//
// WHY COLOUR-WRITE IS NOT CACHED: eColorWriteEnableEXT is only put in the pipeline's dynamic list
// when color_count != 0 (shaders.cpp), so a pipeline with no colour attachments declares it
// STATIC -- and binding such a pipeline leaves any previously set dynamic value undefined. The
// other fourteen states are dynamic in every graphics pipeline, so they are safe to cache across
// pipeline binds; colour-write is re-issued every time it is needed.
// INVARIANT, and the thing that breaks this if forgotten: any code issuing one of these dynamic
// state commands on a CommandBuffer by any route other than ApplyGraphicsDynamicParams must call
// DynamicCache().Reset() on that buffer, or the next draw will skip a setter it needs and
// inherit the foreign value. As of 2026-10-08 the only such site is blitHelper.cpp, which runs on
// the same CommandBuffer as the draw path; swapchain.cpp is safe because it records through a
// separate present scheduler, hence a different CommandBuffer with its own cache. Before adding a
// setter anywhere, re-run:
//   grep -rn "setViewport\|setScissor\|setLineWidth\|setBlendConstants\|setDepthTestEnable\
//     \|setDepthWriteEnable\|setDepthCompareOp\|setDepthBias\|setStencil\|setColorWriteEnable"
struct GraphicsDynamicCache {
	GraphicsDynamicState applied {};
	bool                 valid = false;

	void Reset() noexcept { valid = false; }
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GRAPHICSDYNAMICSTATE_H_
