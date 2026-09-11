#include "graphics/host_gpu/renderer/cache/gpuResourceManager.h"

#include "common/assert.h"
#include "common/timer.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/debug.h"

#include <atomic>
namespace Libs::Graphics {

GpuResourceManager::GpuResourceManager(GraphicContext& graphics, CommandScheduler& scheduler)
    : m_scheduler(scheduler), m_buffer_cache(graphics, scheduler, m_page_manager, m_texture_cache),
      m_texture_cache(graphics, scheduler, m_page_manager, m_buffer_cache) {}

GpuResourceManager::~GpuResourceManager() = default;

bool GpuResourceManager::HandleFault(PageFaultAccess access, uint64_t fault_vaddr) noexcept {
	// The host reports the faulting byte, not the instruction's access width. Both caches
	// resolve its page; guessing a width can cross the end of a valid guest mapping.
	constexpr uint64_t fault_size = 1;
	if (!IsMapped(fault_vaddr, fault_size)) {
		return false;
	}
	if (access == PageFaultAccess::Write) {
		m_buffer_cache.InvalidateMemory(fault_vaddr, fault_size);
		m_texture_cache.InvalidateMemory(fault_vaddr, fault_size);
	} else {
		m_buffer_cache.ReadMemory(fault_vaddr, fault_size);
	}
	return true;
}

bool GpuResourceManager::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	if (!IsMapped(vaddr, size)) {
		return false;
	}
	m_buffer_cache.InvalidateMemory(vaddr, size);
	m_texture_cache.InvalidateMemory(vaddr, size);
	return true;
}

bool GpuResourceManager::IsMapped(uint64_t vaddr, uint64_t size) const noexcept {
	if (!GuestRange {vaddr, size}.Valid()) {
		return false;
	}
	std::shared_lock lock(m_mapped_ranges_mutex);
	return m_mapped_ranges.Contains(vaddr, size);
}

void GpuResourceManager::MapMemory(uint64_t vaddr, uint64_t size) {
	{
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Add(vaddr, size);
	}
}

void GpuResourceManager::UnmapMemory(uint64_t vaddr, uint64_t size) {
	if (CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported memory unmap from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	bool intersects = false;
	{
		std::shared_lock lock(m_mapped_ranges_mutex);
		intersects = m_mapped_ranges.Intersects(vaddr, size);
	}
	{
		static std::atomic<uint32_t> total {0}, mapped_hits {0};
		const auto n = total.fetch_add(1, std::memory_order_relaxed) + 1;
		const auto h = mapped_hits.fetch_add(intersects ? 1 : 0, std::memory_order_relaxed) +
		               (intersects ? 1 : 0);
		if ((n % 8192) == 0) {
			LOGF("UnmapStats: total=%u gpu_mapped=%u (%.1f%%)\n", n, h, 100.0 * h / n);
		}
	}
	// ~98% of guest unmaps are for memory the GPU never mapped, so neither cache has
	// anything to drop - yet the full path still pays a blocking SendCommandSync
	// round-trip onto the (already saturated) GPU worker thread, several thousand
	// times a frame. Skip it.
	//
	// Unmaps are also a frequent drain point for deferred operations - EOP label
	// commits land in WaitPriorityOperations/PopPendingOperations - so keep pumping
	// those or the guest can spin forever on a label that never gets written (this is
	// what made the naive version of this skip livelock). One async pump is kept in
	// flight at a time, coalescing every unmap that arrives while it is queued.
	if (!intersects && m_gpu != nullptr && !CommandScheduler::InDeferredOperation()) {
		if (!m_deferred_pump_queued.exchange(true, std::memory_order_acq_rel)) {
			m_gpu->SendCommand([this] {
				m_deferred_pump_queued.store(false, std::memory_order_release);
				if (m_scheduler.Active()) {
					m_scheduler.SyncDeferredOperations();
				}
			});
		}
		return;
	}
	const auto unmap = [this, vaddr, size] {
		// Do not Finish()/idle-wait the GPU. Waiting on the current tick deadlocked
		// boot (priority callbacks wait for a tick this thread would have to
		// submit). Drain only already-signaled ticks. GPU-dirty bytes still wait
		// inside InvalidateMemory's buffer-download path.
		if (m_scheduler.Active()) {
			m_scheduler.SyncDeferredOperations();
		}
		// Land any deferred staging->guest readback before this range disappears,
		// otherwise its writeback would hit unmapped (or re-mapped) memory.
		m_buffer_cache.DrainDeferredReadbacks(true);
		// Discard rather than write back: the guest is destroying this range, so the
		// GPU->CPU download InvalidateMemory would do is unobservable work, and its
		// 512 KiB widening drags unrelated dirty ranges into the same GPU idle.
		m_buffer_cache.DiscardMemory(vaddr, size);
		m_texture_cache.UnmapMemory(vaddr, size);
		std::lock_guard lock(m_mapped_ranges_mutex);
		m_mapped_ranges.Subtract(vaddr, size);
	};
	if (m_gpu == nullptr) {
		unmap();
		return;
	}
	m_gpu->SendCommandSync(unmap);
}

void GpuResourceManager::PrepareBda() {
	std::shared_lock lock(m_mapped_ranges_mutex);
	m_mapped_ranges.ForEach([this](uint64_t start, uint64_t end) {
		m_buffer_cache.SynchronizeBuffersInRange(start, end - start);
	});
	m_fault_process_pending = true;
}

void GpuResourceManager::RunGarbageCollector() {
	FrameWorkScope gc_scope(FrameWorkKind::Gc);
	const auto     now  = [] { return Common::Timer::QueryPerformanceCounter(); };
	const auto     t0   = now();
	if (m_fault_process_pending) {
		m_fault_process_pending = false;
		m_buffer_cache.ProcessFaultBuffer();
	}
	const auto t1 = now();
	m_texture_cache.ProcessDownloadImages();
	const auto t2 = now();
	m_texture_cache.RunGarbageCollector();
	const auto t3 = now();
	m_buffer_cache.RunGarbageCollector();
	const auto t4 = now();
	{
		static std::atomic<uint32_t> count {0};
		static std::atomic<uint64_t> fault_us {0}, dlimg_us {0}, texgc_us {0}, bufgc_us {0};
		const auto freq = Common::Timer::QueryPerformanceFrequency();
		const auto us   = [freq](uint64_t a, uint64_t b) {
            return freq == 0 ? 0ull : (b - a) * 1000000ull / freq;
		};
		fault_us.fetch_add(us(t0, t1), std::memory_order_relaxed);
		dlimg_us.fetch_add(us(t1, t2), std::memory_order_relaxed);
		texgc_us.fetch_add(us(t2, t3), std::memory_order_relaxed);
		bufgc_us.fetch_add(us(t3, t4), std::memory_order_relaxed);
		if ((count.fetch_add(1, std::memory_order_relaxed) % 512) == 511) {
			LOGF("GcSplit/512: fault=%.1fms dlimg=%.1fms texgc=%.1fms bufgc=%.1fms\n",
			     fault_us.exchange(0) / 1000.0, dlimg_us.exchange(0) / 1000.0,
			     texgc_us.exchange(0) / 1000.0, bufgc_us.exchange(0) / 1000.0);
		}
	}
}

} // namespace Libs::Graphics
