#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEBUG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEBUG_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics {

class CommandBuffer;
class RenderContext;
class Image;

namespace HW {
class Context;
class Shader;
class UserConfig;
struct RenderTarget;
struct ScanModeControl;
struct ScreenViewport;
} // namespace HW

struct ScissorRect {
	int left   = 0;
	int top    = 0;
	int right  = 0;
	int bottom = 0;
};

uint32_t                 render_target_mask_slot(uint32_t mask, uint32_t slot);
uint32_t                 render_target_first_bound_slot(const CommandBuffer& buffer);
bool                     graphics_debug_dump_enabled();
// Opt-in resource provenance. KYTY_TRACE_RESOURCES is a comma-separated list of
// guest addresses; log the first occurrence of each distinct binding only.
bool                     TraceResourceAddress(uint64_t address, uint64_t size);
void                     TraceResourceBinding(uint64_t frame, const std::string& binding);
// Capture the first binding of each selected hash at/after KYTY_CAPTURE_INPUTS_FRAME.
bool                     CaptureShaderInputs(uint64_t hash, uint64_t frame);
void                     DumpShaderInput(CommandBuffer& command, RenderContext& renderer,
                                         Image& image, const std::string& tag);
void                     DumpShaderBufferInput(CommandBuffer& command, RenderContext& renderer,
                                               vk::Buffer buffer, uint64_t offset, uint64_t size,
                                               const std::string& tag);
void                     uc_print(const char* func, const HW::UserConfig& uc);
void                     uc_check(const HW::UserConfig& uc);
void                     sh_print(const char* func, const HW::Shader& uc);
std::vector<std::string> rt_print(const char* func, const HW::RenderTarget& rt);
bool                     RenderIsColorTileModeLinear(Prospero::TileMode tile_mode);
void                     hw_print(const CommandBuffer& buffer);
void                     hw_check(const CommandBuffer& buffer);
void                     LogDrawPhase(const char* draw_name, const char* phase);
ScissorRect calc_final_scissor(const HW::ScreenViewport& vp, const HW::ScanModeControl& smc,
                               vk::Extent2D extent, uint32_t viewport_index);

enum class FrameWorkKind : uint8_t {
	Draw,
	Dispatch,
	Submit,
	Finish,
	Present,
	Process,  // whole GuestGpu::Process(submission) on the GPU worker thread
	Gc,       // RunGarbageCollector
	Flush,    // CommandProcessor::BufferFlush
	SendCmd,  // draining cross-thread SendCommand callbacks (readbacks etc.)
	DrawPrep, // whole RenderExecutor::DrawIndex/Auto (nests Draw): pipeline lookup,
	          // descriptor resolve, FindImage/FindBuffer, hw_check
	FaultBuf, // BufferCache::ProcessFaultBuffer (readback-via-page-fault, nests inside Gc)
	HwCheck,  // hw_check + uc_check per-draw state validation (nests in DrawPrep)
	Pipeline, // PipelineCache::GetGraphicsPrograms per draw (nests in DrawPrep)
	RtResolve,// Resolve{Color,Depth}Target per draw (nests in DrawPrep)
};

struct FrameWorkPulse {
	uint32_t draws       = 0;
	uint32_t dispatches  = 0;
	uint32_t submits     = 0;
	uint32_t finishes    = 0;
	uint32_t presents    = 0;
	uint32_t processes   = 0;
	uint32_t gcs         = 0;
	uint32_t flushes     = 0;
	uint32_t sendcmds    = 0;
	uint32_t drawpreps   = 0;
	uint32_t faultbufs   = 0;
	uint32_t hwchecks    = 0;
	double   draw_ms     = 0.0;
	double   dispatch_ms = 0.0;
	double   submit_ms   = 0.0;
	double   finish_ms   = 0.0;
	double   present_ms  = 0.0;
	double   process_ms  = 0.0;
	double   gc_ms       = 0.0;
	double   flush_ms    = 0.0;
	double   sendcmd_ms  = 0.0;
	double   drawprep_ms = 0.0;
	double   faultbuf_ms = 0.0;
	double   hwcheck_ms  = 0.0;
	double   pipeline_ms = 0.0;
	double   rtresolve_ms = 0.0;
};

[[nodiscard]] FrameWorkPulse ConsumeFrameWorkPulse();

class FrameWorkScope {
public:
	explicit FrameWorkScope(FrameWorkKind kind);
	~FrameWorkScope();
	FrameWorkScope(const FrameWorkScope&)            = delete;
	FrameWorkScope& operator=(const FrameWorkScope&) = delete;

private:
	FrameWorkKind kind_;
	uint64_t      start_;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DEBUG_H_
