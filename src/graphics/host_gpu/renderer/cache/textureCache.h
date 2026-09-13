#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/lruCache.h"
#include "common/slotVector.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionManager.h"
#include "graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
#include "graphics/host_gpu/renderer/image/blitHelper.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/tiler.h"

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
class Buffer;
class BufferCache;
class CommandBuffer;
class CommandScheduler;
class RenderExecutor;
struct TextureCacheTestAccess;

class TextureCache {
public:
	enum class BindingType : uint8_t { Texture, Storage, RenderTarget, DepthTarget, VideoOut };

	struct ImageDesc {
		ImageInfo     info;
		ImageViewInfo view_info;
		BindingType   type = BindingType::Texture;
	};

	TextureCache(GraphicContext& graphics, CommandScheduler& scheduler, PageManager& page_manager,
	             BufferCache& buffer_cache);
	~TextureCache();
	KYTY_CLASS_NO_COPY(TextureCache);

	[[nodiscard]] ImageId       FindImage(ImageDesc& desc, bool exact_format = false);
	void                        UpdateImage(ImageId id);
	[[nodiscard]] ImageId       FindImageFromRange(uint64_t address, uint64_t size,
	                                               bool ensure_valid = true);
	// Diagnostic: every image registered at exactly `address`, not just the one the scoring in
	// FindImageFromRange happens to pick. UFC 5 binds one address as several resources in a
	// single frame (a LUT, a raw buffer, a 1600x900 scene, a 400x225 target), and a dump that
	// takes only the winner silently measures a different resource than the one under study.
	[[nodiscard]] std::vector<ImageId> FindAllImagesAtAddress(uint64_t address, uint64_t size);
	// Diagnostic: zero every colour image registered at `address`. Used to test whether a
	// self-feeding temporal buffer recovers once its poisoned history is discarded, which
	// separates "NaN generated every frame" from "NaN injected once and then persisting".
	uint32_t ClearImagesAtAddress(CommandBuffer& command, uint64_t address, uint64_t size);
	// DIAGNOSTIC, NOT A FIX. Seeds freshly created colour images with a non-zero constant
	// instead of leaving them at the zero the host OS hands us. Guest direct memory on the
	// console holds whatever was there before; VirtualAlloc(MEM_COMMIT) zero-fills, and zero is
	// PATHOLOGICAL rather than merely different - it feeds 0/0 and rcp(0) into the temporal
	// upscaler, which is exactly where the black round starts (ledger session 12 point 7).
	// Enabled by KYTY_SEED_NEW_IMAGES=<float>; inert when unset. KYTY_SEED_NEW_IMAGES_FORMAT
	// overrides the format filter (decimal VkFormat, default 97 = R16G16B16A16_SFLOAT).
	uint32_t SeedNewImages(CommandBuffer& command);
	// nullptr when KYTY_SEED_NEW_IMAGES is unset, so the whole path stays inert by default.
	static const float* SeedNewImagesValue();
	// KYTY_SEED_NEW_IMAGES_NOISE=1: fill with a replicated noise tile instead of a constant, so
	// the seed carries spatial VARIATION. A constant seed (0.0 or 0.5) fails identically, which
	// is what put the blame on uniformity rather than on the value - ledger session 12.
	bool SeedImageWithNoise(Image& image);
	[[nodiscard]] ImageId       FindLastPresentableColor();
	[[nodiscard]] vk::ImageView FindTexture(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindRenderTarget(ImageId id, const ImageDesc& desc);
	[[nodiscard]] vk::ImageView FindDepthTarget(ImageId id, const ImageDesc& desc);
	[[nodiscard]] Image&        GetImage(ImageId id) {
		auto& image = m_slot_images[id];
		TouchImage(image);
		return image;
	}
	void MarkGpuWritten(ImageId id);

	[[nodiscard]] bool ClearImageFromBuffer(CommandBuffer& command, uint64_t address, uint64_t size,
	                                        uint32_t packed_clear);
	void               InvalidateMemory(uint64_t address, uint64_t size);
	void               InvalidateMemoryFromGPU(uint64_t address, uint64_t size);

	// The game's VideoOut buffer registration is the authoritative interpretation of
	// the bits at a flip address. Record it here so that whichever binding first
	// creates the cache image for that memory (a CS storage write can beat the
	// VideoOut resolve) stamps the registered packed format, not its own guess.
	void RegisterVideoOutSurface(uint64_t address, uint64_t size, vk::Format format);
	[[nodiscard]] bool IsRegionGpuModified(uint64_t address, uint64_t size);

	[[nodiscard]] bool IsMeta(uint64_t address);
	[[nodiscard]] bool IsMetaCleared(uint64_t address, uint32_t slice,
	                                 uint32_t* fill_value = nullptr);
	[[nodiscard]] bool ClearMeta(uint64_t address);
	// Record deferred DCC state while the original guest dispatch writes the metadata.
	void               TrackDccFill(uint64_t address, uint64_t size, uint32_t fill_value);
	[[nodiscard]] bool TouchMeta(uint64_t address, uint32_t slice, bool is_clear);

	void UnmapMemory(uint64_t address, uint64_t size);
	void ProcessDownloadImages();
	void RunGarbageCollector();

private:
	enum class TransferDirection { Upload, Download };
	struct TextureTransferPlan;
	struct DownloadPlan;

	struct MetaDataInfo {
		// A guest metadata-fill dispatch may initialize DCC before its render target is bound.
		// PendingDcc retains that exact fill until an image binding classifies the address,
		// without exposing an unconfirmed buffer address to the normal metadata heuristics.
		// Keep all surface metadata in one entry so CMask/FMask can be
		// registered beside HTile and DCC without introducing parallel tracking paths.
		enum class Type : uint8_t { PendingDcc, CMask, FMask, HTile, Dcc };

		Type     type       = Type::PendingDcc;
		uint32_t clear_mask = 0;
		uint32_t fill_value = 0xffffffffu;
		uint64_t fill_size  = 0;
	};

	struct OverlapResult {
		ImageId image;
		int32_t mip   = -1;
		int32_t layer = -1;
	};

	using ImageIds       = InlinePageOwnerList<ImageId, 16>;
	using ImagePageTable = MultiLevelPageTable<ImageIds, 20, 40, 10>;
	void ConfigureGarbageCollectionBudget(uint64_t available_budget);

	[[nodiscard]] ImageId     InsertImage(const ImageInfo& info);
	[[nodiscard]] ImageId     GetNullImage(const ImageDesc& desc);
	// If [info.data] sits inside a registered VideoOut surface, force info.pixel_format
	// to the registered packed format. Caller holds m_lock.
	void                      PinVideoOutFormat(ImageInfo& info) const;
	void                      RegisterImage(ImageId id);
	void                      UnregisterImage(ImageId id);
	void                      DeleteImage(ImageId id);
	void                      FreeImage(ImageId id);
	void                      TouchImage(Image& image);
	void                      TrackImage(ImageId id);
	void                      TrackImageHead(ImageId id);
	void                      TrackImageTail(ImageId id);
	void                      UntrackImage(ImageId id);
	void                      UntrackImageHead(ImageId id);
	void                      UntrackImageTail(ImageId id);
	void                      MarkAsMaybeDirty(ImageId id, Image& image);
	void                      TrackImageDownload(ImageId id, Image& image);
	[[nodiscard]] static bool SameGuestLayout(const ImageInfo& cached, const ImageInfo& requested);
	[[nodiscard]] static bool SameBacking(const ImageInfo& cached, const ImageInfo& requested,
	                                      bool exact_format);
	[[nodiscard]] static BindingType UploadBinding(const Image& image);
	[[nodiscard]] bool               SafeToDownload(const Image& image);

	// Caller holds m_lock; it also serializes the per-image query epoch.
	[[nodiscard]] ImageIds      FindImagesInRegion(uint64_t address, uint64_t size,
	                                               bool page_overlap) const;
	[[nodiscard]] OverlapResult ResolveOverlap(const ImageInfo& requested, BindingType binding,
	                                           ImageId cached, ImageId merged);
	[[nodiscard]] ImageId       ResolveDepthOverlap(const ImageInfo& requested, BindingType binding,
	                                                ImageId cached);
	[[nodiscard]] ImageId       ExpandImage(const ImageInfo& info, ImageId source);
	void                        RefreshImage(ImageId id);
	void                        PrepareDccClear(ImageId id, const ImageDesc& desc);
	void                        InitializeImage(ImageId id);
	[[nodiscard]] TextureTransferPlan
	BuildTextureTransfer(const Image& image, BindingType binding, TransferDirection direction) const;
	[[nodiscard]] DownloadPlan BuildDownload(const Image& image) const;
	void UploadImage(Image& image, Buffer& source, uint64_t source_offset);
	void DownloadImageData(Image& image, Buffer& destination, uint64_t destination_offset,
	                       uint64_t destination_size, DownloadPlan plan);
	void DownloadDepth(Image& image, Buffer& destination, uint64_t destination_offset);
	void CommitGpuWrite(Image& image);
	// Caller holds m_lock. Volume layer ranges select depth slices.
	void ClearImage(CommandBuffer& command, ImageId id, const vk::ImageSubresourceRange& range,
	                const vk::ClearValue& clear);
	void PrepareImageCopy(Image& image);
	void RefreshCopySource(ImageId id);
	[[nodiscard]] bool CopyD16(Image& destination, Image& source);
	void               CopyImage(ImageId destination, ImageId source);
	void               NotePresentableColor(const Image& image);
	void               AssociateStencil(ImageId depth, GuestRange stencil);
	void CopyImageMip(ImageId destination, ImageId source, uint32_t mip, uint32_t layer);
	void ValidateImageDesc(const ImageDesc& desc) const;

	void               InvalidateCpuAliases(uint64_t address, uint64_t size);
	[[nodiscard]] bool TryDownloadImage(ImageId id);

	GraphicContext&                                   m_graphics;
	CommandScheduler&                                 m_scheduler;
	TrackingSpinLock                                  m_lock;
	PageManager&                                      m_page_manager;
	BlitHelper                                        m_blit_helper;
	TileManager                                       m_tiler;
	BufferCache&                                      m_buffer_cache;
	Common::SlotVector<Image>                         m_slot_images;
	ImagePageTable                                    m_image_page_table;
	std::unordered_map<vk::Format, ImageId>           m_null_images;
	Common::LeastRecentlyUsedCache<ImageId, uint64_t> m_lru_cache;
	std::unordered_set<ImageId>                       m_download_images;
	std::map<uint64_t, MetaDataInfo>                  m_surface_metas;
	struct VideoOutSurface {
		uint64_t   address = 0;
		uint64_t   size    = 0;
		vk::Format format  = vk::Format::eUndefined;
	};
	std::vector<VideoOutSurface>                      m_video_out_surfaces;
	uint64_t                                          m_total_used_memory  = 0;
	uint64_t                                          m_trigger_gc_memory  = 0;
	uint64_t                                          m_pressure_gc_memory = 1536ull * 1024 * 1024;
	uint64_t         m_critical_gc_memory     = 3ull * 1024 * 1024 * 1024;
	uint64_t         m_gc_tick                = 0;
	mutable uint32_t m_image_query_epoch      = 0;
	bool             m_readback_linear_images = false;
	// Images created since the last SeedNewImages sweep, for KYTY_SEED_NEW_IMAGES. Filtering
	// happens at sweep time, not insert time, because `backing` is not usable until then.
	std::vector<ImageId> m_pending_seed;
	uint64_t         m_presentable_address    = 0;
	uint64_t         m_presentable_size       = 0;

	friend struct TextureCacheTestAccess;
	friend class BufferCache;
	friend class RenderExecutor;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_TEXTURECACHE_H_
