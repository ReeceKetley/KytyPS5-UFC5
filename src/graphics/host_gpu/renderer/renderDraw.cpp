#include "graphics/host_gpu/renderer/renderDraw.h"

#include "common/assert.h"
#include "common/timer.h"
#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/host_gpu/renderer/pipeline/shaderResourceBarrier.h"
#include "graphics/host_gpu/renderer/gpuTimestamps.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sceneDrawDebug.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/errno.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

namespace {

struct TrackedDrawTarget {
	uint32_t image_index      = UINT32_MAX;
	uint32_t image_generation = 0;
	uint32_t frame_num        = UINT32_MAX;
};

std::mutex                                      g_tracked_draw_targets_lock;
std::unordered_map<uint64_t, TrackedDrawTarget> g_tracked_draw_targets;

bool ShouldTrackDrawTargets() {
	static const bool enabled = [] {
		const char* env = std::getenv("KYTY_TRACK_DRAW_TARGETS");
		return env != nullptr && env[0] != '\0' && env[0] != '0';
	}();
	return enabled;
}

void TrackDrawTarget(uint64_t address, ImageId image_id, uint32_t frame_num) {
	if (!ShouldTrackDrawTargets() || !image_id) {
		return;
	}
	std::scoped_lock guard {g_tracked_draw_targets_lock};
	g_tracked_draw_targets.insert_or_assign(address,
	                                        TrackedDrawTarget {image_id.index,
	                                                           image_id.generation, frame_num});
}

} // namespace

bool GetTrackedDrawTarget(uint64_t address, uint32_t& image_index, uint32_t& image_generation,
                          uint32_t& frame_num) {
	std::scoped_lock guard {g_tracked_draw_targets_lock};
	const auto       it = g_tracked_draw_targets.find(address);
	if (it == g_tracked_draw_targets.end()) {
		return false;
	}
	image_index      = it->second.image_index;
	image_generation = it->second.image_generation;
	frame_num        = it->second.frame_num;
	return true;
}

int32_t ResolveVertexOffset(uint32_t index_offset, const ShaderVertexInputInfo& vs_input_info) {
	if (index_offset != 0 || !vs_input_info.fetch_embedded) {
		return static_cast<int32_t>(index_offset);
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = *vs_input_info.stage.program;
	const auto& resources = vs_input_info.stage.resources;
	if (program.info.vertex_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.vertex_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			return static_cast<int32_t>(resources.user_data[index]);
		}
	}

	return 0;
}

uint32_t ResolveInstanceOffset(const ShaderVertexInputInfo& vs_input_info) {
	if (!vs_input_info.fetch_embedded) {
		return 0;
	}

	EXIT_IF(!vs_input_info.stage);
	const auto& program   = *vs_input_info.stage.program;
	const auto& resources = vs_input_info.stage.resources;
	if (program.info.instance_offset_sgpr >= static_cast<int32_t>(program.user_data_base)) {
		const auto index =
		    static_cast<uint32_t>(program.info.instance_offset_sgpr) - program.user_data_base;
		if (index < resources.user_data.size()) {
			return resources.user_data[index];
		}
	}

	return 0;
}

static std::atomic<uint32_t> g_draw_state_log_count   = 0;
static std::atomic<uint32_t> g_draw_input_log_count   = 0;
static std::atomic<uint32_t> g_mrt_state_log_count    = 0;

static std::atomic<uint32_t> g_framebuffer_skip_log_count = 0;

// DIAGNOSTIC, default off. `KYTY_SKIP_PS_HASH=<hex>[,<hex>...]` drops the draw call for draws
// binding those pixel shaders - everything else about the draw (pipeline, descriptors, render
// targets, all the host CPU work) still happens, only the vkCmdDraw* is not issued.
//
// Why it exists: the claim that 3 pixel ubershaders own ~79% of the frame rests on per-draw GPU
// timestamp brackets, which is attribution, not causation - begin/end timestamps around a draw
// on a pipelined GPU can still absorb work from other draws in flight, and this ledger has
// already shipped one wrong figure from exactly that (the 96% wave64 number). Not issuing the
// draws at all is the causal control, and it also gives the UPPER BOUND on what any fix to
// those shaders can ever be worth. Keeping the CPU-side work means the delta is GPU time only.
// The frame is not correct with this set - draws are missing. It is a measurement tool.
static bool ShouldSkipPixelShaderHash(uint64_t hash) {
	static const std::vector<uint64_t> skipped = [] {
		std::vector<uint64_t> list;
		const char*           env = std::getenv("KYTY_SKIP_PS_HASH");
		if (env == nullptr) {
			return list;
		}
		const char* cursor = env;
		while (*cursor != '\0') {
			while (*cursor == ',' || *cursor == ' ' || *cursor == '\t') {
				++cursor;
			}
			if (*cursor == '\0') {
				break;
			}
			if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
				cursor += 2;
			}
			char*      end   = nullptr;
			const auto value = std::strtoull(cursor, &end, 16);
			if (end == cursor) {
				break;
			}
			list.push_back(static_cast<uint64_t>(value));
			cursor = end;
		}
		return list;
	}();
	return !skipped.empty() && std::find(skipped.begin(), skipped.end(), hash) != skipped.end();
}

// Diagnostic only. When enabled, bypass depth rejection for the three watched UFC5 pixel
// shaders on the frozen fight HDR target. This is deliberately narrower than a global depth
// override: if the target starts changing, depth rejection is causal; if it stays frozen, the
// remaining candidates are rasterization/clip, shader discard, and ineffective colour output.
static bool ShouldForceWatchedDepthOff() {
	static const bool enabled = [] {
		const char* env = std::getenv("KYTY_WATCH_FORCE_DEPTH_OFF");
		return env != nullptr && env[0] != '\0' && env[0] != '0';
	}();
	return enabled;
}

static bool ShouldCensusLargeDrawTargets() {
	static const bool enabled = [] {
		const char* env = std::getenv("KYTY_DRAW_TARGET_CENSUS");
		return env != nullptr && env[0] != '\0' && env[0] != '0';
	}();
	return enabled;
}

static bool ShouldForceLargeTargetDepthOff() {
	static const bool enabled = [] {
		const char* env = std::getenv("KYTY_LARGE_TARGET_FORCE_DEPTH_OFF");
		return env != nullptr && env[0] != '\0' && env[0] != '0';
	}();
	return enabled;
}

static float ConvertPolygonOffsetConstantFactor(float guest_factor, const HW::PolyOffset& offset,
                                                vk::Format host_depth_format) {
	if (offset.db_is_float_fmt) {
		return guest_factor;
	}

	int host_depth_bits = 0;
	switch (host_depth_format) {
		case vk::Format::eD16Unorm:
		case vk::Format::eD16UnormS8Uint: host_depth_bits = 16; break;
		case vk::Format::eD24UnormS8Uint: host_depth_bits = 24; break;
		default:
			// A fixed-point guest bias cannot be represented exactly by a floating-point host
			// attachment without VK_EXT_depth_bias_control.
			return guest_factor;
	}
	return std::ldexp(guest_factor, host_depth_bits + offset.neg_num_db_bits);
}

static const char* RenderColorTypeName(const RenderColorInfo& color) {
	return color.image_id ? "RenderTexture" : "NoColorOutput";
}

static void LogFramebufferSkip(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               uint32_t index_count, uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	auto log_id = g_framebuffer_skip_log_count.fetch_add(1, std::memory_order_relaxed);
	if (log_id >= 128) {
		return;
	}

	LOGF(
	    "DrawFramebufferSkip[%u]: %s color=%s color_addr=0x%010" PRIx64 " color_size=0x%016" PRIx64
	    " color_image=%s depth_format=%s depth_image=%s depth_vaddr_num=%d target_mask=0x%08" PRIx32
	    " prim=%u index_count=%u flags=0x%08" PRIx32 "\n",
	    log_id, draw_name, RenderColorTypeName(color), color.desc.info.data.address,
	    color.desc.info.data.size, color.image_id ? "yes" : "no",
	    vk::to_string(depth.desc.view_info.format).c_str(), depth.image_id ? "yes" : "no",
	    static_cast<int>(!depth.desc.info.data.Empty()) +
	        static_cast<int>(depth.desc.info.HasStencil()),
	    ctx.GetRenderTargetMask(), static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags);
}

static void LogMrtState(const char* draw_name, const CommandBuffer& buffer,
                        const ShaderPixelInputInfo& ps_input_info) {
	const auto& ctx            = buffer.GetRegisters();
	const auto& sh_regs        = ctx.GetShaderRegisters();
	const auto  rt_mask        = ctx.GetRenderTargetMask();
	const auto  cb_shader_mask = sh_regs.m_cbShaderMask;
	const auto& bc0            = ctx.GetBlendControl(0);

	auto log_id = g_mrt_state_log_count.fetch_add(1);
	if (log_id >= 32) {
		return;
	}

	LOGF("MrtState[%u]: %s rt_mask=0x%08" PRIx32 " cb_shader_mask=0x%08" PRIx32
	     " blend0=%s src=%u dst=%u alpha_src=%u alpha_dst=%u sep_alpha=%s\n",
	     log_id, draw_name, rt_mask, cb_shader_mask, bc0.enable ? "true" : "false",
	     bc0.color_srcblend, bc0.color_destblend, bc0.alpha_srcblend, bc0.alpha_destblend,
	     bc0.separate_alpha_blend ? "true" : "false");

	for (uint32_t i = 0; i < 8; i++) {
		const auto& rt  = ctx.GetRenderTarget(i);
		const auto& bc  = ctx.GetBlendControl(i);
		const auto  ctm = (rt_mask >> (i * 4u)) & 0x0fu;
		const auto  csm = (cb_shader_mask >> (i * 4u)) & 0x0fu;

		if (rt.base.addr == 0 && ps_input_info.target_output_mode[i] == 0 && ctm == 0 && csm == 0 &&
		    !bc.enable) {
			continue;
		}

		LOGF("MrtState[%u]: slot=%u addr=0x%010" PRIx64
		     " target_mask=0x%x shader_mask=0x%x out_mode=%u"
		     " fmt=0x%08" PRIx32 " nfmt=0x%08" PRIx32 " order=0x%08" PRIx32
		     " width=%u height=%u tile=%u"
		     " blend=%s src=%u dst=%u alpha_src=%u alpha_dst=%u\n",
		     log_id, i, rt.base.addr, ctm, csm, ps_input_info.target_output_mode[i],
		     static_cast<uint32_t>(rt.info.format), static_cast<uint32_t>(rt.info.channel_type),
		     static_cast<uint32_t>(rt.info.channel_order), rt.attrib2.width + 1,
		     rt.attrib2.height + 1, static_cast<uint32_t>(rt.attrib3.tile_mode),
		     bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.alpha_srcblend,
		     bc.alpha_destblend);
	}
}

static void LogDrawTargetState(const char* draw_name, const RenderColorInfo& color,
                               const RenderDepthInfo& depth, const CommandBuffer& buffer,
                               const ShaderPixelInputInfo& ps_input_info, uint32_t index_count,
                               uint32_t flags) {
	const auto& ctx  = buffer.GetRegisters();
	const auto& ucfg = buffer.GetUserConfig();
	if (!color.image_id) {
		return;
	}

	auto log_id = g_draw_state_log_count.fetch_add(1);
	if (log_id >= 192) {
		return;
	}

	const auto& cc             = ctx.GetColorControl();
	const auto& bc             = ctx.GetBlendControl(color.target_slot);
	const auto& dc             = ctx.GetDepthControl();
	const auto& vp             = ctx.GetScreenViewport();
	const auto& vp0            = vp.viewports[0];
	const auto& ps_resources   = ps_input_info.stage.program->info;
	const auto  sampled_images = std::count_if(
	    ps_resources.images.begin(), ps_resources.images.end(), [](const auto& image) {
		    return image.resource_class == ShaderRecompiler::IR::ImageResourceClass::Sampled;
	    });

	const auto extent = color.Extent();
	const auto sc     = calc_final_scissor(vp, ctx.GetScanModeControl(), extent, 0);

	LOGF(
	    "DrawTargetState[%u]: frame=%d %s target=%s addr=0x%010" PRIx64
	    " extent=%ux%u prim=%u index_count=%u flags=0x%08" PRIx32 " color_mask=0x%08" PRIx32
	    " cc_mode=%u cc_op=0x%02x"
	    " blend=%s src=%u dst=%u comb=%u ps_tex=%d sampled=%d storage=%d ps_kill=%s target_mode0=%u"
	    " depth_test=%s depth_write=%s depth_func=%u depth_clear=%s viewport=(%.1f,%.1f %.1fx%.1f) "
	    "scissor=(%d,%d)-(%d,%d)\n",
	    log_id, buffer.GetContext().GetGpu().GetFrameNum(), draw_name, RenderColorTypeName(color),
	    color.desc.info.data.address, extent.width, extent.height,
	    static_cast<uint32_t>(ucfg.GetPrimType()), index_count, flags, ctx.GetRenderTargetMask(),
	    cc.mode, cc.op,
	    bc.enable ? "true" : "false", bc.color_srcblend, bc.color_destblend, bc.color_comb_fcn,
	    static_cast<int>(ps_resources.images.size()), static_cast<int>(sampled_images),
	    static_cast<int>(ps_resources.images.size() - sampled_images),
	    ps_input_info.ps_pixel_kill_enable ? "true" : "false", ps_input_info.target_output_mode[0],
	    dc.z_enable ? "true" : "false", dc.z_write_enable ? "true" : "false", dc.zfunc,
	    depth.depth_clear_enable ? "true" : "false", vp0.xoffset - vp0.xscale,
	    vp0.yoffset - vp0.yscale, vp0.xscale * 2.0f, vp0.yscale * 2.0f, sc.left, sc.top, sc.right,
	    sc.bottom);

	LogMrtState(draw_name, buffer, ps_input_info);
}

static void LogDrawInputState(const CommandBuffer& buffer, const RenderColorInfo& color,
                              const ShaderVertexInputInfo& vs_input_info,
                              uint32_t index_type_and_size, uint32_t index_count,
                              const void* index_addr) {
	auto log_id = g_draw_input_log_count.fetch_add(1);
	if (log_id >= 64) {
		return;
	}

	LOGF("DrawInputState[%u]: frame=%d target=%s addr=0x%010" PRIx64
	     " index_type=%u index_count=%u index_addr=0x%016" PRIx64
	     " vs_resources=%d vs_buffers=%d\n",
	     log_id, buffer.GetContext().GetGpu().GetFrameNum(), RenderColorTypeName(color),
	     color.desc.info.data.address, index_type_and_size, index_count,
	     reinterpret_cast<uint64_t>(index_addr), vs_input_info.resources_num,
	     vs_input_info.buffers_num);

	for (int bi = 0; bi < vs_input_info.buffers_num; bi++) {
		const auto& b = vs_input_info.buffers[bi];
		LOGF("DrawInputState[%u]: vb[%d] addr=0x%010" PRIx64
		     " stride=%u records=%u fetch_index=%u attr_num=%d\n",
		     log_id, bi, b.addr, b.stride, b.num_records, b.fetch_index, b.attr_num);

		const auto* bytes = reinterpret_cast<const uint8_t*>(b.addr);
		if (bytes != nullptr && b.stride != 0) {
			const uint32_t records = std::min<uint32_t>(b.num_records, 4u);
			for (uint32_t rec = 0; rec < records; rec++) {
				const auto* rec_bytes = bytes + static_cast<uint64_t>(rec) * b.stride;
				const auto  dword_num = std::min<uint32_t>(b.stride / 4u, 12u);
				uint32_t    raw[12]   = {};
				float       flt[12]   = {};
				for (uint32_t i = 0; i < dword_num; i++) {
					std::memcpy(&raw[i], rec_bytes + i * 4u, sizeof(raw[i]));
					std::memcpy(&flt[i], rec_bytes + i * 4u, sizeof(flt[i]));
				}
				LOGF("DrawInputState[%u]: vb[%d].rec[%u] stride=%u dwords=%u raw=%08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " %08" PRIx32 " %08" PRIx32 " %08" PRIx32
				     " f=(%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f)\n",
				     log_id, bi, rec, b.stride, dword_num, raw[0], raw[1], raw[2], raw[3], raw[4],
				     raw[5], raw[6], raw[7], raw[8], flt[0], flt[1], flt[2], flt[3], flt[4], flt[5],
				     flt[6], flt[7], flt[8]);

				for (int ai = 0; ai < b.attr_num; ai++) {
					const auto  res_index = b.attr_indices[ai];
					const auto& r         = vs_input_info.resources[res_index];
					const auto& rd        = vs_input_info.resources_dst[res_index];
					const auto  offset    = b.attr_offsets[ai];
					if (offset + 4u <= b.stride &&
					    r.Format() == Prospero::BufferFormat::k8_8_8_8UNorm) {
						uint32_t packed = 0;
						std::memcpy(&packed, rec_bytes + offset, sizeof(packed));
						const auto r8 = (packed >> 0u) & 0xffu;
						const auto g8 = (packed >> 8u) & 0xffu;
						const auto b8 = (packed >> 16u) & 0xffu;
						const auto a8 = (packed >> 24u) & 0xffu;
						LOGF("DrawInputState[%u]: vb[%d].rec[%u].attr[%d] dst=v%d fmt=56 "
						     "rgba8=%02" PRIx32 "%02" PRIx32 "%02" PRIx32 "%02" PRIx32
						     " rgba=(%.3f,%.3f,%.3f,%.3f)\n",
						     log_id, bi, rec, ai, rd.register_start, r8, g8, b8, a8,
						     static_cast<double>(r8) / 255.0, static_cast<double>(g8) / 255.0,
						     static_cast<double>(b8) / 255.0, static_cast<double>(a8) / 255.0);
					}
				}
			}
		}

		for (int ai = 0; ai < b.attr_num; ai++) {
			const auto  res_index = b.attr_indices[ai];
			const auto& r         = vs_input_info.resources[res_index];
			const auto& rd        = vs_input_info.resources_dst[res_index];
			LOGF("DrawInputState[%u]: attr[%d] res=%d offset=%u dst=v%d regs=%d fetch_index=%u "
			     "sharp=%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
			     log_id, ai, res_index, b.attr_offsets[ai], rd.register_start, rd.registers_num,
			     rd.fetch_index, r.fields[0], r.fields[1], r.fields[2], r.fields[3]);
		}
	}
}

static void SetGraphicsDynamicParams(const CommandBuffer& buffer, vk::CommandBuffer vk_buffer,
                                     const ShaderVertexInputInfo& vs_input_info,
                                     const RenderColorInfo* colors, uint32_t color_count,
                                     const RenderDepthInfo& depth) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(colors == nullptr);
	const auto& ctx = buffer.GetRegisters();

	const auto&  vp = ctx.GetScreenViewport();
	vk::Extent2D framebuffer_extent {};
	if (color_count > 0 && colors[0].image_id) {
		framebuffer_extent = colors[0].Extent();
	} else if (depth.image_id) {
		framebuffer_extent = {depth.desc.info.extent.width, depth.desc.info.extent.height};
	} else {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		framebuffer_extent = {limits.maxFramebufferWidth, limits.maxFramebufferHeight};
	}

	const auto& outputs = vs_input_info.stage.program->info.outputs;
	const bool  indexed_viewports =
	    std::any_of(outputs.begin(), outputs.end(), [](const auto& output) {
		    return output.kind == ShaderRecompiler::IR::StageOutputKind::ViewportIndex;
	    });
	constexpr uint32_t viewport_slots = std::size(HW::ScreenViewport {}.viewports);
	std::array<vk::Viewport, viewport_slots> viewports {};
	std::array<vk::Rect2D, viewport_slots>   scissors {};
	const uint32_t viewport_count = indexed_viewports ? viewport_slots : 1;
	for (uint32_t i = 0; i < viewport_count; i++) {
		const auto& guest    = vp.viewports[i];
		auto&       viewport = viewports[i];
		if (ctx.GetClipControl().clip_disable) {
			const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
			viewport.width  = static_cast<float>(std::min(limits.maxViewportDimensions[0], 16384u));
			viewport.height = static_cast<float>(std::min(limits.maxViewportDimensions[1], 16384u));
		} else {
			viewport.x      = guest.xoffset - guest.xscale;
			viewport.y      = guest.yoffset - guest.yscale;
			viewport.width  = guest.xscale * 2.0f;
			viewport.height = guest.yscale * 2.0f;
		}
		viewport.minDepth =
		    guest.zoffset - (ctx.GetClipControl().dx_clip_space ? 0.0f : guest.zscale);
		viewport.maxDepth = guest.zscale + guest.zoffset;

		const auto final_scissor =
		    calc_final_scissor(vp, ctx.GetScanModeControl(), framebuffer_extent, i);
		auto& scissor  = scissors[i];
		scissor.offset = {final_scissor.left, final_scissor.top};
		scissor.extent = {static_cast<uint32_t>(final_scissor.right - final_scissor.left),
		                  static_cast<uint32_t>(final_scissor.bottom - final_scissor.top)};
		if (viewport.width == 0.0f) {
			// Keep empty slots at their guest index; Vulkan requires a positive viewport width.
			viewport.width = 1.0f;
			scissor.extent = {0, 0};
		}
	}
	vk_buffer.setViewportWithCount(viewport_count, viewports.data());
	vk_buffer.setScissorWithCount(viewport_count, scissors.data());

	float line_width = ctx.GetLineWidth();
	if (line_width != 1.0f) {
		static bool logged = false;
		if (!logged) {
			LOGF("Render: temporary: clamping Vulkan line width %f to 1.0 because wideLines is "
			     "not enabled\n",
			     line_width);
			logged = true;
		}
		line_width = 1.0f;
	}
	vk_buffer.setLineWidth(line_width);
	const auto&      blend = ctx.GetBlendColor();
	const std::array blend_constants {blend.red, blend.green, blend.blue, blend.alpha};
	vk_buffer.setBlendConstants(blend_constants.data());
	vk_buffer.setDepthTestEnable(depth.depth_test_enable ? VK_TRUE : VK_FALSE);
	vk_buffer.setDepthWriteEnable(depth.depth_write_enable ? VK_TRUE : VK_FALSE);
	vk_buffer.setDepthCompareOp(depth.depth_compare_op);
	vk_buffer.setStencilTestEnable(depth.stencil_test_enable ? VK_TRUE : VK_FALSE);

	const auto& mode              = ctx.GetModeControl();
	const auto& poly_offset       = ctx.GetPolyOffset();
	const bool  use_front         = mode.poly_offset_front_enable && !mode.cull_front;
	const bool  use_back          = mode.poly_offset_back_enable && !mode.cull_back;
	const bool  depth_bias_enable = use_front || use_back;
	vk_buffer.setDepthBiasEnable(depth_bias_enable ? VK_TRUE : VK_FALSE);
	if (depth_bias_enable) {
		// Vulkan has one bias for both faces. Prefer a visible front face when both are enabled.
		const float guest_constant_factor =
		    use_front ? poly_offset.front_offset : poly_offset.back_offset;
		const float constant_factor = ConvertPolygonOffsetConstantFactor(
		    guest_constant_factor, poly_offset, depth.desc.view_info.format);
		const float slope_factor =
		    (use_front ? poly_offset.front_scale : poly_offset.back_scale) / 16.0f;
		vk_buffer.setDepthBias(constant_factor, poly_offset.clamp, slope_factor);
	}

	if (depth.stencil_test_enable) {
		vk_buffer.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
		                                depth.stencil_dynamic_front.compareMask);
		vk_buffer.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
		                                depth.stencil_dynamic_back.compareMask);
		vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
		                              depth.stencil_dynamic_front.writeMask);
		vk_buffer.setStencilWriteMask(vk::StencilFaceFlagBits::eBack,
		                              depth.stencil_dynamic_back.writeMask);
		vk_buffer.setStencilReference(vk::StencilFaceFlagBits::eFront,
		                              depth.stencil_dynamic_front.reference);
		vk_buffer.setStencilReference(vk::StencilFaceFlagBits::eBack,
		                              depth.stencil_dynamic_back.reference);
	}

#if defined(__APPLE__)
	// MoltenVK has no VK_EXT_color_write_enable; the pipeline is created without the
	// eColorWriteEnableEXT dynamic state and relies on the static colorWriteMask instead.
#else
	vk::Bool32 enable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	// Color-control operation selects special color-buffer paths, not the normal component write
	// mask. Attachment availability therefore follows the target write mask.
	for (uint32_t i = 0; i < color_count; i++) {
		enable[i] = render_target_mask_slot(ctx.GetRenderTargetMask(), colors[i].target_slot) != 0
		                ? VK_TRUE
		                : VK_FALSE;
	}
	if (color_count != 0) {
		vk_buffer.setColorWriteEnableEXT(color_count, enable);
	}
#endif
}

static bool DrawHasValidVertexShader(const HW::Shader& sh_ctx) {

	const auto& vs = sh_ctx.GetVs();
	return ShaderAddressValid(vs.es_regs.data_addr);
}

static bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return db.shader_kill_enable || db.shader_z_export_enable || db.shader_mask_export_enable ||
	       db.shader_dual_export_enable || db.shader_execute_on_noop;
}

struct DrawRenderState {
	RenderDepthInfo       depth_info;
	RenderColorInfo       color_info[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	uint32_t              color_count                              = 0;
	bool                  ps_active                                = true;
	ShaderVertexInputInfo vs_input_info;
	ShaderPixelInputInfo  ps_input_info;
	PipelineCache::GraphicsPrograms programs;
};

struct DrawCallInfo {
	const char*          name           = nullptr;
	CommandBufferDebugOp debug_op       = CommandBufferDebugOp::DrawIndex;
	uint32_t             index_count    = 0;
	uint32_t             instance_count = 0;
	uint32_t             first_instance = 0;
};

RenderState RenderExecutor::AcquireRenderTargets(CommandBuffer& buffer, RenderColorInfo* colors,
                                                 uint32_t color_count, RenderDepthInfo& depth,
                                                 const std::optional<PreparedBindings>& pixel) {
	EXIT_IF(colors == nullptr || color_count > RENDER_COLOR_ATTACHMENTS_MAX);
	auto&       cache = m_context.GetTextureCache();
	RenderState state {};
	state.width                 = std::numeric_limits<uint32_t>::max();
	state.height                = std::numeric_limits<uint32_t>::max();
	state.num_layers            = std::numeric_limits<uint32_t>::max();
	state.num_color_attachments = color_count;
	uint32_t attachment_samples = 0;
	for (uint32_t i = 0; i < color_count; i++) {
		auto& target = colors[i];
		EXIT_IF(!target.image_id);
		const auto old_image = cache.m_slot_images.try_get(target.image_id);
		if (old_image == nullptr || (!old_image->registered && !old_image->info.data.Empty()) ||
		    old_image->binding.needs_rebind || old_image->depth_id) {
			// FindRenderTarget rejects an image that carries a depth alias; the guest is
			// reusing a depth surface as a color target. Re-resolve so FindImage recreates
			// a color image for this address (textureCache.cpp force-recreates on IsDepth).
			if (old_image != nullptr) {
				old_image->binding = {};
			}
			target.image_id = cache.FindImage(target.desc);
			BindRenderTarget(target.image_id);
		}
		const auto image_view = cache.FindRenderTarget(target.image_id, target.desc);
		if (TraceResourceAddress(target.desc.info.data.address, target.desc.info.data.size)) {
			TraceResourceBinding(m_context.GetGpu().GetFrameNum(),
			    fmt::format("color_target={} ps_hash=0x{:016x} addr=0x{:x} bytes={} "
			                "format={} mip={} layer={}+{}",
			                i, pixel ? pixel->program->shader_hash : uint64_t {0},
			                target.desc.info.data.address, target.desc.info.data.size,
			                static_cast<int>(target.desc.view_info.format),
			                target.desc.view_info.base_level, target.desc.view_info.base_layer,
			                target.desc.view_info.layer_count));
		}
		auto&      image      = cache.GetImage(target.image_id);
		SetVulkanObjectNameF(m_context.GetGraphics().device, image.backing.image,
		                     "Kyty.MRT{}.Image[guest=0x{:016x} size=0x{:x} format={}]",
		                     target.target_slot, image.info.data.address, image.info.data.size,
		                     static_cast<uint32_t>(image.info.pixel_format));
		SetVulkanObjectNameF(m_context.GetGraphics().device, image_view,
		                     "Kyty.MRT{}.View[guest=0x{:016x} mip={} layer={}+{}]",
		                     target.target_slot, image.info.data.address,
		                     target.desc.view_info.base_level, target.desc.view_info.base_layer,
		                     target.desc.view_info.layer_count);
		EXIT_IF(image.backing.samples != target.desc.info.samples || image_view == nullptr);
		if (attachment_samples == 0) {
			attachment_samples = target.desc.info.samples;
		} else if (attachment_samples != target.desc.info.samples) {
			EXIT("mixed color attachment sample counts are unsupported: %u and %u\n",
			     attachment_samples, target.desc.info.samples);
		}
		const auto& view   = target.desc.view_info;
		const auto  layout = image.binding.is_bound ? vk::ImageLayout::eGeneral
		                                            : vk::ImageLayout::eColorAttachmentOptimal;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access =
		    vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite;
		image.Transit(layout, image.binding.attachment_access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		const auto extent       = target.Extent();
		state.width             = std::min(state.width, extent.width);
		state.height            = std::min(state.height, extent.height);
		state.num_layers        = std::min(state.num_layers, view.layer_count);
		auto& attachment        = state.color_attachments[i];
		attachment.image_view   = image_view;
		attachment.image_layout = layout;
	}
	if (depth.image_id) {
		const auto owner = cache.m_slot_images.try_get(depth.image_id);
		if (owner == nullptr || !owner->registered || owner->binding.needs_rebind) {
			EXIT("depth target changed after render-state discovery\n");
		}
		const auto  image_view = cache.FindDepthTarget(depth.image_id, depth.desc);
		const auto& metadata   = depth.desc.info.metadata;
		if (metadata.kind == ImageMetadataKind::Htile && depth.depth_clear_enable &&
		    !cache.ClearMeta(metadata.range.address)) {
			EXIT("failed to acquire HTile metadata for a depth clear\n");
		}
		depth.depth_meta_clear_enable =
		    metadata.kind == ImageMetadataKind::Htile &&
		    cache.IsMetaCleared(metadata.range.address, depth.desc.view_info.base_layer);
		depth.depth_load_clear_enable = depth.depth_clear_enable || depth.depth_meta_clear_enable;
		if (depth.depth_meta_clear_enable &&
		    !cache.TouchMeta(metadata.range.address, depth.desc.view_info.base_layer, false)) {
			EXIT("failed to consume HTile clear state\n");
		}
		auto& image = cache.GetImage(depth.image_id);
		SetVulkanObjectNameF(m_context.GetGraphics().device, image.backing.image,
		                     "Kyty.DepthTarget.Image[guest=0x{:016x} size=0x{:x} format={}]",
		                     image.info.data.address, image.info.data.size,
		                     static_cast<uint32_t>(image.info.pixel_format));
		SetVulkanObjectNameF(m_context.GetGraphics().device, image_view,
		                     "Kyty.DepthTarget.View[guest=0x{:016x} layer={}+{}]",
		                     image.info.data.address, depth.desc.view_info.base_layer,
		                     depth.desc.view_info.layer_count);
		EXIT_IF(image_view == nullptr || image.backing.samples != depth.desc.info.samples);
		if (attachment_samples == 0) {
			attachment_samples = depth.desc.info.samples;
		} else if (attachment_samples != depth.desc.info.samples) {
			EXIT("mixed color/depth sample counts are unsupported: %u and %u\n", attachment_samples,
			     depth.desc.info.samples);
		}
		const bool feedback = depth.depth_write_enable && pixel &&
		    std::ranges::any_of(pixel->images, [&](const TextureBinding& binding) {
			    if (binding.image_id != depth.image_id ||
			        binding.desc.type != TextureCache::BindingType::Texture) {
				    return false;
			    }
			    const auto native =
			        std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
			    EXIT_IF(native == image.views.end());
			    const auto& sampled = native->info;
			    const auto& target = depth.desc.view_info;
			    return (sampled.aspect & vk::ImageAspectFlagBits::eDepth) &&
			           ImageRangeOverlaps(sampled.base_level, sampled.level_count,
			                              target.base_level, target.level_count) &&
			           ImageRangeOverlaps(sampled.base_layer, sampled.layer_count,
			                              target.base_layer, target.layer_count);
		    });
		if (feedback && !m_context.GetGraphics().attachment_feedback_loop_enabled) {
			EXIT("depth attachment feedback loop is not supported by the host\n");
		}
		const auto layout = feedback ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
		                             : depth_attachment_layout(depth);
		// The attachment store writes even when guest depth/stencil tests do not.
		const auto access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
		                    vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
		image.binding.attachment_layout = layout;
		image.binding.attachment_access = access;
		const auto& view                = depth.desc.view_info;
		image.Transit(layout, access,
		              ImageSubresourceRange {view.base_level, view.level_count, view.base_layer,
		                                     view.layer_count},
		              buffer.Handle());
		state.width               = std::min(state.width, depth.desc.info.extent.width);
		state.height              = std::min(state.height, depth.desc.info.extent.height);
		state.num_layers          = std::min(state.num_layers, view.layer_count);
		const auto aspects        = ImageViewOps::DepthAspectMask(depth.desc.view_info.format);
		auto&      attachment     = state.depth_stencil_attachment;
		attachment.image_view     = image_view;
		attachment.image_layout   = layout;
		attachment.clear_value[0] = std::bit_cast<uint32_t>(depth.depth_clear_value);
		attachment.clear_value[1] = depth.stencil_clear_value;
		attachment.has_depth      = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eDepth);
		attachment.depth_clear    = depth.depth_load_clear_enable;
		attachment.has_stencil    = static_cast<bool>(aspects & vk::ImageAspectFlagBits::eStencil);
		attachment.stencil_clear  = depth.stencil_clear_enable;
	}
	if (color_count == 0 && !depth.image_id) {
		const auto& limits = buffer.GetGraphics().GetPhysicalDeviceProperties().limits;
		state.width        = limits.maxFramebufferWidth;
		state.height       = limits.maxFramebufferHeight;
	} else if (attachment_samples == 0 ||
	           vulkan_sample_count(attachment_samples) == vk::SampleCountFlagBits {}) {
		EXIT("render state has no valid attachments\n");
	}
	if (state.num_layers == std::numeric_limits<uint32_t>::max()) {
		state.num_layers = 1;
	}
	EXIT_IF(state.width == 0 || state.height == 0 || state.num_layers == 0 ||
	        state.width == std::numeric_limits<uint32_t>::max() ||
	        state.height == std::numeric_limits<uint32_t>::max());
	return state;
}

static bool DrawHasActivePixelShader(const CommandBuffer& buffer) {
	const auto& ctx              = buffer.GetRegisters();
	const auto& sh_regs          = ctx.GetShaderRegisters();
	const bool  has_color_output = (ctx.GetRenderTargetMask() & sh_regs.m_cbShaderMask) != 0;
	return ShaderAddressValid(buffer.GetShaders().GetPs().ps_regs.data_addr) &&
	       (has_color_output || PixelShaderHasDepthOrCoverageSideEffects(sh_regs));
}

enum class CbColorMode : uint8_t {
	Disable            = 0,
	Normal             = 1,
	EliminateFastClear = 2,
	Resolve            = 3,
	FmaskDecompress    = 5,
	DccDecompress      = 6,
};

static bool ConsumeMetadataColorOperation(const CommandBuffer& buffer) {
	const auto& ctx  = buffer.GetRegisters();
	const auto  mode = ctx.GetColorControl().mode;
	// These special modes run color-buffer metadata or decompression operations. The shader is a
	// vehicle for that operation, and its exported color must not be applied as a normal draw.
	// Kyty stores expanded Vulkan images rather than compressed guest surfaces, so no equivalent
	// hardware pass is emitted. Tracked DCC clear state is materialized on attachment bind;
	// future CMask/FMask support can consume its state through the same TextureCache path.
	return mode == static_cast<uint8_t>(CbColorMode::EliminateFastClear) ||
	       mode == static_cast<uint8_t>(CbColorMode::FmaskDecompress) ||
	       mode == static_cast<uint8_t>(CbColorMode::DccDecompress);
}

struct DrawEmitInfo {
	bool     indexed       = false;
	int32_t  vertex_offset = 0;
	uint32_t first_vertex  = 0;
	uint32_t first_instance = 0;
};

struct DrawIndexBufferSource {
	uint64_t      address   = 0;
	const void*   host_data = nullptr;
	uint64_t      size      = 0;
	vk::IndexType type      = vk::IndexType::eUint16;
	uint32_t      guest_element_size = 0;
};

struct PreparedIndexBuffer {
	vk::Buffer     buffer = nullptr;
	vk::DeviceSize offset = 0;
	vk::IndexType  type   = vk::IndexType::eUint16;
};

static uint64_t VertexBufferDescriptorSize(const ShaderVertexInputBuffer& buffer,
                                           const ShaderVertexInputInfo& info) {
	if (buffer.stride != 0 || buffer.num_records == 0) {
		return static_cast<uint64_t>(buffer.stride) * buffer.num_records;
	}

	uint64_t size = 0;
	for (int i = 0; i < buffer.attr_num; i++) {
		const auto& resource = info.resources[buffer.attr_indices[i]];
		// RDNA2 OOB_SELECT=2 only checks NumRecords != 0. A constant attribute still
		// fetches its entire format; NumRecords is not a byte count in this mode.
		const uint64_t extent = resource.OutOfBounds() == 2
		                            ? static_cast<uint64_t>(buffer.attr_offsets[i]) +
		                                  ShaderRecompiler::Format::GetFormatInfo(resource.Format()).byte_size
		                            : buffer.num_records;
		size = std::max(size, extent);
	}
	return size;
}

struct VertexBufferRange {
	uint64_t                     base_address  = 0;
	uint64_t                     requested_end = 0;
	uint64_t                     acquired_end  = 0;
	std::pair<Buffer*, uint64_t> binding;

	[[nodiscard]] uint64_t RequestedSize() const { return requested_end - base_address; }
};

struct PreparedVertexBuffers {
	static constexpr uint32_t MaxBuffers = ShaderVertexInputInfo::RES_MAX;

	std::array<vk::Buffer, MaxBuffers>     buffers {};
	std::array<vk::DeviceSize, MaxBuffers> offsets {};
	uint32_t                               count = 0;
};

static PreparedVertexBuffers AcquireVertexBuffers(CommandBuffer&               buffer,
                                                  const ShaderVertexInputInfo& vs_input_info) {
	EXIT_IF(vs_input_info.buffers_num < 0 ||
	        vs_input_info.buffers_num > ShaderVertexInputInfo::RES_MAX);

	// Collect the non-empty guest vertex ranges.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> ranges {};
	uint32_t                                                      range_count = 0;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(vertex, vs_input_info);
		if (size == 0) {
			continue;
		}
		if (vertex.addr == 0 || size > UINT64_MAX - vertex.addr) {
			EXIT("invalid vertex buffer range: addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
			     vertex.addr, size);
		}
		ranges[range_count++] = {vertex.addr, vertex.addr + size};
	}

	std::sort(ranges.begin(), ranges.begin() + range_count,
	          [](const VertexBufferRange& left, const VertexBufferRange& right) {
		          return left.base_address < right.base_address;
	          });

	// Merge overlapping or touching ranges before acquiring host buffers.
	std::array<VertexBufferRange, ShaderVertexInputInfo::RES_MAX> merged_ranges {};
	uint32_t                                                      merged_count = 0;
	for (uint32_t i = 0; i < range_count; i++) {
		const auto& range = ranges[i];
		if (merged_count != 0 &&
		    merged_ranges[merged_count - 1].requested_end >= range.base_address) {
			merged_ranges[merged_count - 1].requested_end =
			    std::max(merged_ranges[merged_count - 1].requested_end, range.requested_end);
			continue;
		}
		merged_ranges[merged_count++] = {range.base_address, range.requested_end};
	}

	auto& cache = buffer.GetContext().GetBufferCache();
	for (uint32_t i = 0; i < merged_count; i++) {
		auto& range = merged_ranges[i];
		// PPSA20298
		const auto size =
		    Libs::LibKernel::Memory::ClampRangeSize(range.base_address, range.RequestedSize());
		range.acquired_end = range.base_address + size;
		range.binding      = cache.ObtainBuffer(range.base_address, size, false);
		SetVulkanObjectNameF(
		    buffer.GetContext().GetGraphics().device, range.binding.first->Handle(),
		    "Kyty.VertexBufferRange[guest=0x{:016x} size=0x{:x}]", range.base_address, size);
	}

	// Rebuild slot bindings, offsetting non-empty slots into their acquired merged range.
	PreparedVertexBuffers prepared;
	prepared.count         = static_cast<uint32_t>(vs_input_info.buffers_num);
	vk::Buffer null_buffer = nullptr;
	for (int i = 0; i < vs_input_info.buffers_num; i++) {
		const auto& vertex = vs_input_info.buffers[i];
		const auto  size   = VertexBufferDescriptorSize(vertex, vs_input_info);
		if (size == 0) {
			if (null_buffer == nullptr) {
				null_buffer = cache.GetBuffer(NULL_BUFFER_ID).Handle();
			}
			prepared.buffers[i] = null_buffer;
			prepared.offsets[i] = 0;
			continue;
		}

		const auto range = std::find_if(merged_ranges.begin(), merged_ranges.begin() + merged_count,
		                                [&](const VertexBufferRange& value) {
			                                return vertex.addr >= value.base_address &&
			                                       vertex.addr < value.acquired_end;
		                                });
		if (range == merged_ranges.begin() + merged_count) {
			EXIT("vertex buffer address is outside the acquired range: addr=0x%016" PRIx64 "\n",
			     vertex.addr);
		}

		prepared.buffers[i] = range->binding.first->Handle();
		prepared.offsets[i] = range->binding.second + vertex.addr - range->base_address;
		SetVulkanObjectNameF(
		    buffer.GetContext().GetGraphics().device, prepared.buffers[i],
		    "Kyty.VertexBuffer[slot={} guest=0x{:016x} size=0x{:x} stride={} records={}]", i,
		    vertex.addr, size, vertex.stride, vertex.num_records);
	}

	return prepared;
}

static void SetDrawDebugPhase(CommandBuffer& buffer, uint64_t submit_id, const DrawCallInfo& draw,
                              uint32_t phase) {
	buffer.SetDebugInfo(static_cast<uint32_t>(draw.debug_op), submit_id, phase, draw.index_count, 0,
	                    draw.instance_count, draw.first_instance);
}

static bool GetDrawTopology(const HW::UserConfig& ucfg, bool auto_draw,
                            vk::PrimitiveTopology& topology) {

	topology = vk::PrimitiveTopology::ePointList;

	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kNone: return false;
		case Prospero::PrimitiveType::kPointList:
			topology = vk::PrimitiveTopology::ePointList;
			break;
		case Prospero::PrimitiveType::kLineList: topology = vk::PrimitiveTopology::eLineList; break;
		case Prospero::PrimitiveType::kLineStrip:
			topology = vk::PrimitiveTopology::eLineStrip;
			break;
		case Prospero::PrimitiveType::kTriList:
			topology = vk::PrimitiveTopology::eTriangleList;
			break;
		case Prospero::PrimitiveType::kTriFan:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		case Prospero::PrimitiveType::kTriStrip:
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kRectList:
			topology = vk::PrimitiveTopology::ePatchList;
			break;
		case Prospero::PrimitiveType::kRectListLegacy:
			if (!auto_draw) {
				EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			topology = vk::PrimitiveTopology::eTriangleStrip;
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			topology = vk::PrimitiveTopology::eTriangleFan;
			break;
		default: {
			static std::atomic_bool logged = false;
			if (!logged.exchange(true, std::memory_order_relaxed)) {
				std::printf("Skipping draw with unknown primitive type: %u\n",
				            static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			return false;
		}
	}

	return true;
}

static bool ResolvePrimitiveRestart(const CommandBuffer& buffer, vk::PrimitiveTopology topology,
                                    uint32_t index_type_and_size) {
	const auto control = buffer.GetUserConfig().GetPrimitiveResetControl();
	EXIT_NOT_IMPLEMENTED((control & ~0x3u) != 0);
	if ((control & 0x1u) == 0) {
		return false;
	}
	switch (buffer.GetUserConfig().GetPrimType()) {
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip: break;
		default: return false;
	}
	if (topology != vk::PrimitiveTopology::eLineStrip &&
	    topology != vk::PrimitiveTopology::eTriangleStrip &&
	    topology != vk::PrimitiveTopology::eTriangleFan) {
		return false;
	}

	uint32_t index_mask = 0;
	switch (static_cast<Prospero::IndexType>(index_type_and_size)) {
		case Prospero::IndexType::kIndex8: index_mask = 0xffu; break;
		case Prospero::IndexType::kIndex16: index_mask = 0xffffu; break;
		case Prospero::IndexType::kIndex32: index_mask = 0xffffffffu; break;
		default: EXIT("unknown index_type_and_size: %u\n", index_type_and_size);
	}

	const auto reset_index = buffer.GetRegisters().GetPrimitiveResetIndex();
	if ((control & 0x2u) != 0 && (reset_index & ~index_mask) != 0) {
		return false;
	}
	EXIT_NOT_IMPLEMENTED((reset_index & index_mask) != index_mask);
	return true;
}

bool RenderExecutor::PrepareDrawRenderState(uint64_t submit_id, CommandBuffer& buffer,
                                            const DrawCallInfo& draw,
                                            uint32_t            render_target_slice_offset,
                                            bool log_setup_phases, DrawRenderState& state) {
	auto& ctx = buffer.GetRegisters();

	FrameWorkScope rtresolve_scope(FrameWorkKind::RtResolve);
	if (ResolveColorTargets(submit_id, buffer, render_target_slice_offset)) {
		return false;
	}
	if (log_setup_phases) {
		LogDrawPhase(draw.name, "ResolveRenderColorTarget");
	}
	for (uint32_t slot = 0; slot < RENDER_COLOR_ATTACHMENTS_MAX; slot++) {
		if (slot == 0 || (render_target_mask_slot(ctx.GetRenderTargetMask(), slot) != 0 &&
		                  ctx.GetRenderTarget(slot).base.addr != 0)) {
			ResolveRenderColorTarget(submit_id, buffer, state.color_info[state.color_count],
			                         render_target_slice_offset, slot);
			if (state.color_info[state.color_count].image_id) {
				state.color_count++;
			}
		}
	}
	if (log_setup_phases) {
		LogDrawPhase(draw.name, "ResolveRenderDepthTarget");
	}
	ResolveRenderDepthTarget(submit_id, buffer, state.depth_info);

	state.ps_active       = DrawHasActivePixelShader(buffer);
	if (state.color_count == 0 && !state.depth_info.image_id && !state.ps_active) {
		LogFramebufferSkip(draw.name, state.color_info[0], state.depth_info, buffer,
		                   draw.index_count, 0);
		return false;
	}

	return true;
}

static void RefreshShaders(CommandBuffer& buffer, const DrawCallInfo& draw, bool log_phases,
                           DrawRenderState& state) {
	auto& ctx    = buffer.GetRegisters();
	auto& sh_ctx = buffer.GetShaders();

	const auto& vertex_shader_info = sh_ctx.GetVs();
	const auto& pixel_shader_info  = sh_ctx.GetPs();
	const auto& shader_regs        = ctx.GetShaderRegisters();

	state.programs      = {};
	state.ps_input_info = {};
	std::array<Prospero::ColorComponentMapping, RENDER_COLOR_ATTACHMENTS_MAX>
	    target_export_mapping {};
	for (uint32_t i = 0; i < state.color_count; i++) {
		target_export_mapping[state.color_info[i].target_slot] = state.color_info[i].export_mapping;
	}
	auto& pipeline_cache = buffer.GetContext().GetPipelineCache();
	if (log_phases) {
		LogDrawPhase(draw.name, "GetGraphicsPrograms");
	}
	{
		FrameWorkScope pipeline_scope(FrameWorkKind::Pipeline);
		state.programs = pipeline_cache.GetGraphicsPrograms(
		    vertex_shader_info, pixel_shader_info, shader_regs, ctx, buffer.GetUserConfig(),
		    target_export_mapping, state.ps_active, state.vs_input_info, state.ps_input_info);
	}
}

static PreparedIndexBuffer PrepareIndexBuffer(CommandBuffer&               buffer,
                                              const DrawIndexBufferSource& source) {
	PreparedIndexBuffer prepared;
	if (source.size == 0) {
		return prepared;
	}
	prepared.type = source.type;
	if (source.host_data != nullptr) {
		auto& stream = buffer.GetContext().GetBufferCache().GetUtilityBuffer(MemoryUsage::Stream);
		prepared.offset = stream.Copy(source.host_data, source.size, 16);
		prepared.buffer = stream.Handle();
	} else {
		auto [buffer_ptr, offset] =
		    buffer.GetContext().GetBufferCache().ObtainBuffer(source.address, source.size, false);
		prepared.buffer = buffer_ptr->Handle();
		prepared.offset = offset;
	}
	if (source.host_data != nullptr) {
		SetVulkanObjectNameF(buffer.GetContext().GetGraphics().device, prepared.buffer,
		                     "Kyty.IndexBuffer[guest=transient size=0x{:x} type={}]", source.size,
		                     static_cast<uint32_t>(source.type));
	} else {
		SetVulkanObjectNameF(buffer.GetContext().GetGraphics().device, prepared.buffer,
		                     "Kyty.IndexBuffer[guest=0x{:016x} size=0x{:x} type={}]",
		                     source.address, source.size, static_cast<uint32_t>(source.type));
	}
	return prepared;
}

static void CommitVertexBuffers(vk::CommandBuffer            vk_buffer,
                                const PreparedVertexBuffers& prepared) {
	for (uint32_t i = 0; i < prepared.count; i++) {
		EXIT_IF(prepared.buffers[i] == nullptr);
	}
	if (prepared.count != 0) {
		vk_buffer.bindVertexBuffers(0, prepared.count, prepared.buffers.data(),
		                            prepared.offsets.data());
	}
}

static void CommitIndexBuffer(vk::CommandBuffer vk_buffer, const PreparedIndexBuffer& prepared) {
	if (prepared.buffer == nullptr) {
		return;
	}
	vk_buffer.bindIndexBuffer(prepared.buffer, prepared.offset, prepared.type);
}

static void LogDrawStateIfNeeded(const CommandBuffer& buffer, const DrawCallInfo& draw,
                                 const DrawRenderState& state, bool always_log,
                                 bool force_legacy_rect_log, uint32_t index_type_and_size,
                                 const void* index_addr) {
	if (!graphics_debug_dump_enabled()) {
		return;
	}

	if (!always_log && !force_legacy_rect_log) {
		return;
	}

	if (state.ps_active) {
		LogDrawTargetState(draw.name, state.color_info[0], state.depth_info, buffer,
		                   state.ps_input_info, draw.index_count, 0);
	}
	LogDrawInputState(buffer, state.color_info[0], state.vs_input_info, index_type_and_size,
	                  draw.index_count, index_addr);
}

static void EmitDrawPrimitives(const HW::UserConfig& ucfg, vk::CommandBuffer vk_buffer,
                               const ShaderVertexInputInfo& vs_input_info, const DrawCallInfo& draw,
                               const DrawEmitInfo& emit) {
	switch (ucfg.GetPrimType()) {
		case Prospero::PrimitiveType::kPointList:
		case Prospero::PrimitiveType::kLineList:
		case Prospero::PrimitiveType::kLineStrip:
		case Prospero::PrimitiveType::kTriList:
		case Prospero::PrimitiveType::kTriFan:
		case Prospero::PrimitiveType::kTriStrip:
		case Prospero::PrimitiveType::kRectList:
			if (emit.indexed) {
				vk_buffer.drawIndexed(draw.index_count, draw.instance_count, 0, emit.vertex_offset,
				                      emit.first_instance);
			} else {
				vk_buffer.draw(draw.index_count, draw.instance_count, emit.first_vertex,
				               emit.first_instance);
			}
			break;
		case Prospero::PrimitiveType::kRectListLegacy:
			if (emit.indexed) {
				EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
			}
			// Sarah
			EXIT_NOT_IMPLEMENTED(!(draw.index_count == 3 && vs_input_info.buffers_num == 0));
			vk_buffer.draw(4, draw.instance_count, emit.first_vertex, emit.first_instance);
			break;
		case Prospero::PrimitiveType::kQuadListLegacy:
			EXIT_NOT_IMPLEMENTED((draw.index_count & 0x3u) != 0);
			for (uint32_t i = 0; i < draw.index_count; i += 4) {
				if (emit.indexed) {
					vk_buffer.drawIndexed(4, draw.instance_count, i, emit.vertex_offset,
					                      emit.first_instance);
				} else {
					vk_buffer.draw(4, draw.instance_count, i + emit.first_vertex,
					               emit.first_instance);
				}
			}
			break;
		default: EXIT("unknown primitive type: %u\n", static_cast<uint32_t>(ucfg.GetPrimType()));
	}
}

void RenderExecutor::ExecutePreparedDraw(uint64_t submit_id, CommandBuffer& buffer,
                                         const DrawCallInfo& draw, DrawRenderState& state,
                                         vk::PrimitiveTopology topology, const DrawEmitInfo& emit,
                                         const DrawIndexBufferSource& index_source,
                                         bool primitive_restart_enable, bool log_pipeline_phase,
                                         bool set_bind_debug, bool set_auto_debug) {
	FrameWorkScope frame_work(FrameWorkKind::Draw);
	auto& ucfg = buffer.GetUserConfig();
	const bool mesh_active = state.vs_input_info.stage.program->stage == ShaderType::Mesh;
	uint32_t   mesh_groups = 0;
	if (mesh_active) {
		const auto& mesh = state.vs_input_info.mesh;
		if (primitive_restart_enable || mesh.primitives_per_group == 0) {
			EXIT("unsupported mesh draw: primitive=%u indexed=%u restart=%u\n",
			     static_cast<uint32_t>(ucfg.GetPrimType()), emit.indexed, primitive_restart_enable);
		}
		const auto primitives = mesh.InputPrimitiveCount(draw.index_count);
		if (primitives == 0 || draw.instance_count == 0) {
			return;
		}
		mesh_groups        = (primitives - 1u) / mesh.primitives_per_group + 1u;
		const auto& limits = m_context.GetGraphics().mesh_shader_properties;
		if (mesh_groups > limits.maxMeshWorkGroupCount[0] ||
		    draw.instance_count > limits.maxMeshWorkGroupCount[1] ||
		    static_cast<uint64_t>(mesh_groups) * draw.instance_count >
		        limits.maxMeshWorkGroupTotalCount) {
			EXIT("mesh draw exceeds host workgroup limits: %ux%u\n", mesh_groups,
			     draw.instance_count);
		}
	}

	if (mesh_active && emit.indexed) {
		// Register the original guest indices for shader reads; PrepareGraphicsBindings
		// synchronizes registered BDA ranges before any draw commands are committed.
		(void)m_context.GetBufferCache().FindBuffer(
		    index_source.address, static_cast<uint64_t>(draw.index_count) *
		                              index_source.guest_element_size);
	}
	// Draw recording measured ~53us/draw in a fight, the largest single item left in the
	// frame. Split it: descriptor-set build, vertex/index acquire, render-target acquire,
	// pipeline-cache lookup.
	const auto dp_t0 = Common::Timer::QueryPerformanceCounter();
	LogDrawPhase(draw.name, "PrepareBindings");
	auto bindings = PrepareGraphicsBindings(state.vs_input_info.stage, state.ps_input_info.stage,
	                                        state.ps_active);
	const auto            dp_t1 = Common::Timer::QueryPerformanceCounter();
	PreparedVertexBuffers vertex_bindings;
	PreparedIndexBuffer   index_binding;
	if (!mesh_active) {
		LogDrawPhase(draw.name, "PrepareVertexBuffers");
		vertex_bindings = AcquireVertexBuffers(buffer, state.vs_input_info);
		index_binding   = PrepareIndexBuffer(buffer, index_source);
	}
	const auto dp_t2 = Common::Timer::QueryPerformanceCounter();
	const auto rendering =
	    AcquireRenderTargets(buffer, state.color_info, state.color_count, state.depth_info,
	                         bindings.pixel);
	const auto dp_t3 = Common::Timer::QueryPerformanceCounter();

	if (log_pipeline_phase) {
		LogDrawPhase(draw.name, "CreatePipeline");
	}
	auto& pipeline = m_context.GetPipelineCache().CreateGraphicsPipeline(
	    std::span {state.color_info, state.color_count}, state.depth_info, state.vs_input_info, buffer,
	    state.ps_active ? &state.ps_input_info : nullptr, topology, primitive_restart_enable,
	    state.programs.vertex, state.programs.pixel);
	{
		const auto dp_t4 = Common::Timer::QueryPerformanceCounter();
		static std::atomic<uint32_t> n {0};
		static std::atomic<uint64_t> bind_ns {0}, vbuf_ns {0}, rt_ns {0}, pipe_ns {0};
		const auto                   freq = Common::Timer::QueryPerformanceFrequency();
		const auto ns = [freq](uint64_t a, uint64_t b) {
			return freq == 0 ? 0ull : (b - a) * 1000000000ull / freq;
		};
		bind_ns.fetch_add(ns(dp_t0, dp_t1), std::memory_order_relaxed);
		vbuf_ns.fetch_add(ns(dp_t1, dp_t2), std::memory_order_relaxed);
		rt_ns.fetch_add(ns(dp_t2, dp_t3), std::memory_order_relaxed);
		pipe_ns.fetch_add(ns(dp_t3, dp_t4), std::memory_order_relaxed);
		if ((n.fetch_add(1, std::memory_order_relaxed) % 8192) == 8191) {
			LOGF("DrawPhase/8192: bindings=%.1fus vbuf=%.1fus rendertargets=%.1fus "
			     "pipeline=%.1fus\n",
			     bind_ns.exchange(0) / 8192.0 / 1000.0, vbuf_ns.exchange(0) / 8192.0 / 1000.0,
			     rt_ns.exchange(0) / 8192.0 / 1000.0, pipe_ns.exchange(0) / 8192.0 / 1000.0);
		}
	}

	// Resource preparation above may synchronously finish and restart the scheduler. From this
	// point onward, every operation targets the current command buffer and cannot touch guest
	// memory.
	auto vk_buffer = buffer.Handle();
	if (set_bind_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x100u);
	}
	if (set_auto_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x200u);
	}
	if (!mesh_active) {
		CommitVertexBuffers(vk_buffer, vertex_bindings);
	}
	if (bindings.pixel.has_value()) {
		if (set_auto_debug) {
			SetDrawDebugPhase(buffer, submit_id, draw, 0x300u);
		}
	}
	std::array<PreparedBindings*, 2> descriptor_stages {&bindings.vertex, nullptr};
	const size_t                     descriptor_stage_count = bindings.pixel.has_value() ? 2u : 1u;
	if (bindings.pixel) {
		descriptor_stages[1] = &*bindings.pixel;
	}
	CommitBindings(buffer, vk::PipelineBindPoint::eGraphics, pipeline,
	               std::span {descriptor_stages.data(), descriptor_stage_count});
	if (mesh_active) {
		const uint32_t draw_data[] {
		    draw.index_count,
		    emit.indexed ? static_cast<uint32_t>(emit.vertex_offset) : emit.first_vertex,
		    emit.first_instance, index_source.guest_element_size,
		    static_cast<uint32_t>(index_source.address),
		    static_cast<uint32_t>(index_source.address >> 32u)};
		static_assert(std::size(draw_data) == ShaderRecompiler::IR::PushData::MeshDrawDwordCount);
		vk_buffer.pushConstants(pipeline.pipeline_layout,
		                        vk::ShaderStageFlagBits::eMeshEXT |
		                            vk::ShaderStageFlagBits::eFragment,
		                        0, sizeof(draw_data), draw_data);
	} else {
		CommitIndexBuffer(vk_buffer, index_binding);
	}

	SetGraphicsDynamicParams(buffer, vk_buffer, state.vs_input_info, state.color_info,
	                         state.color_count, state.depth_info);
	if (m_context.GetGraphics().attachment_feedback_loop_enabled) {
		vk_buffer.setAttachmentFeedbackLoopEnableEXT(
		    rendering.depth_stencil_attachment.image_layout ==
		            vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
		        ? vk::ImageAspectFlags {vk::ImageAspectFlagBits::eDepth}
		        : vk::ImageAspectFlags {});
	}

	LogDrawPhase(draw.name, "BeginRendering");
	if (set_auto_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x400u);
	}
	m_context.GetCommandScheduler().BeginRendering(rendering);
	vk_buffer.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.pipeline);
	if (set_auto_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x500u);
	}
	const uint64_t draw_vs_hash = state.vs_input_info.stage.program != nullptr
	                                  ? state.vs_input_info.stage.program->shader_hash
	                                  : uint64_t {0};
	const uint64_t draw_ps_hash = state.ps_active &&
	                                      state.ps_input_info.stage.program != nullptr
	                                  ? state.ps_input_info.stage.program->shader_hash
	                                  : uint64_t {0};
	if (ShouldTrackDrawTargets()) {
		const auto frame_num = static_cast<uint32_t>(buffer.GetContext().GetGpu().GetFrameNum());
		for (uint32_t i = 0; i < state.color_count; i++) {
			TrackDrawTarget(state.color_info[i].desc.info.data.address,
			                state.color_info[i].image_id, frame_num);
		}
	}
	const bool large_draw_target = state.color_count != 0 &&
	    state.color_info[0].Extent().width >= 1280u &&
	    state.color_info[0].Extent().height >= 720u;
	if (large_draw_target && ShouldCensusLargeDrawTargets()) {
		const auto frame_num = static_cast<uint32_t>(buffer.GetContext().GetGpu().GetFrameNum());
		if ((frame_num % 30u) == 0u) {
			static std::mutex census_lock;
			static uint32_t census_frame = UINT32_MAX;
			static std::unordered_set<uint64_t> census_seen;
			std::scoped_lock guard {census_lock};
			if (census_frame != frame_num) {
				census_frame = frame_num;
				census_seen.clear();
			}
			const auto& color = state.color_info[0];
			const auto  key = draw_ps_hash ^ std::rotl(color.desc.info.data.address, 17);
			if (census_seen.size() < 128 && census_seen.insert(key).second) {
				const auto& depth = state.depth_info;
				const auto& blend = buffer.GetRegisters().GetBlendControl(color.target_slot);
				const auto  extent = color.Extent();
				LOGF("LargeDrawTarget: frame=%u vs=0x%016" PRIx64 " ps=0x%016" PRIx64
				     " addr=0x%016" PRIx64 " img=%u extent=%ux%u slot=%u"
				     " depth_img=%u depth_addr=0x%016" PRIx64 " depth_fmt=%d"
				     " depth_test=%d depth_write=%d depth_op=%d stencil=%d"
				     " kill=%d color_mask=0x%x out_mode=%u blend=%d src=%u dst=%u comb=%u"
				     " indices=%u instances=%u force_depth_off=%d\n",
				     frame_num, draw_vs_hash, draw_ps_hash, color.desc.info.data.address,
				     color.image_id.index, extent.width, extent.height, color.target_slot,
				     depth.image_id.index, depth.desc.info.data.address,
				     static_cast<int>(depth.desc.view_info.format),
				     depth.depth_test_enable ? 1 : 0, depth.depth_write_enable ? 1 : 0,
				     static_cast<int>(depth.depth_compare_op),
				     depth.stencil_test_enable ? 1 : 0,
				     state.ps_active && state.ps_input_info.ps_pixel_kill_enable ? 1 : 0,
				     (buffer.GetRegisters().GetRenderTargetMask() >> (color.target_slot * 4u)) & 0xfu,
				     state.ps_active ? state.ps_input_info.target_output_mode[color.target_slot] : 0u,
				     blend.enable ? 1 : 0, blend.color_srcblend, blend.color_destblend,
				     blend.color_comb_fcn, draw.index_count, draw.instance_count,
				     ShouldForceLargeTargetDepthOff() ? 1 : 0);
			}
		}
		if (ShouldForceLargeTargetDepthOff()) {
			vk_buffer.setDepthTestEnable(VK_FALSE);
			vk_buffer.setStencilTestEnable(VK_FALSE);
		}
	}
	const bool watched_fight_target =
	    draw_ps_hash != 0 && IsWatchedDrawPixelShader(draw_ps_hash) && state.color_count != 0 &&
	    state.color_info[0].desc.info.data.address == 0x0000001168360000ull;
	if (watched_fight_target) {
		const auto frame_num = static_cast<uint32_t>(buffer.GetContext().GetGpu().GetFrameNum());
		static std::atomic<uint32_t> last_state_frame {UINT32_MAX};
		if ((frame_num % 30u) == 0u &&
		    last_state_frame.exchange(frame_num, std::memory_order_relaxed) != frame_num) {
			const auto& depth = state.depth_info;
			const auto& blend = buffer.GetRegisters().GetBlendControl(0);
			LOGF("WatchedDrawState: frame=%u ps=0x%016" PRIx64
			     " depth_img=%u depth_addr=0x%016" PRIx64 " depth_fmt=%d"
			     " depth_test=%d depth_write=%d depth_op=%d stencil=%d"
			     " kill=%d color_mask=0x%08" PRIx32 " out_mode=%u"
			     " blend=%d src=%u dst=%u comb=%u indices=%u instances=%u force_depth_off=%d\n",
			     frame_num, draw_ps_hash, depth.image_id.index, depth.desc.info.data.address,
			     static_cast<int>(depth.desc.view_info.format), depth.depth_test_enable ? 1 : 0,
			     depth.depth_write_enable ? 1 : 0, static_cast<int>(depth.depth_compare_op),
			     depth.stencil_test_enable ? 1 : 0,
			     state.ps_input_info.ps_pixel_kill_enable ? 1 : 0,
			     buffer.GetRegisters().GetRenderTargetMask(),
			     state.ps_input_info.target_output_mode[0], blend.enable ? 1 : 0,
			     blend.color_srcblend, blend.color_destblend, blend.color_comb_fcn,
			     draw.index_count, draw.instance_count, ShouldForceWatchedDepthOff() ? 1 : 0);
		}
		if (ShouldForceWatchedDepthOff()) {
			vk_buffer.setDepthTestEnable(VK_FALSE);
		}
	}
	// Which colour target do the UFC5 pixel ubershaders actually draw into? Surface dumps show
	// both dominant in-fight 1600x900 targets GPU-modified but containing EXACTLY zero, while
	// GpuDraws shows these shaders costing 95-262us/draw - i.e. real per-pixel work. Those two
	// facts cannot both describe the same surface, so log the bound attachment for these draws
	// and find out which surface they write. Rate-limited; same watch list as the timestamps.
	if (draw_ps_hash != 0 && IsWatchedDrawPixelShader(draw_ps_hash)) {
		static std::atomic<uint32_t> logged {0};
		if (logged.fetch_add(1, std::memory_order_relaxed) < 48) {
			std::string targets;
			for (uint32_t i = 0; i < state.color_count; i++) {
				const auto& color = state.color_info[i];
				const auto  ext   = color.Extent();
				// Log the actual ImageId, not a bool. rt68360000 dumps byte-identical across
				// hundreds of frames while the game is live, so either these draws resolve a
				// DIFFERENT cache image than the dump/compositor does for the same guest
				// address (the FindImage alias family), or the draws stopped landing. The id
				// is what separates those.
				targets += fmt::format(" [{}]=0x{:016x} {}x{} fmt={} img={} mip={} layer={}", i,
				                       color.desc.info.data.address, ext.width, ext.height,
				                       static_cast<uint32_t>(color.desc.info.pixel_format),
				                       color.image_id.index, color.guest_mip_level,
				                       color.guest_array_layer);
			}
			LOGF("WatchedDrawTarget: ps=0x%016" PRIx64 " colors=%u depth=%d%s\n", draw_ps_hash,
			     state.color_count, state.depth_info.image_id ? 1 : 0, targets.c_str());
		}
	}
	// Live scene-draw bisector. See sceneDrawDebug.h: the fight round draws a black shape over
	// the centre, and this finds which draw does it by binary search driven from host keys,
	// instead of a rebuild-and-renavigate cycle per step.
	bool scene_draw_skipped = false;
	bool scene_draw_hidden  = false;
	if (state.color_count > 0 && state.color_info[0].image_id &&
	    state.color_info[0].desc.info.data.address == SceneDrawDebug::Target()) {
		const auto& color0 = state.color_info[0];
		const auto  ext    = color0.Extent();
		SceneDrawDebug::Entry entry;
		entry.vs_hash        = draw_vs_hash;
		entry.ps_hash        = draw_ps_hash;
		entry.index_count    = draw.index_count;
		entry.instance_count = draw.instance_count;
		entry.topology       = static_cast<uint32_t>(topology);
		entry.target_width   = ext.width;
		entry.target_height  = ext.height;
		entry.image_id       = color0.image_id.index;
		entry.depth_test     = state.depth_info.depth_test_enable;
		entry.depth_write    = state.depth_info.depth_write_enable;
		const auto id = SceneDrawDebug::NoteDraw(m_context.GetGpu().GetFrameNum(), entry);
		if (SceneDrawDebug::ShouldSuppress(id)) {
			// Skip drops the draw entirely, which also removes its depth writes. Hide keeps
			// depth behaviour and only stops colour. The difference distinguishes "this draw
			// paints the black" from "this draw's depth hides everything behind it".
			if (SceneDrawDebug::CurrentMode() == SceneDrawDebug::Mode::Skip) {
				scene_draw_skipped = true;
			} else {
				scene_draw_hidden = true;
			}
		}
	}
#if !defined(__APPLE__)
	if (scene_draw_hidden && state.color_count != 0) {
		// The pipeline already declares eColorWriteEnableEXT as dynamic state (shaders.cpp),
		// so disabling colour writes needs no pipeline work.
		vk::Bool32 disable[RENDER_COLOR_ATTACHMENTS_MAX] = {};
		vk_buffer.setColorWriteEnableEXT(state.color_count, disable);
	}
#endif
	GpuTimestamps::Instance().BeginDraw(vk_buffer, draw_vs_hash, draw_ps_hash);
	// Bracket the skipped draw too, so GpuDraws still reports it - at ~0us instead of ~21000us.
	if (!ShouldSkipPixelShaderHash(draw_ps_hash) && !scene_draw_skipped) {
		if (mesh_active) {
			vk_buffer.drawMeshTasksEXT(mesh_groups, draw.instance_count, 1);
		} else {
			EmitDrawPrimitives(ucfg, vk_buffer, state.vs_input_info, draw, emit);
		}
	}
	GpuTimestamps::Instance().EndDraw(vk_buffer);
#if !defined(__APPLE__)
	if (scene_draw_hidden && state.color_count != 0) {
		// Restore the per-target write mask the next draw expects. Leaving colour writes off
		// would silently hide every later draw in this render pass and make the bisection lie.
		const auto& restore_ctx                          = buffer.GetRegisters();
		vk::Bool32  restore[RENDER_COLOR_ATTACHMENTS_MAX] = {};
		for (uint32_t i = 0; i < state.color_count; i++) {
			restore[i] = render_target_mask_slot(restore_ctx.GetRenderTargetMask(),
			                                     state.color_info[i].target_slot) != 0
			                 ? VK_TRUE
			                 : VK_FALSE;
		}
		vk_buffer.setColorWriteEnableEXT(state.color_count, restore);
	}
#endif

	if (set_auto_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x600u);
	}
	vk::PipelineStageFlags shader_write_stages = {};
	if (HasShaderBufferWrites(state.vs_input_info.stage)) {
		shader_write_stages |= mesh_active ? vk::PipelineStageFlagBits::eMeshShaderEXT
		                                   : vk::PipelineStageFlagBits::eVertexShader;
	}
	if (state.ps_active && HasShaderBufferWrites(state.ps_input_info.stage)) {
		shader_write_stages |= vk::PipelineStageFlagBits::eFragmentShader;
	}
	if (shader_write_stages) {
		m_context.GetCommandScheduler().EndRendering();
		ShaderWriteBarrier(vk_buffer, shader_write_stages);
	}
	LogDrawPhase(draw.name, "DrawComplete");
	if (set_auto_debug) {
		SetDrawDebugPhase(buffer, submit_id, draw, 0x700u);
	}
}

void RenderExecutor::DrawIndex(uint64_t submit_id, CommandBuffer& buffer,
                               const DrawIndexArgs& args) {
	KYTY_PROFILER_FUNCTION();
	FrameWorkScope draw_prep(FrameWorkKind::DrawPrep);

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndex), submit_id,
	                    args.index_count, 0, 1, args.instance_count,
	                    reinterpret_cast<uint64_t>(args.index_addr));

	Common::LockGuard lock(m_context.GetMutex());
	if (args.index_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		sh_print("GraphicsRenderDrawIndex():Shader:", sh_ctx);
		uc_print("GraphicsRenderDrawIndex():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndex():Parameters:\n"
		     "\t index_type_and_size = 0x%08" PRIx32 "\n"
		     "\t index_count         = 0x%08" PRIx32 "\n"
		     "\t index_addr          = 0x%016" PRIx64 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t base_vertex         = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.index_type_and_size, args.index_count,
		     reinterpret_cast<uint64_t>(args.index_addr), args.instance_count,
		     static_cast<uint32_t>(args.base_vertex), args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, false, topology)) {
		return;
	}

	vk::IndexType index_type           = vk::IndexType::eUint16;
	uint64_t      index_size           = 0;
	bool          expand_index8_to_u16 = false;
	const bool    primitive_restart =
	    ResolvePrimitiveRestart(buffer, topology, args.index_type_and_size);

	switch (static_cast<Prospero::IndexType>(args.index_type_and_size)) {
		case Prospero::IndexType::kIndex16:
			index_type = vk::IndexType::eUint16;
			index_size = 2 * static_cast<uint64_t>(args.index_count);
			break;
		case Prospero::IndexType::kIndex32:
			index_type = vk::IndexType::eUint32;
			index_size = 4 * static_cast<uint64_t>(args.index_count);
			break;
		// Some games use it - need vulkan extension
		case Prospero::IndexType::kIndex8:
			index_type           = vk::IndexType::eUint16;
			index_size           = static_cast<uint64_t>(args.index_count);
			expand_index8_to_u16 = true;
			break;
		default: EXIT("unknown index_type_and_size: %u\n", args.index_type_and_size);
	}

	const DrawCallInfo    draw {"DrawIndex", CommandBufferDebugOp::DrawIndex, args.index_count,
	                            args.instance_count, args.first_instance};
	std::vector<uint16_t> expanded_indices;
	if (expand_index8_to_u16) {
		EXIT_NOT_IMPLEMENTED(args.index_addr == nullptr);
		const auto* src = static_cast<const uint8_t*>(args.index_addr);
		expanded_indices.resize(args.index_count);
		for (uint32_t i = 0; i < args.index_count; i++) {
			expanded_indices[i] = primitive_restart && src[i] == 0xffu ? 0xffffu : src[i];
		}
	}

	DrawIndexBufferSource index_source {};
	index_source.address = reinterpret_cast<uint64_t>(args.index_addr);
	index_source.host_data =
	    expanded_indices.empty() ? nullptr : static_cast<const void*>(expanded_indices.data());
	index_source.size =
	    expanded_indices.empty() ? index_size : expanded_indices.size() * sizeof(uint16_t);
	index_source.type = index_type;
	index_source.guest_element_size = static_cast<uint32_t>(index_size / args.index_count);

	DrawRenderState state {};
	if (!PrepareDrawRenderState(submit_id, buffer, draw, args.render_target_slice_offset, true,
	                            state)) {
		ResetBindings();
		return;
	}

	RefreshShaders(buffer, draw, true, state);

	LogDrawStateIfNeeded(buffer, draw, state, true, false, args.index_type_and_size,
	                     args.index_addr);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto vertex_offset =
	    indirect
	        ? args.base_vertex
	        : ResolveVertexOffset(ucfg.GetIndexOffset(), state.vs_input_info) + args.base_vertex;

	DrawEmitInfo emit {};
	emit.indexed       = true;
	emit.vertex_offset = vertex_offset;
	emit.first_instance =
	    indirect ? args.first_instance : ResolveInstanceOffset(state.vs_input_info);

	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source,
	                    primitive_restart, true, true, false);
	ResetBindings();
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void RenderExecutor::DrawAuto(uint64_t submit_id, CommandBuffer& buffer, const DrawAutoArgs& args) {
	KYTY_PROFILER_FUNCTION();
	FrameWorkScope draw_prep(FrameWorkKind::DrawPrep);

	EXIT_IF(buffer.IsInvalid());
	EXIT_IF(args.offset_source == DrawOffsetSource::DrawState && args.first_instance != 0);
	m_context.GetCommandScheduler().PopPendingOperations();
	auto& ucfg   = buffer.GetUserConfig();
	auto& sh_ctx = buffer.GetShaders();

	buffer.SetDebugInfo(static_cast<uint32_t>(CommandBufferDebugOp::DrawIndexAuto), submit_id,
	                    args.vertex_count, 0, args.first_vertex, args.instance_count,
	                    args.first_instance);

	Common::LockGuard lock(m_context.GetMutex());
	if (args.vertex_count == 0 || args.instance_count == 0) {
		return;
	}

	if (ConsumeMetadataColorOperation(buffer)) {
		ResetBindings();
		return;
	}

	if (!DrawHasValidVertexShader(sh_ctx)) {
		return;
	}

	if (graphics_debug_dump_enabled()) {
		sh_print("GraphicsRenderDrawIndexAuto():Shader:", sh_ctx);
		uc_print("GraphicsRenderDrawIndexAuto():UserConfig:", ucfg);
		hw_print(buffer);

		LOGF("GraphicsRenderDrawIndexAuto():Parameters:\n"
		     "\t vertex_count        = 0x%08" PRIx32 "\n"
		     "\t instance_count      = 0x%08" PRIx32 "\n"
		     "\t first_vertex        = 0x%08" PRIx32 "\n"
		     "\t first_instance      = 0x%08" PRIx32 "\n",
		     args.vertex_count, args.instance_count, args.first_vertex, args.first_instance);
	}

	uc_check(ucfg);

	hw_check(buffer);

	const DrawCallInfo draw {"DrawIndexAuto", CommandBufferDebugOp::DrawIndexAuto,
	                         args.vertex_count, args.instance_count, args.first_instance};

	DrawRenderState state {};
	if (!PrepareDrawRenderState(submit_id, buffer, draw, args.render_target_slice_offset, false,
	                            state)) {
		ResetBindings();
		return;
	}

	vk::PrimitiveTopology topology = vk::PrimitiveTopology::ePointList;
	if (!GetDrawTopology(ucfg, true, topology)) {
		ResetBindings();
		return;
	}
	RefreshShaders(buffer, draw, false, state);

	const bool rect_list = topology == vk::PrimitiveTopology::ePatchList;
	if (rect_list && state.vs_input_info.buffers_num == 0 &&
	    state.vs_input_info.stage.program->param_export_mask == 0 &&
	    state.ps_input_info.input_num != 0) {
		if (graphics_debug_dump_enabled()) {
			LOGF("DrawIndexAuto: skipping rect-list draw with no VS param exports and PS inputs: "
			     "ps_inputs=%u ps=0x%016" PRIx64 " es=0x%016" PRIx64 " gs=0x%016" PRIx64 "\n",
			     state.ps_input_info.input_num, sh_ctx.GetPs().ps_regs.data_addr,
			     sh_ctx.GetVs().es_regs.data_addr, sh_ctx.GetVs().gs_regs.data_addr);
		}
		ResetBindings();
		return;
	}

	LogDrawStateIfNeeded(buffer, draw, state, false,
	                     ucfg.GetPrimType() == Prospero::PrimitiveType::kRectListLegacy, 0,
	                     nullptr);

	const bool indirect = args.offset_source == DrawOffsetSource::IndirectArgs;
	const auto vertex_offset =
	    indirect ? static_cast<int32_t>(args.first_vertex)
	             : ResolveVertexOffset(ucfg.GetIndexOffset(), state.vs_input_info) +
	                   static_cast<int32_t>(args.first_vertex);
	DrawEmitInfo emit {};
	emit.first_vertex = static_cast<uint32_t>(vertex_offset);
	emit.first_instance =
	    indirect ? args.first_instance : ResolveInstanceOffset(state.vs_input_info);

	DrawIndexBufferSource index_source {};
	ExecutePreparedDraw(submit_id, buffer, draw, state, topology, emit, index_source, false, false,
	                    false, true);
	ResetBindings();
}

bool RenderExecutor::ResolveColorTargets(uint64_t submit_id, CommandBuffer& buffer,
                                         uint32_t render_target_slice_offset) {
	const auto& hw = buffer.GetRegisters();
	if (hw.GetColorControl().mode != 3) {
		return false;
	}

	const auto& src_rt = hw.GetRenderTarget(0);
	const auto& dst_rt = hw.GetRenderTarget(1);
	if (src_rt.base.addr == 0 || dst_rt.base.addr == 0) {
		return false;
	}

	RenderColorInfo src {};
	RenderColorInfo dst {};
	ResolveRenderColorTarget(submit_id, buffer, src, render_target_slice_offset, 0, true, true);
	ResolveRenderColorTarget(submit_id, buffer, dst, render_target_slice_offset, 1, true, true);
	if (!src.image_id || !dst.image_id) {
		return false;
	}
	if (src.desc.info.data.address == dst.desc.info.data.address &&
	    src.guest_mip_level == dst.guest_mip_level &&
	    src.guest_array_layer == dst.guest_array_layer) {
		return true;
	}

	auto& cache = m_context.GetTextureCache();
	cache.MarkGpuWritten(dst.image_id);
	auto& source      = cache.GetImage(src.image_id);
	auto& destination = cache.GetImage(dst.image_id);
	destination.Resolve(source, {src.guest_mip_level, 1, src.guest_array_layer, 1},
	                    {dst.guest_mip_level, 1, dst.guest_array_layer, 1});
	return true;
}

} // namespace Libs::Graphics
