#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WATCHEDIMAGETRACE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WATCHEDIMAGETRACE_H_

#include "common/slotVector.h"

#include <cstdint>
#include <string>

namespace Libs::Graphics {

class Image;
struct ImageViewInfo;

enum class WatchedImageEvent : uint8_t {
	Create,
	Upload,
	Materialize,
	SampleBind,
	StorageBind,
	RenderTargetBind,
	DepthTargetBind,
	CopySrc,
	CopyDst,
	Clear,
	GpuWrite,
	CpuWrite,
	Download,
	AliasResolve,
	Retarget,
	Recreate,
	Delete,
	GcConsider,
	GcSkip,
	GcDelete,
	ViewCreate,
	ViewReuse,
	ViewChange,
	SnapshotGood,
	SnapshotBad,
	BreakBefore,
	BreakAfter,
};

struct WatchedImageState {
	uint64_t frame              = 0;
	uint32_t op_index           = UINT32_MAX;
	uint64_t guest_addr         = 0;
	uint64_t guest_size         = 0;
	uint32_t image_index        = Common::SlotId::INVALID_INDEX;
	uint32_t image_generation   = 0;
	uint64_t vk_image           = 0;
	uint64_t vk_view            = 0;
	uint32_t guest_width        = 0;
	uint32_t guest_height       = 0;
	uint32_t guest_depth        = 0;
	uint32_t guest_levels       = 0;
	uint32_t native_levels      = 0;
	uint32_t guest_layers       = 0;
	uint32_t guest_mip          = UINT32_MAX;
	uint32_t host_mip           = UINT32_MAX;
	uint32_t layer              = UINT32_MAX;
	uint32_t level_count        = 0;
	uint32_t layer_count        = 0;
	int32_t  guest_format       = -1;
	int32_t  vk_format          = -1;
	int32_t  view_format        = -1;
	int32_t  view_type          = -1;
	uint32_t tile_mode          = 0;
	uint64_t content_generation = 0;
	uint64_t last_writer_shader = 0;
	uint64_t last_writer_tick   = 0;
	uint32_t alias_count        = 0;
	bool     cpu_dirty          = false;
	bool     gpu_modified       = false;
	bool     buffer_modified    = false;
	char     owner[48]          = {};
	char     last_writer[96]    = {};
};

[[nodiscard]] bool WatchedImageEnabled() noexcept;
[[nodiscard]] bool WatchedImageMatches(uint64_t guest_addr, uint64_t guest_size,
                                       Common::SlotId image_id = {}) noexcept;
[[nodiscard]] bool WatchedImageMatchesImage(const Image& image,
                                            Common::SlotId image_id = {}) noexcept;

void WatchedImageSetAddress(uint64_t guest_addr) noexcept;
void WatchedImageSetImageId(uint32_t index, uint32_t generation) noexcept;
void WatchedImageClearWatch() noexcept;
void WatchedImageArmBreakOnWrite() noexcept;
void WatchedImageArmSnapshotGood() noexcept;
void WatchedImageArmSnapshotBad() noexcept;
void WatchedImageRefreshTriggers() noexcept;

void WatchedImageSetContext(uint64_t frame, uint32_t op_index, uint64_t shader_hash,
                            const char* owner) noexcept;

void WatchedImageNote(WatchedImageEvent event, const Image* image, Common::SlotId image_id,
                      const char* detail = nullptr, uint32_t mip = UINT32_MAX,
                      uint32_t layer = UINT32_MAX, uint64_t vk_view = 0,
                      const ImageViewInfo* view = nullptr);

void WatchedImageNoteBind(WatchedImageEvent event, Image& image, Common::SlotId image_id,
                          uint64_t vk_view, const ImageViewInfo& view, uint64_t shader_hash,
                          const char* owner);

void WatchedImageBumpContent(Image& image, Common::SlotId image_id, WatchedImageEvent event,
                             const char* writer, uint32_t mip = UINT32_MAX,
                             uint32_t layer = UINT32_MAX);

void WatchedImageNoteAliasOverlap(uint64_t guest_addr, uint64_t guest_size,
                                  Common::SlotId primary_id, uint32_t overlap_count,
                                  const char* detail = nullptr);

void WatchedImageNoteGc(WatchedImageEvent event, const Image* image, Common::SlotId image_id,
                        const char* reason);

[[nodiscard]] const char* WatchedImageEventName(WatchedImageEvent event) noexcept;
[[nodiscard]] WatchedImageState WatchedImagePeekLastState() noexcept;
[[nodiscard]] bool              WatchedImageHasGoodSnapshot() noexcept;
[[nodiscard]] bool              WatchedImageHasBadSnapshot() noexcept;
void                            WatchedImageWriteDifferentialReport() noexcept;

[[nodiscard]] bool WatchedImageUiProbeEnabled() noexcept;
void               WatchedImageNoteUiProbeSample(uint64_t frame, uint64_t shader_hash, Image& image,
                                                 Common::SlotId image_id, uint64_t vk_view,
                                                 const ImageViewInfo& view);

} // namespace Libs::Graphics

#endif
