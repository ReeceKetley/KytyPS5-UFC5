#include "graphics/host_gpu/renderer/cache/bufferCache.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/timer.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;

// KYTY_DEFER_READBACK, parsed once. Returns the minimum GPU->CPU copy size (bytes)
// that gets pushed onto the 1-frame-latency path; UINT64_MAX means deferral is off
// (the default). See the strategy comment in DownloadBufferMemory().
uint64_t ParseDeferMinCopy() {
	const char* d = std::getenv("KYTY_DEFER_READBACK");
	if (d == nullptr || d[0] == '\0' || d[0] == '0') {
		return UINT64_MAX; // never defer (default)
	}
	if (std::strcmp(d, "all") == 0) {
		return 0;
	}
	char*      end = nullptr;
	const auto kb  = std::strtoull(d, &end, 10);
	if (end != d && kb > 1) {
		return kb * 1024;
	}
	return 2 * 1024 * 1024;
}

} // namespace

uint64_t BufferCache::DeferMinCopyBytes() {
	static const uint64_t v = ParseDeferMinCopy();
	return v;
}

bool BufferCache::DeferredReadbackEnabled() {
	return DeferMinCopyBytes() != UINT64_MAX;
}

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		buffer.CopyFrom(m_scheduler.Current(), m_staging_buffer, offset, buffer.Offset(address),
		                chunk, vk::AccessFlagBits::eHostWrite);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

struct BufferCache::DownloadCopy {
	Buffer*  buffer        = nullptr;
	uint64_t source_offset = 0;
	uint64_t address       = 0;
	uint64_t size          = 0;
};

// A GPU->staging copy that has been recorded into the command stream but whose
// staging->guest writeback is deferred until the owning tick retires.
struct BufferCache::DeferredReadback {
	struct Part {
		uint64_t address;        // guest address to write
		uint64_t staging_offset; // absolute offset in m_download_buffer
		uint64_t size;
	};
	uint64_t          tick        = 0;
	uint8_t*          mapped      = nullptr; // base of m_download_buffer host mapping
	uint64_t          base_offset = 0;       // Map() base for this batch
	std::vector<Part> parts;
};

void BufferCache::DrainDeferredReadbacks(bool force) {
	if (m_deferred_readbacks.empty()) {
		return;
	}
	std::vector<DeferredReadback> still_pending;
	for (auto& rb: m_deferred_readbacks) {
		if (!force && !m_scheduler.IsFree(rb.tick)) {
			still_pending.push_back(std::move(rb));
			continue;
		}
		if (!m_scheduler.IsFree(rb.tick)) {
			m_scheduler.Wait(rb.tick);
		}
		for (const auto& p: rb.parts) {
			m_download_buffer.Invalidate(p.staging_offset, p.size);
			// The guest can unmap this range between scheduling and applying the
			// readback; drop it silently in that case (the data is no longer wanted).
			(void)Libs::LibKernel::Memory::TryWriteBacking(
			    p.address, rb.mapped + (p.staging_offset - rb.base_offset), p.size);
		}
	}
	m_deferred_readbacks = std::move(still_pending);
}

void BufferCache::EnsureCurrentForCpu(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0 || size == 0) {
		return;
	}
	// Only meaningful when 1-frame-latency readback is armed: it forces the small
	// indirect arg / count regions current before the CP reads them CPU-side, and
	// lands any deferred writeback for them. With synchronous readback (default) the
	// memory-tracker fault path already keeps these ranges current, and there are
	// never any deferred readbacks to drain - so skip the CPU<->GPU sync round-trip
	// entirely (it was serialising every indirect draw/dispatch, ~3x fps hit on menus).
	if (!DeferredReadbackEnabled()) {
		return;
	}
	// Pull down any GPU-dirty bytes (these arg regions are small, so DownloadBufferMemory
	// takes the synchronous path anyway) then land anything that was already deferred.
	ReadMemory(vaddr, size, false);
	DrainDeferredReadbacks(true);
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	// Running total of what THIS cache holds. The GC needs it every run (~175x/frame), so it
	// cannot be recomputed by walking m_slot_buffers - the symmetric register/unregister hook is
	// the cheap place to maintain it.
	if constexpr (insert) {
		m_registered_bytes += buffer.Size();
	} else {
		m_registered_bytes -= std::min(m_registered_bytes, buffer.Size());
	}
	PageTable::PageRange pages {};
	EXIT_IF(!PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	if constexpr (insert) {
		const auto [it, inserted] = m_buffers.emplace(buffer.CpuAddress(), id);
		(void)it;
		EXIT_IF(!inserted);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		WriteDataBuffer(m_bda_pagetable_buffer, pages.first * sizeof(vk::DeviceAddress),
		                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
	} else {
		const auto found = m_buffers.find(buffer.CpuAddress());
		EXIT_IF(found == m_buffers.end() || found->second != id);
		m_buffers.erase(found);
		EXIT_IF(buffer.Size() > m_total_used_memory);
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		m_bda_pagetable_buffer.Fill(pages.first * sizeof(vk::DeviceAddress),
		                            size_pages * sizeof(vk::DeviceAddress), 0);
		buffer.is_deleted = true;
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	if (!buffer.is_deleted) {
		m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
	}
}

void BufferCache::DeleteBuffer(BufferId id) {
	auto* buffer = m_slot_buffers.try_get(id);
	if (buffer == nullptr || buffer->is_deleted) {
		return;
	}
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

std::pair<uint64_t, uint64_t> BufferCache::DownloadEnvelope(const DownloadCopy& copy) {
	if (copy.buffer == nullptr || copy.size == 0 || copy.source_offset > copy.buffer->Size() ||
	    copy.size > copy.buffer->Size() - copy.source_offset) {
		EXIT("BufferCache: invalid download copy\n");
	}
	const auto begin = copy.source_offset & ~uint64_t {3};
	if (copy.source_offset > UINT64_MAX - copy.size ||
	    copy.source_offset + copy.size > UINT64_MAX - 3) {
		EXIT("BufferCache: download copy alignment overflow\n");
	}
	const auto end = (copy.source_offset + copy.size + 3) & ~uint64_t {3};
	if (end > copy.buffer->Size()) {
		EXIT("BufferCache: aligned download copy exceeds its owner\n");
	}
	return {begin, end - begin};
}

namespace {

// How often a readback actually made it onto the transfer queue rather than falling back to
// draining the graphics queue.
void NoteTransferReadback(bool on_transfer_queue) {
	static std::atomic<uint32_t> total {0}, on_queue {0};
	on_queue.fetch_add(on_transfer_queue ? 1u : 0u, std::memory_order_relaxed);
	if ((total.fetch_add(1, std::memory_order_relaxed) % 256) == 255) {
		const auto n = on_queue.exchange(0);
		LOGF("XferReadback/256: on_transfer_queue=%u (%.1f%%)\n", n, n / 256.0 * 100.0);
	}
}

} // namespace

void BufferCache::DownloadBufferMemory(std::span<const DownloadCopy> copies) {
	{
		static std::atomic<uint32_t> calls {0};
		uint64_t                     total = 0;
		for (const auto& c: copies) {
			total += c.size;
		}
		const auto n = calls.fetch_add(1, std::memory_order_relaxed);
		if (n < 64 || (n % 64) == 0) {
			LOGF("BufferDownload #%u: copies=%zu bytes=%" PRIu64 " first_addr=0x%016" PRIx64
			     " size=%" PRIu64 "\n",
			     n, copies.size(), total, copies.empty() ? 0 : copies.front().address,
			     copies.empty() ? 0 : copies.front().size);
		}
	}
	// Census of EVERY copy, not just the batch's first. The `first_addr` line above samples
	// (64 calls then 1-in-64) and only ever names copies.front(), so it cannot answer "does the
	// guest CPU ever read address X back?" - and that question decides whether GPU-side culling
	// can change the host's draw count at all. Accumulates bytes+count per address and dumps the
	// top talkers. Ungated like Pm4Ops/SchedFinish - Config::GraphicsDebugDumpEnabled() needs
	// --graphics-debug-dump, which none of the other counters require, and gating on it made
	// this print nothing at all on the first attempt.
	{
		struct Site {
			uint64_t bytes = 0;
			uint32_t count = 0;
		};
		static std::mutex                              census_mutex;
		static std::unordered_map<uint64_t, Site>      census;
		static uint32_t                                census_calls = 0;
		std::scoped_lock                               lock {census_mutex};
		for (const auto& c: copies) {
			auto& site = census[c.address];
			site.bytes += c.size;
			site.count++;
		}
		if (++census_calls % 256 == 0) {
			std::vector<std::pair<uint64_t, Site>> sorted(census.begin(), census.end());
			std::sort(sorted.begin(), sorted.end(),
			          [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
			std::string top;
			for (size_t i = 0; i < sorted.size() && i < 8; i++) {
				top += fmt::format(" 0x{:016x}={}x/{}KB", sorted[i].first, sorted[i].second.count,
				                   sorted[i].second.bytes / 1024);
			}
			LOGF("BufferDlCensus/256: sites=%zu%s\n", sorted.size(), top.c_str());
		}
	}
	// Readback strategy. DEFAULT: synchronous (full GPU idle-stall per drain) - stable.
	// 1-frame-latency deferral is a large win on finish_ms (~300ms -> ~20ms/frame) but
	// still destabilises the in-match GPU somewhere beyond the indirect-arg reads that
	// EnsureCurrentForCpu() now protects; needs more work before it can be the default.
	//   KYTY_DEFER_READBACK=1        - defer copies >= 2 MiB
	//   KYTY_DEFER_READBACK=<KiB>    - defer copies >= <KiB>
	//   KYTY_DEFER_READBACK=all      - defer everything
	const uint64_t defer_min_copy = DeferMinCopyBytes();

	// The previous call's deferred batch has almost always retired by now, so this is
	// a cheap check, not a stall. Draining before Map() guarantees the staging region
	// it may wrap into has been consumed.
	DrainDeferredReadbacks(true);

	std::vector<DownloadCopy> batch;
	batch.reserve(copies.size());
	uint64_t                  packed_size   = 0;
	uint64_t                  max_copy_size = 0;
	auto&                     download    = m_download_buffer;
	const auto flush = [&] {
		const auto [mapped, base_offset] = download.Map(packed_size, DOWNLOAD_ALIGNMENT);
		EXIT_IF(mapped == nullptr);
		uint64_t          cursor = 0;
		DeferredReadback  rb;
		rb.mapped      = mapped;
		rb.base_offset = base_offset;
		rb.parts.reserve(batch.size());

		// Only the data this batch reads has to be finished, not everything queued behind
		// it. Per-buffer is a safe over-approximation of per-range.
		uint64_t producer_tick = 0;
		for (const auto& copy: batch) {
			producer_tick = std::max(producer_tick, copy.buffer->LastGpuWriteTick());
		}
		const bool synchronous = max_copy_size < defer_min_copy;
		const bool try_transfer_queue = synchronous && m_scheduler.TransferReadbackAvailable();

		// Geometry of the batch, shared by both recording paths.
		struct PlannedCopy {
			Buffer*    owner;
			vk::Buffer source;
			uint64_t   source_begin;
			uint64_t   staging_offset;
			uint64_t   bytes;
		};
		std::vector<PlannedCopy> planned;
		planned.reserve(batch.size());
		for (const auto& copy: batch) {
			const auto [source_begin, envelope_size] = DownloadEnvelope(copy);
			planned.push_back({copy.buffer, copy.buffer->Handle(), source_begin,
			                   base_offset + cursor, envelope_size});
			const auto write_offset = cursor + copy.source_offset - source_begin;
			rb.parts.push_back({copy.address, base_offset + write_offset, copy.size});
			cursor += AlignDownload(envelope_size);
		}

		bool transferred = false;
		if (try_transfer_queue) {
			const auto destination = download.Handle();
			transferred            = m_scheduler.SubmitTransferReadback(
                [&planned, destination](vk::CommandBuffer command) {
                    std::vector<vk::BufferCopy> regions;
                    regions.reserve(planned.size());
                    for (const auto& item: planned) {
                        // The semaphore wait already makes the producer's writes visible,
                        // so only the host-read dependency after the copy is needed.
                        regions.push_back({.srcOffset = item.source_begin,
                                           .dstOffset = item.staging_offset,
                                           .size      = item.bytes});
                        command.copyBuffer(item.source, destination, 1, &regions.back());
                    }
                    vk::MemoryBarrier host_barrier {};
                    host_barrier.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
                    host_barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
                    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                            vk::PipelineStageFlagBits::eHost, {}, 1,
                                            &host_barrier, 0, nullptr, 0, nullptr);
                },
                producer_tick);
		}
		if (!transferred) {
			for (const auto& item: planned) {
				download.CopyFrom(m_scheduler.Current(), *item.owner, item.source_begin,
				                  item.staging_offset, item.bytes,
				                  vk::AccessFlagBits::eMemoryWrite, vk::AccessFlags {},
				                  vk::AccessFlagBits::eMemoryRead |
				                      vk::AccessFlagBits::eMemoryWrite,
				                  vk::AccessFlagBits::eHostRead);
			}
		}
		download.Commit();
		rb.tick = m_scheduler.CurrentTick();

		if (transferred) {
			// SubmitTransferReadback already waited on its own timeline; the staging data
			// is present without draining the graphics queue.
			NoteTransferReadback(true);
			for (const auto& part: rb.parts) {
				download.Invalidate(part.staging_offset, part.size);
				Libs::LibKernel::Memory::WriteBacking(
				    part.address, mapped + (part.staging_offset - base_offset), part.size);
			}
		} else if (synchronous) {
			NoteTransferReadback(false);
			m_scheduler.Finish("buffer-download");
			m_scheduler.WaitPriorityOperations(rb.tick);
			for (const auto& p: rb.parts) {
				download.Invalidate(p.staging_offset, p.size);
				Libs::LibKernel::Memory::WriteBacking(
				    p.address, mapped + (p.staging_offset - base_offset), p.size);
			}
		} else {
			// 1-frame-latency readback: the GPU->staging copy rides the current submit;
			// the staging->guest writeback runs from the next DownloadBufferMemory (or
			// GC) once this tick retires. Guest memory holds the previous frame's
			// values until then. Frostbite's readbacks (occlusion, draw/instance
			// counts, LOD) are next-frame decisions and tolerate this.
			m_deferred_readbacks.push_back(std::move(rb));
		}
		batch.clear();
		packed_size   = 0;
		max_copy_size = 0;
	};
	for (auto copy: copies) {
		while (copy.size != 0) {
			const auto available = download.Size() - packed_size;
			const auto prefix    = copy.source_offset & 3u;
			const auto bytes     = std::min(copy.size, available - prefix);
			DownloadCopy part {copy.buffer, copy.source_offset, copy.address, bytes};
			const auto [source_begin, envelope_size] = DownloadEnvelope(part);
			(void)source_begin;
			packed_size += AlignDownload(envelope_size);
			max_copy_size = std::max(max_copy_size, bytes);
			batch.push_back(part);
			copy.source_offset += bytes;
			copy.address += bytes;
			copy.size -= bytes;
			if (packed_size == download.Size()) {
				flush();
			}
		}
	}
	if (!batch.empty()) {
		flush();
	}
	for (const auto& copy: copies) {
		m_gpu_modified_ranges.Subtract(copy.address, copy.size);
	}
}

void BufferCache::AppendSmallDirtyDownloads(std::vector<DownloadCopy>& copies) {
	// Frostbite touches dozens of tiny GPU-written counters per frame. Fold every
	// remaining small dirty range into this drain. Tracker GPU-dirty bits are
	// 4 KiB pages: unmarking a 4-byte copy would clear the whole page and leave
	// sibling GPU intervals orphaned, so expand onto those pages and download
	// every dirty interval that lives on them.
	constexpr uint64_t kMaxBytes = 64 * 1024;
	constexpr uint64_t kBudget   = 4 * 1024 * 1024;
	constexpr uint64_t kPage       = TRACKER_PAGE_SIZE;

	RangeSet queued;
	for (const auto& copy: copies) {
		if (copy.size != 0) {
			queued.Add(copy.address, copy.size);
		}
	}

	uint64_t extra = 0;
	const auto add_interval = [&](uint64_t begin, uint64_t size) {
		if (size == 0 || extra >= kBudget || queued.Contains(begin, size)) {
			return;
		}
		const auto* owner = m_page_table.Find(begin >> PageTable::kPageBits);
		if (owner == nullptr || !*owner) {
			return;
		}
		auto* buffer = m_slot_buffers.try_get(*owner);
		if (buffer == nullptr || buffer->is_deleted) {
			return;
		}
		const auto clip_begin = std::max(begin, buffer->CpuAddress());
		const auto clip_end   = std::min(begin + size, buffer->CpuAddress() + buffer->Size());
		if (clip_begin >= clip_end) {
			return;
		}
		const auto clip_size = clip_end - clip_begin;
		if (queued.Contains(clip_begin, clip_size) || extra + clip_size > kBudget) {
			return;
		}
		copies.push_back({buffer, buffer->Offset(clip_begin), clip_begin, clip_size});
		queued.Add(clip_begin, clip_size);
		extra += clip_size;
	};

	std::vector<std::pair<uint64_t, uint64_t>> seeds;
	m_gpu_modified_ranges.ForEach([&](uint64_t begin, uint64_t end) {
		if (end <= begin) {
			return;
		}
		const auto size = end - begin;
		if (size > kMaxBytes || queued.Contains(begin, size)) {
			return;
		}
		seeds.emplace_back(begin, end);
	});
	for (const auto [begin, end]: seeds) {
		if (extra >= kBudget) {
			break;
		}
		add_interval(begin, end - begin);
	}

	// Fold every remaining GPU interval on pages we already plan to download so
	// those pages can be unmarked without orphaning siblings.
	const size_t expand_until = copies.size();
	for (size_t i = 0; i < expand_until && extra < kBudget; ++i) {
		const auto begin = copies[i].address;
		const auto end   = copies[i].address + copies[i].size;
		if (end <= begin) {
			continue;
		}
		const auto page_begin = begin & ~(kPage - 1);
		const auto page_end   = (end + kPage - 1) & ~(kPage - 1);
		m_gpu_modified_ranges.ForEachIntersection(
		    page_begin, page_end - page_begin, [&](RangeSet::Range range) {
			    add_interval(range.address, range.size);
		    });
	}
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 32 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));

	// DIAGNOSTIC OVERRIDES, in MB. `critical` decides whether RunGarbageCollector DOWNLOADS a
	// dirty buffer or skips it, and in-fight UFC5 sits ~1% over it permanently
	// (used=5287MB vs critical=5222MB, aggressive=128/128), so every GC run pays the download.
	// These make the threshold A/B-able without a rebuild.
	const auto override_mb = [](const char* name, uint64_t& value) {
		const char* env = std::getenv(name);
		if (env == nullptr || env[0] == '\0') {
			return;
		}
		char*      end    = nullptr;
		const auto parsed = std::strtoull(env, &end, 10);
		if (end != env && parsed != 0) {
			value = parsed * 1024ull * 1024ull;
		}
	};
	override_mb("KYTY_GC_TRIGGER_MB", m_trigger_gc_memory);
	override_mb("KYTY_GC_CRITICAL_MB", m_critical_gc_memory);
	LOGF("BufferGcThresholds: budget=%" PRIu64 "MB trigger=%" PRIu64 "MB critical=%" PRIu64 "MB\n",
	     static_cast<uint64_t>(budget) / (1024 * 1024), m_trigger_gc_memory / (1024 * 1024),
	     m_critical_gc_memory / (1024 * 1024));
}

BufferCache::~BufferCache() {
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
	m_buffers.clear();
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid memory-invalidation range\n");
	}
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write) {
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	// Split a readback's cost: how much is the GPU actually being waited on
	// (ReadMemoryOnGpu, which contains the Finish), and how much is the guest thread
	// queueing behind a saturated GPU worker thread before its command is even run?
	// Those need completely different fixes.
	const bool on_gpu_thread = GuestGpu::IsGpuThread();
	const auto t0            = Common::Timer::QueryPerformanceCounter();
	uint64_t   work_ticks    = 0;
	m_scheduler.Context().GetGpu().SendCommandSync([this, vaddr, size, is_write, &work_ticks] {
		const auto w0 = Common::Timer::QueryPerformanceCounter();
		ReadMemoryOnGpu(vaddr, size, is_write);
		work_ticks = Common::Timer::QueryPerformanceCounter() - w0;
	});
	{
		static std::atomic<uint64_t> n_gpu {0}, n_guest {0}, us_gpu {0}, us_guest {0},
		    us_guest_work {0};
		const auto freq = Common::Timer::QueryPerformanceFrequency();
		if (freq != 0) {
			const auto total_us = (Common::Timer::QueryPerformanceCounter() - t0) * 1000000ull / freq;
			const auto work_us  = work_ticks * 1000000ull / freq;
			uint64_t   c        = 0;
			if (on_gpu_thread) {
				us_gpu.fetch_add(total_us, std::memory_order_relaxed);
				c = n_gpu.fetch_add(1, std::memory_order_relaxed) + 1;
			} else {
				us_guest.fetch_add(total_us, std::memory_order_relaxed);
				us_guest_work.fetch_add(work_us, std::memory_order_relaxed);
				c = n_guest.fetch_add(1, std::memory_order_relaxed) + 1;
			}
			if (((n_gpu.load(std::memory_order_relaxed) +
			      n_guest.load(std::memory_order_relaxed)) %
			     512) == 0) {
				const auto ng = n_gpu.load(std::memory_order_relaxed);
				const auto nq = n_guest.load(std::memory_order_relaxed);
				const auto tg = us_gpu.load(std::memory_order_relaxed);
				const auto tq = us_guest.load(std::memory_order_relaxed);
				const auto wq = us_guest_work.load(std::memory_order_relaxed);
				LOGF("ReadbackSplit: gpu_thread=%" PRIu64 "/%.0fms  guest_thread=%" PRIu64
				     "/%.0fms (of which gpu_work=%.0fms, queued=%.0fms)\n",
				     ng, tg / 1000.0, nq, tq / 1000.0, wq / 1000.0, (tq - wq) / 1000.0);
			}
			(void)c;
		}
	}
}

void BufferCache::DiscardMemory(uint64_t vaddr, uint64_t size) {
	// KYTY_UNMAP_DISCARD=0 restores the old write-back-on-unmap behaviour.
	static const bool discard = [] {
		const char* e = std::getenv("KYTY_UNMAP_DISCARD");
		return e == nullptr || (e[0] != '0' || e[1] != '\0');
	}();
	if (!discard) {
		InvalidateMemory(vaddr, size);
		return;
	}
	if (vaddr == 0 || size == 0 || !GuestRange {vaddr, size}.Valid()) {
		return;
	}
	m_scheduler.Context().GetGpu().SendCommandSync(
	    [this, vaddr, size] { DiscardMemoryOnGpu(vaddr, size); });
}

void BufferCache::DiscardMemoryOnGpu(uint64_t vaddr, uint64_t size) {
	// The guest is unmapping this range, so it can never observe these bytes again:
	// downloading GPU-dirty data back into memory that is about to be destroyed is
	// pure waste. InvalidateMemory() would do exactly that, and it widens to a
	// 512 KiB window first, dragging unrelated dirty ranges into the same GPU-idle
	// Finish("buffer-download"). Drop GPU ownership instead, keeping the tracker and
	// the range set consistent so the GC still retires the buffers cleanly.
	if (!IsRegionRegistered(vaddr, size)) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
		return;
	}
	static std::atomic<uint32_t> discards {0};
	const auto                   n = discards.fetch_add(1, std::memory_order_relaxed);
	if (n < 8 || (n % 4096) == 0) {
		LOGF("UnmapDiscard #%u: addr=0x%016" PRIx64 " size=%" PRIu64 " gpu_dirty=%d\n", n, vaddr,
		     size, m_gpu_modified_ranges.Intersects(vaddr, size) ? 1 : 0);
	}
	m_gpu_modified_ranges.Subtract(vaddr, size);
	// Tracker bits are 4 KiB pages: release a page only when no GPU-dirty bytes
	// remain on it, so a partially covered edge page keeps its siblings' state.
	const auto page_begin = vaddr & ~(TRACKER_PAGE_SIZE - 1);
	const auto page_end   = (vaddr + size + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
	for (auto page = page_begin; page < page_end; page += TRACKER_PAGE_SIZE) {
		if (!m_gpu_modified_ranges.Intersects(page, TRACKER_PAGE_SIZE)) {
			m_memory_tracker.UnmarkRegionAsGpuModified(page, TRACKER_PAGE_SIZE);
		}
	}
	m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
}

void BufferCache::ReadMemoryOnGpu(uint64_t vaddr, uint64_t size, bool is_write) {
	if (is_write && !IsRegionRegistered(vaddr, size)) {
		return;
	}
	auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];

	// Widen nearby CPU reads so they share one GPU drain, then fold other small
	// dirty ranges into the same Finish so later faults in this frame are free.
	constexpr uint64_t WindowSize   = 512 * 1024;
	const auto         buffer_begin = buffer.CpuAddress();
	const auto         buffer_end   = buffer_begin + buffer.Size();
	const auto         window_begin = std::max(vaddr & ~(WindowSize - 1), buffer_begin);
	const auto window_end = std::min(std::max(window_begin + WindowSize, vaddr + size), buffer_end);

	std::vector<DownloadCopy> copies;
	m_memory_tracker.ForEachDownloadRange<false>(
	    window_begin, window_end - window_begin,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "memory invalidation");
	    },
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    for (const auto range: m_gpu_modified_ranges.Intersections(address, bytes)) {
			    copies.push_back(
			        {&buffer, buffer.Offset(range.address), range.address, range.size});
		    }
	    });
	if (!copies.empty()) {
		AppendSmallDirtyDownloads(copies);
		DownloadBufferMemory(copies);
		// Tracker bits are 4 KiB pages. Release a page only when no GPU-dirty
		// bytes remain on it: a 4-byte unmark would clear siblings and trip GC,
		// and a copy that straddles the 512 KiB window must still release the
		// pages it fully drained.
		const auto unmark_clean_pages = [this](uint64_t begin, uint64_t end) {
			if (end <= begin) {
				return;
			}
			const auto page_begin = begin & ~(TRACKER_PAGE_SIZE - 1);
			const auto page_end =
			    (end + TRACKER_PAGE_SIZE - 1) & ~(TRACKER_PAGE_SIZE - 1);
			for (auto page = page_begin; page < page_end; page += TRACKER_PAGE_SIZE) {
				if (!m_gpu_modified_ranges.Intersects(page, TRACKER_PAGE_SIZE)) {
					m_memory_tracker.UnmarkRegionAsGpuModified(page, TRACKER_PAGE_SIZE);
				}
			}
		};
		unmark_clean_pages(window_begin, window_end);
		for (const auto& copy: copies) {
			unmark_clean_pages(copy.address, copy.address + copy.size);
		}
	}
	if (is_write) {
		m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
	}
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			if (expands_right) {
				end += std::min(StreamLeapSize, PageTable::kAddressSpaceSize - end);
			}
			if (expands_left) {
				const auto minimum = CACHING_PAGESIZE * 2;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	EXIT_IF(m_scheduler.Current().IsInvalid());
	const auto end = (vaddr + size + CACHING_PAGESIZE - 1) & ~(CACHING_PAGESIZE - 1);
	vaddr &= ~(CACHING_PAGESIZE - 1);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	vk::Buffer                  source;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept { source = UploadCopies(buffer, copies, total_size); });
	if (source) {
		auto& command = m_scheduler.Current();
		command.EndRendering();
		const auto native = command.Handle();
		vk::BufferMemoryBarrier before {};
		before.srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                       vk::AccessFlagBits::eTransferRead |
		                       vk::AccessFlagBits::eTransferWrite;
		before.dstAccessMask       = vk::AccessFlagBits::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = buffer.Handle();
		before.offset              = 0;
		before.size                = buffer.Size();
		native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
		                       vk::PipelineStageFlagBits::eTransfer,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &before, 0, nullptr);
		native.copyBuffer(source, buffer.Handle(), static_cast<uint32_t>(copies.size()),
		                  copies.data());
		auto after          = before;
		after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
		after.dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
		native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                       vk::PipelineStageFlagBits::eAllCommands,
		                       vk::DependencyFlagBits::eByRegion, 0, nullptr, 1, &after, 0, nullptr);
	}
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

vk::Buffer BufferCache::UploadCopies(Buffer& buffer, std::span<vk::BufferCopy> copies,
                                     uint64_t total_size) {
	if (copies.empty()) {
		return nullptr;
	}

	auto [mapped, base_offset] = m_staging_buffer.Map(total_size, 4);
	if (mapped != nullptr) {
		for (auto& copy: copies) {
			const auto address = buffer.CpuAddress() + copy.dstOffset;
			std::memcpy(mapped + copy.srcOffset, reinterpret_cast<const void*>(address), copy.size);
			copy.srcOffset += base_offset;
		}
		m_staging_buffer.Commit();
		return m_staging_buffer.Handle();
	}

	auto temporary = std::make_unique<Buffer>(m_graphics, m_scheduler, MemoryUsage::Upload, 0,
	                                         vk::BufferUsageFlagBits::eTransferSrc, total_size);
	for (const auto& copy: copies) {
		const auto address = buffer.CpuAddress() + copy.dstOffset;
		std::memcpy(temporary->Mapped().data() + copy.srcOffset,
		            reinterpret_cast<const void*>(address), copy.size);
	}
	temporary->Flush(0, total_size);
	const auto handle = temporary->Handle();
	m_scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return handle;
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	auto& command = m_scheduler.Current();
	if (command.IsInvalid() || !GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: buffer request requires a recording command buffer\n");
	}

	if (!is_written && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr && Libs::LibKernel::Memory::TryReadBacking(vaddr, mapped, size)) {
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	auto* buffer = m_slot_buffers.try_get(id);
	if (buffer == nullptr || buffer->is_deleted || !buffer->IsInBounds(vaddr, size)) {
		id     = FindBuffer(vaddr, size);
		buffer = &m_slot_buffers[id];
	}
	TouchBuffer(*buffer);
	(void)SynchronizeBuffer(*buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written) {
		m_gpu_modified_ranges.Add(vaddr, size);
		// The write is being recorded into the command buffer that owns this tick.
		buffer->NoteGpuWrite(m_scheduler.CurrentTick());
	}
	return {buffer, buffer->Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid image source\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	auto [staging, stage_offset] = m_staging_buffer.Map(size, 16);
	if (staging == nullptr || (!Libs::LibKernel::Memory::TryReadBacking(vaddr, staging, size) &&
	                           !Libs::LibKernel::Memory::TryReadPrtBacking(vaddr, staging, size))) {
		EXIT("BufferCache: failed to read mapped guest image backing\n");
	}
	m_staging_buffer.Commit();
	return {&m_staging_buffer, stage_offset};
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	const auto id          = FindBuffer(vaddr, size);
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true, id);
	EXIT_IF(dst == nullptr);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	EXIT_IF(src == nullptr || dst == nullptr);
	if (src == dst && src_offset < dst_offset + size && dst_offset < src_offset + size) {
		EXIT("BufferCache: resolved Vulkan copy ranges overlap\n");
	}
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	// Land any deferred readback whose tick has retired so guest memory does not lag
	// when downloads pause (menus, load screens).
	DrainDeferredReadbacks(false);
	const auto tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	// `aggressive` decides whether a dirty buffer is DOWNLOADED (a GPU drain) or left alone.
	// m_total_used_memory is TOTAL device memory, shared with the texture cache - and the texture
	// cache cannot evict GPU-modified tiled images at all (measured: 100% of its LRU candidates
	// are tiled, deleted=0 forever while 1GB over its own critical line). So total memory is
	// pinned above critical permanently, by memory this cache does not own and cannot free, and
	// the buffer GC was left `aggressive=128/128` downloading dirty buffers forever in response.
	//
	// This cache holds a flat ~500MB of a ~6.5GB budget. Going aggressive is only meaningful when
	// evicting buffers could actually relieve the pressure, so require this cache to be holding a
	// material share of the budget as well. `KYTY_BUFGC_OWN_SHARE=0` restores the old behaviour.
	static const uint64_t own_share_pct = [] {
		const char* env = std::getenv("KYTY_BUFGC_OWN_SHARE");
		if (env == nullptr || env[0] == '\0') {
			return uint64_t {25};
		}
		char*      end    = nullptr;
		const auto parsed = std::strtoull(env, &end, 10);
		return end == env ? uint64_t {25} : parsed;
	}();
	const bool own_pressure =
	    own_share_pct == 0 ||
	    m_registered_bytes >= m_critical_gc_memory * own_share_pct / 100;
	const bool aggressive = m_total_used_memory >= m_critical_gc_memory && own_pressure;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	std::vector<DownloadCopy> copies;
	size_t                    retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		EXIT_IF(buffer.is_deleted);
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool tracker_dirty =
		    m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		const bool range_dirty =
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size());
		if ((tracker_dirty || range_dirty) && !aggressive) {
			return false;
		}
		if (tracker_dirty || range_dirty) {
			m_gpu_modified_ranges.ForEachIntersection(
			    buffer.CpuAddress(), buffer.Size(), [&](RangeSet::Range range) {
				    copies.push_back({&buffer, range.address - buffer.CpuAddress(), range.address,
				                      range.size});
			    });
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
		}
		return ++retire_count == limit;
	});
	// Why this counter exists: BufferDlCensus showed one 10 MB buffer (0x113d000000) downloaded
	// ~164x per census window, ~12x per frame, ~120 MB/frame. The demand-read path clips to a
	// 512 KiB window and AppendSmallDirtyDownloads caps seeds at 64 KiB / 4 MiB, so neither can
	// emit a 10 MB copy - only this path can, and only when `aggressive`. That means the cache
	// is pinned at critical VRAM pressure and is evicting-and-redownloading the same live buffer
	// every frame. Report the pressure and the volume so that is measured, not assumed.
	{
		static std::atomic<uint32_t> runs {0}, aggressive_runs {0};
		static std::atomic<uint64_t> bytes {0};
		uint64_t                     run_bytes = 0;
		for (const auto& c: copies) {
			run_bytes += c.size;
		}
		bytes.fetch_add(run_bytes, std::memory_order_relaxed);
		if (aggressive) {
			aggressive_runs.fetch_add(1, std::memory_order_relaxed);
		}
		if ((runs.fetch_add(1, std::memory_order_relaxed) % 128) == 127) {
			// Is the VRAM growth even in buffers? GetDeviceMemoryUsage() is TOTAL device memory
			// (VMA), shared with the texture cache, so `used` climbing does not by itself mean
			// the buffer cache is the one growing. Census what this cache actually holds, split
			// by dirty/clean, so the growth is attributed instead of assumed.
			uint64_t held = 0, dirty_bytes = 0, clean_bytes = 0;
			uint32_t count = 0, dirty_count = 0, old_clean = 0;
			m_slot_buffers.ForEach([&](BufferId id, const Buffer& buffer) {
				if (buffer.is_deleted) {
					return;
				}
				count++;
				held += buffer.Size();
				if (m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
					dirty_count++;
					dirty_bytes += buffer.Size();
				} else {
					clean_bytes += buffer.Size();
					old_clean++;
				}
				(void)id;
			});
			LOGF("BufferGc/128: aggressive=%u/128 used=%" PRIu64 "MB trigger=%" PRIu64
			     "MB critical=%" PRIu64 "MB downloaded=%" PRIu64 "MB dirtylist=%zu | "
			     "cache=%uk buffers %" PRIu64 "MB (dirty %u/%" PRIu64 "MB clean %u/%" PRIu64
			     "MB)\n",
			     aggressive_runs.exchange(0), m_total_used_memory / (1024 * 1024),
			     m_trigger_gc_memory / (1024 * 1024), m_critical_gc_memory / (1024 * 1024),
			     bytes.exchange(0) / (1024 * 1024), dirty_buffers.size(), count / 1000,
			     held / (1024 * 1024), dirty_count, dirty_bytes / (1024 * 1024), old_clean,
			     clean_bytes / (1024 * 1024));
			// Sanity-check the running counter against the walk it replaces. Exact equality is
			// the wrong test: the walk skips `is_deleted` buffers while the counter tracks
			// register/unregister, so they legitimately differ by a few bytes in flight. Only a
			// difference big enough to move the 25%-of-budget gate matters.
			const auto drift = held > m_registered_bytes ? held - m_registered_bytes
			                                             : m_registered_bytes - held;
			if (drift > 16 * MiB) {
				LOGF("BufferGc: registered_bytes drift: counter=%" PRIu64 "MB walk=%" PRIu64
				     "MB\n",
				     m_registered_bytes / (1024 * 1024), held / (1024 * 1024));
			}
		}
	}

	if (dirty_buffers.empty()) {
		return;
	}

	if (!copies.empty()) {
		DownloadBufferMemory(copies);
	}
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size()) ||
		    m_gpu_modified_ranges.Intersects(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: garbage collection retained GPU ownership\n");
		}
		m_memory_tracker.UntrackMemory(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		// DownloadBufferMemory() above recorded GPU->staging copies against these very
		// buffers into the command buffer that is still recording, and the deferred
		// readback path does not Finish() before returning. Destroying the VkBuffer here
		// invalidates that command buffer ("VkBuffer ... was destroyed" /
		// VUID-vkCmdPipelineBarrier-commandBuffer-recording) and loses the device. Retire
		// on tick completion instead, exactly as DeleteBuffer() does.
		if (m_scheduler.Active()) {
			m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
		} else {
			m_slot_buffers.erase(id);
		}
	}
}

void BufferCache::ProcessFaultBuffer() {
	FrameWorkScope faultbuf_scope(FrameWorkKind::FaultBuf);
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::SynchronizeBuffersInRange(uint64_t vaddr, uint64_t size) {
	const auto end = vaddr + size;
	auto       it  = m_buffers.upper_bound(vaddr);
	if (it != m_buffers.begin()) {
		--it;
	}
	for (; it != m_buffers.end() && it->first < end; ++it) {
		auto&      buffer = m_slot_buffers[it->second];
		const auto start  = std::max(buffer.CpuAddress(), vaddr);
		const auto finish = std::min(buffer.CpuAddress() + buffer.Size(), end);
		if (start < finish) {
			(void)SynchronizeBuffer(buffer, start, finish - start, false, false);
		}
	}
}

} // namespace Libs::Graphics
