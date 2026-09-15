#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_MIPTAILDIAGNOSTIC_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_MIPTAILDIAGNOSTIC_H_

#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/shader/shaderBindings.h"

#include <cstdint>

namespace Libs::Graphics {

// Diagnostic-only helpers for guest logical mip counts that exceed the Vulkan
// complete mip chain (floor(log2(max_dim))+1). No behaviour change / no clamp.
void LogGuestMipLayoutChain(uint64_t address, Prospero::BufferFormat format,
                            Prospero::TileMode tile, uint32_t width, uint32_t height,
                            uint32_t depth, uint32_t levels, bool volume);

void LogGuestHostMipMismatch(const char* where, uint64_t address, const ImageInfo& guest,
                             uint32_t host_mip_levels, uint32_t requested_base,
                             uint32_t requested_count);

void NoteOverwideTextureResolve(uint64_t frame, uint64_t shader_hash, uint32_t stage,
                                uint32_t slot, bool read, bool written, bool atomic, bool storage,
                                bool r128, uint32_t mip_mode, const ShaderTextureResource& descriptor,
                                uint32_t guest_levels, uint32_t complete_levels,
                                const ImageInfo* populated);

void FlushOverwideTexturePrepare(uint64_t frame, uint64_t shader_hash, uint32_t stage);

} // namespace Libs::Graphics

#endif
