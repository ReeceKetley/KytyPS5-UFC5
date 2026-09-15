#include "graphics/host_gpu/renderer/image/mipTailDiagnostic.h"

#include "common/logging/log.h"
#include "graphics/guest_gpu/tile.h"

#include <atomic>
#include <bit>
#include <cinttypes>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

namespace {

struct OverwideNote {
	uint32_t slot          = 0;
	uint32_t base_level    = 0;
	uint32_t last_level    = 0;
	uint32_t max_mip       = 0;
	uint32_t guest_levels  = 0;
	uint32_t complete      = 0;
	bool     read          = false;
	bool     written       = false;
	bool     atomic        = false;
	bool     storage       = false;
	uint64_t address       = 0;
};

thread_local std::vector<OverwideNote> t_overwide_notes;

[[nodiscard]] uint64_t LayoutKey(uint64_t address, uint32_t levels, uint32_t format, uint32_t tile,
                                 uint32_t width, uint32_t height) {
	// Coarse de-dupe so a single boot does not spam identical layout dumps.
	return address ^ (static_cast<uint64_t>(levels) << 48) ^ (static_cast<uint64_t>(format) << 32) ^
	       (static_cast<uint64_t>(tile) << 24) ^ (static_cast<uint64_t>(width) << 12) ^ height;
}

} // namespace

void LogGuestMipLayoutChain(uint64_t address, Prospero::BufferFormat format, Prospero::TileMode tile,
                            uint32_t width, uint32_t height, uint32_t depth, uint32_t levels,
                            bool volume) {
	static std::mutex              lock;
	static std::unordered_set<uint64_t> seen;
	const auto key = LayoutKey(address, levels, static_cast<uint32_t>(format),
	                           static_cast<uint32_t>(tile), width, height);
	{
		std::scoped_lock guard {lock};
		if (!seen.insert(key).second) {
			return;
		}
	}

	const auto complete =
	    std::bit_width(std::max({width, height, volume ? std::max(depth, 1u) : 1u}));
	LOGF("MipTailDiag: guest layout dump addr=0x%016" PRIx64
	     " format=%u tile=%u extent=%ux%ux%u levels=%u complete=%u volume=%d\n",
	     address, static_cast<uint32_t>(format), static_cast<uint32_t>(tile), width, height, depth,
	     levels, complete, volume);

	TileSurfaceLayout layout {};
	const TileSurfaceDescription description {
	    format,
	    tile,
	    volume ? TileSurfaceDimension::Dim3D : TileSurfaceDimension::Dim2D,
	    width,
	    height,
	    volume ? depth : 1u,
	    levels,
	    1};
	if (!TileGetTiledTextureLayout(description, layout)) {
		LOGF("MipTailDiag: TileGetTiledTextureLayout FAILED for addr=0x%016" PRIx64 "\n", address);
		TileSizeOffset level_sizes[16] {};
		TilePaddedSize padded[16] {};
		TileSizeAlign  total {};
		TileGetTextureSize(format, width, height, levels, tile, &total, level_sizes, padded);
		LOGF("MipTailDiag: fallback TileGetTextureSize total=0x%x align=0x%x\n", total.size,
		     total.align);
		for (uint32_t level = 0; level < levels; ++level) {
			const auto logical_w = std::max(width >> level, 1u);
			const auto logical_h = std::max(height >> level, 1u);
			LOGF("MipTailDiag: mip%u logical=%ux%u padded=%ux%u size=0x%x offset=0x%x "
			     "src_size=0x%x src_offset=0x%x tail_xy=(%u,%u)\n",
			     level, logical_w, logical_h, padded[level].width, padded[level].height,
			     level_sizes[level].size, level_sizes[level].offset, level_sizes[level].src_size,
			     level_sizes[level].src_offset, level_sizes[level].x, level_sizes[level].y);
		}
		return;
	}

	LOGF("MipTailDiag: tiled first_tail_level=%u block_slice=0x%" PRIx64 " total=0x%" PRIx64
	     " block=%ux%ux%u bpe=%u block_size=0x%x\n",
	     layout.first_tail_level, layout.block_slice_size, layout.total_size,
	     layout.texture.block.block_width, layout.texture.block.block_height,
	     layout.texture.block.block_depth, layout.texture.block.bytes_per_element,
	     layout.texture.block.block_size);

	for (uint32_t level = 0; level < levels; ++level) {
		const auto& mip        = layout.mips[level];
		const auto  logical_w  = std::max(width >> level, 1u);
		const auto  logical_h  = std::max(height >> level, 1u);
		const bool  in_tail    = level >= layout.first_tail_level;
		uint32_t    byte_in_block = 0;
		bool        byte_ok       = false;
		if (in_tail) {
			byte_ok = TileGetBlockOffset(layout.texture.block, mip.tail_x, mip.tail_y, 0,
			                             byte_in_block);
		}
		LOGF("MipTailDiag: mip%u logical=%ux%u elements=%ux%u padded=%ux%u "
		     "tiled_offset=0x%" PRIx64 " tiled_size=0x%" PRIx64 " tail=%d tail_xy=(%u,%u) "
		     "byte_in_block=%s0x%x shared_tail_base=0x%016" PRIx64 "\n",
		     level, logical_w, logical_h, mip.width, mip.height, mip.padded_width, mip.padded_height,
		     mip.offset, mip.size, in_tail ? 1 : 0, mip.tail_x, mip.tail_y, byte_ok ? "" : "?",
		     byte_in_block, in_tail ? address : address + mip.offset);
	}

	if (levels > complete) {
		LOGF("MipTailDiag: OVERWIDE guest declares levels=%u but Vulkan complete chain is %u "
		     "(indices 0..%u). Extra logical mips live in the mip-tail block if first_tail=%u.\n",
		     levels, complete, complete - 1, layout.first_tail_level);
	}
}

void LogGuestHostMipMismatch(const char* where, uint64_t address, const ImageInfo& guest,
                             uint32_t host_mip_levels, uint32_t requested_base,
                             uint32_t requested_count) {
	const auto complete = std::bit_width(
	    std::max({guest.extent.width, guest.extent.height, guest.extent.depth}));
	LOGF("MipTailDiag: GUEST_LOGICAL_GT_HOST_PHYSICAL where=%s addr=0x%016" PRIx64
	     " guest_levels=%u host_levels=%u complete=%u requested_view=%u+%u "
	     "extent=%ux%ux%u format=%u tile=%u\n",
	     where, address, guest.resources.levels, host_mip_levels, complete, requested_base,
	     requested_count, guest.extent.width, guest.extent.height, guest.extent.depth,
	     static_cast<uint32_t>(guest.guest_format), static_cast<uint32_t>(guest.tile_mode));

	for (uint32_t level = 0; level < guest.resources.levels && level < guest.mip_layout.size();
	     ++level) {
		const auto& mip = guest.mip_layout[level];
		LOGF("MipTailDiag: ImageInfo.mip_layout[%u] offset=0x%" PRIx64 " size=0x%" PRIx64
		     " pitch=%u height=%u logical=%ux%u%s\n",
		     level, mip.offset, mip.size, mip.pitch, mip.height,
		     std::max(guest.extent.width >> level, 1u), std::max(guest.extent.height >> level, 1u),
		     level >= host_mip_levels ? "  << NO HOST MIP" : "");
	}

	LogGuestMipLayoutChain(address, guest.guest_format, guest.tile_mode, guest.extent.width,
	                       guest.extent.height, guest.extent.depth, guest.resources.levels,
	                       guest.IsVolume());
}

void NoteOverwideTextureResolve(uint64_t frame, uint64_t shader_hash, uint32_t stage, uint32_t slot,
                                bool read, bool written, bool atomic, bool storage, bool r128,
                                uint32_t mip_mode, const ShaderTextureResource& descriptor,
                                uint32_t guest_levels, uint32_t complete_levels,
                                const ImageInfo* populated) {
	const auto address    = descriptor.Base40();
	const auto base_level = descriptor.BaseLevel();
	const auto last_level = descriptor.LastLevel();
	const auto max_mip    = descriptor.MaxMip();
	LOGF("MipTailDiag: resolve frame=%" PRIu64 " hash=0x%016" PRIx64
	     " stage=%u slot=%u read=%d write=%d atomic=%d storage=%d r128=%d mip_mode=%u "
	     "addr=0x%016" PRIx64 " base=%u last=%u max=%u guest_levels=%u complete=%u "
	     "extent=%ux%u format=%u tile=%u type=%u "
	     "dwords=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
	     frame, shader_hash, stage, slot, read, written, atomic, storage, r128, mip_mode, address,
	     base_level, last_level, max_mip, guest_levels, complete_levels,
	     static_cast<uint32_t>(descriptor.Width5()) + 1u,
	     static_cast<uint32_t>(descriptor.Height5()) + 1u, static_cast<uint32_t>(descriptor.Format()),
	     static_cast<uint32_t>(descriptor.TileMode()), static_cast<uint32_t>(descriptor.Type()),
	     descriptor.fields[0], descriptor.fields[1], descriptor.fields[2], descriptor.fields[3],
	     descriptor.fields[4], descriptor.fields[5], descriptor.fields[6], descriptor.fields[7]);

	if (populated != nullptr) {
		LogGuestMipLayoutChain(address, populated->guest_format, populated->tile_mode,
		                       populated->extent.width, populated->extent.height,
		                       populated->extent.depth, populated->resources.levels,
		                       populated->IsVolume());
	} else {
		LogGuestMipLayoutChain(address, descriptor.Format(), descriptor.TileMode(),
		                       static_cast<uint32_t>(descriptor.Width5()) + 1u,
		                       static_cast<uint32_t>(descriptor.Height5()) + 1u,
		                       static_cast<uint32_t>(descriptor.Depth()) + 1u, guest_levels, false);
	}

	t_overwide_notes.push_back(OverwideNote {
	    .slot         = slot,
	    .base_level   = base_level,
	    .last_level   = last_level,
	    .max_mip      = max_mip,
	    .guest_levels = guest_levels,
	    .complete     = complete_levels,
	    .read         = read,
	    .written      = written,
	    .atomic       = atomic,
	    .storage      = storage,
	    .address      = address,
	});
}

void FlushOverwideTexturePrepare(uint64_t frame, uint64_t shader_hash, uint32_t stage) {
	auto notes = std::move(t_overwide_notes);
	t_overwide_notes.clear();
	if (notes.size() < 2) {
		return;
	}
	for (size_t i = 0; i < notes.size(); ++i) {
		for (size_t j = i + 1; j < notes.size(); ++j) {
			if (notes[i].address != notes[j].address) {
				continue;
			}
			if (notes[i].base_level == notes[j].base_level &&
			    notes[i].last_level == notes[j].last_level) {
				continue;
			}
			LOGF("MipTailDiag: SAME_PREPARE_MULTI_MIP frame=%" PRIu64 " hash=0x%016" PRIx64
			     " stage=%u addr=0x%016" PRIx64
			     " slotA=%u viewA=%u..%u rwA=%d%d slotB=%u viewB=%u..%u rwB=%d%d "
			     "complete=%u guest_levels=%u\n",
			     frame, shader_hash, stage, notes[i].address, notes[i].slot, notes[i].base_level,
			     notes[i].last_level, notes[i].read, notes[i].written, notes[j].slot,
			     notes[j].base_level, notes[j].last_level, notes[j].read, notes[j].written,
			     notes[i].complete, notes[i].guest_levels);
		}
	}
}

} // namespace Libs::Graphics
