#include "graphics/host_gpu/renderer/watchedImageTrace.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/dispatchInspector.h"
#include "graphics/host_gpu/renderer/image/image.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>

namespace Libs::Graphics {
namespace {

constexpr const char* kWatchFile = "D:/PS5/dumps/WATCH_IMAGE";
constexpr const char* kBreakFile = "D:/PS5/dumps/WATCH_BREAK_WRITE";
constexpr const char* kGoodFile  = "D:/PS5/dumps/WATCH_SNAPSHOT_GOOD";
constexpr const char* kBadFile   = "D:/PS5/dumps/WATCH_SNAPSHOT_BAD";
constexpr const char* kDiffFile  = "D:/PS5/dumps/watch_image_diff.txt";
constexpr const char* kGoodJson  = "D:/PS5/dumps/watch_image_good.json";
constexpr const char* kBadJson   = "D:/PS5/dumps/watch_image_bad.json";

std::mutex g_lock;
std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_watch_addr{0};
std::atomic<uint32_t> g_watch_index{Common::SlotId::INVALID_INDEX};
std::atomic<uint32_t> g_watch_generation{0};
std::atomic<bool> g_break_on_write{false};
std::atomic<bool> g_arm_good{false};
std::atomic<bool> g_arm_bad{false};
std::atomic<bool> g_ui_probe{false};
uint64_t g_ctx_frame = 0;
uint32_t g_ctx_op = UINT32_MAX;
uint64_t g_ctx_shader = 0;
char g_ctx_owner[48] = {};
WatchedImageState g_last{};
WatchedImageState g_good{};
WatchedImageState g_bad{};
bool g_have_good = false;
bool g_have_bad = false;
uint64_t g_last_view_key = 0;
int32_t g_last_view_format = -1;
int32_t g_last_view_type = -1;
uint32_t g_last_base_level = UINT32_MAX;
uint32_t g_last_level_count = 0;
uint32_t g_last_base_layer = UINT32_MAX;
uint32_t g_last_layer_count = 0;
uint32_t g_last_swizzle = 0;
std::unordered_set<uint64_t> g_ui_probe_seen;
uint32_t g_ui_probe_logged = 0;

uint32_t PackSwizzle(const vk::ComponentMapping& m) noexcept {
  auto c = [](vk::ComponentSwizzle s) { return static_cast<uint32_t>(s) & 0xfu; };
  return c(m.r) | (c(m.g) << 4) | (c(m.b) << 8) | (c(m.a) << 12);
}
bool RangesOverlap(uint64_t a, uint64_t asz, uint64_t b, uint64_t bsz) noexcept {
  if (a == 0 || b == 0) return false;
  return a < b + std::max<uint64_t>(bsz, 1) && b < a + std::max<uint64_t>(asz, 1);
}
void EnableFromConfig() {
  if (const char* addr = std::getenv("KYTY_WATCH_IMAGE_ADDR"); addr && addr[0]) {
    char* end = nullptr; auto v = std::strtoull(addr, &end, 0);
    if (end != addr && v) { g_watch_addr.store(v); g_enabled.store(true); }
  }
  if (const char* id = std::getenv("KYTY_WATCH_IMAGE_ID"); id && id[0]) {
    uint32_t index = 0, gen = 0;
    if (std::sscanf(id, "%u.%u", &index, &gen) >= 1 && index != Common::SlotId::INVALID_INDEX) {
      g_watch_index.store(index); g_watch_generation.store(gen); g_enabled.store(true);
    }
  }
  if (std::getenv("KYTY_WATCH_BREAK_WRITE")) g_break_on_write.store(true);
  const char* probe = std::getenv("KYTY_WATCH_UI_PROBE");
  g_ui_probe.store(probe && probe[0] == '1');
}
struct OnceInit { OnceInit() { EnableFromConfig(); } };
OnceInit g_once;
bool ConsumeTriggerFile(const char* path, std::string* contents) {
  std::error_code ec; if (!std::filesystem::exists(path, ec)) return false;
  std::ifstream in{path}; std::string line; std::getline(in, line); in.close();
  std::filesystem::remove(path, ec); if (contents) *contents = line; return true;
}
void FillState(WatchedImageState* s, const Image* image, Common::SlotId id, uint64_t vk_view, const ImageViewInfo* view) {
  if (!s) return;
  s->frame = g_ctx_frame ? g_ctx_frame : GetInspectorRecordingFrame();
  s->op_index = g_ctx_op != UINT32_MAX ? g_ctx_op : PeekInspectorOperationIndex(s->frame);
  s->last_writer_shader = g_ctx_shader;
  std::snprintf(s->owner, sizeof(s->owner), "%s", g_ctx_owner[0] ? g_ctx_owner : "-");
  if (image) {
    s->guest_addr = image->info.data.address; s->guest_size = image->info.data.size;
    s->vk_image = reinterpret_cast<uintptr_t>(static_cast<VkImage>(image->backing.image));
    s->guest_width = image->info.extent.width; s->guest_height = image->info.extent.height;
    s->guest_depth = image->info.extent.depth; s->guest_levels = image->GuestLevelCount();
    s->native_levels = image->NativeLevelCount(); s->guest_layers = image->info.resources.layers;
    s->guest_format = static_cast<int32_t>(image->info.guest_format);
    s->vk_format = static_cast<int32_t>(image->info.pixel_format);
    s->tile_mode = static_cast<uint32_t>(image->info.tile_mode);
    s->content_generation = image->content_generation; s->last_writer_tick = image->tick_accessed_last;
    s->cpu_dirty = image->IsCpuDirty(); s->gpu_modified = image->IsGpuModified();
    s->buffer_modified = image->IsBufferModified();
    std::snprintf(s->last_writer, sizeof(s->last_writer), "%s", image->last_writer[0] ? image->last_writer : "-");
  }
  if (id) { s->image_index = id.index; s->image_generation = id.generation; }
  s->vk_view = vk_view;
  if (view) {
    s->view_format = static_cast<int32_t>(view->format); s->view_type = static_cast<int32_t>(view->type);
    s->guest_mip = view->base_level;
    s->host_mip = (image && view->base_level < image->NativeLevelCount()) ? view->base_level : 0;
    s->layer = view->base_layer; s->level_count = view->level_count; s->layer_count = view->layer_count;
  }
}
void WriteJson(const char* path, const WatchedImageState& s, const char* label) {
  std::ofstream out{path, std::ios::trunc}; if (!out) return;
  out << "{\n  \"label\": \"" << label << "\",\n"
      << "  \"frame\": " << s.frame << ",\n"
      << "  \"guest_addr\": \"0x" << std::hex << s.guest_addr << std::dec << "\",\n"
      << "  \"image_id\": \"" << s.image_index << "." << s.image_generation << "\",\n"
      << "  \"vk_image\": \"0x" << std::hex << s.vk_image << std::dec << "\",\n"
      << "  \"vk_view\": \"0x" << std::hex << s.vk_view << std::dec << "\",\n"
      << "  \"extent\": \"" << s.guest_width << "x" << s.guest_height << "\",\n"
      << "  \"guest_mip\": " << s.guest_mip << ",\n  \"layer\": " << s.layer << ",\n"
      << "  \"vk_format\": " << s.vk_format << ",\n  \"view_format\": " << s.view_format << ",\n"
      << "  \"tile_mode\": " << s.tile_mode << ",\n  \"content_generation\": " << s.content_generation << ",\n"
      << "  \"last_writer\": \"" << s.last_writer << "\",\n  \"owner\": \"" << s.owner << "\",\n"
      << "  \"alias_count\": " << s.alias_count << "\n}\n";
}
void Emit(WatchedImageEvent event, const WatchedImageState& s, const char* detail) {
  LOGF("WatchImage: frame=%" PRIu64 " op=%u event=%s addr=0x%016" PRIx64
       " image=%u.%u vk_image=0x%016" PRIx64 " vk_view=0x%016" PRIx64
       " mip=%u layer=%u view=%dx%d gen=%" PRIu64 " dirty=%d/%d/%d alias=%u owner=%s writer=%s shader=0x%016" PRIx64 " %s\n",
       s.frame, s.op_index == UINT32_MAX ? 0u : s.op_index, WatchedImageEventName(event),
       s.guest_addr, s.image_index, s.image_generation, s.vk_image, s.vk_view,
       s.guest_mip == UINT32_MAX ? 0u : s.guest_mip, s.layer == UINT32_MAX ? 0u : s.layer,
       s.view_format, s.view_type, s.content_generation, s.cpu_dirty?1:0, s.gpu_modified?1:0,
       s.buffer_modified?1:0, s.alias_count, s.owner, s.last_writer, s.last_writer_shader,
       detail ? detail : "");
}
bool IsWrite(WatchedImageEvent e) noexcept {
  switch (e) {
    case WatchedImageEvent::Upload: case WatchedImageEvent::Materialize:
    case WatchedImageEvent::CopyDst: case WatchedImageEvent::Clear:
    case WatchedImageEvent::GpuWrite: case WatchedImageEvent::CpuWrite:
    case WatchedImageEvent::RenderTargetBind: case WatchedImageEvent::DepthTargetBind:
    case WatchedImageEvent::Recreate: case WatchedImageEvent::Retarget: return true;
    default: return false;
  }
}

// BREAK_ON_WRITE ignores initial upload/materialize — those fire on every fresh Image and
// previously aborted the session before the menu icon could be observed.
bool IsBreakWrite(WatchedImageEvent e) noexcept {
  switch (e) {
    case WatchedImageEvent::CopyDst: case WatchedImageEvent::Clear:
    case WatchedImageEvent::GpuWrite: case WatchedImageEvent::CpuWrite:
    case WatchedImageEvent::RenderTargetBind: case WatchedImageEvent::DepthTargetBind:
    case WatchedImageEvent::Recreate: case WatchedImageEvent::Retarget: return true;
    default: return false;
  }
}
} // namespace


const char* WatchedImageEventName(WatchedImageEvent event) noexcept {
	switch (event) {
		case WatchedImageEvent::Create: return "CREATE";
		case WatchedImageEvent::Upload: return "UPLOAD";
		case WatchedImageEvent::Materialize: return "MATERIALIZE";
		case WatchedImageEvent::SampleBind: return "SAMPLE_BIND";
		case WatchedImageEvent::StorageBind: return "STORAGE_BIND";
		case WatchedImageEvent::RenderTargetBind: return "RENDER_TARGET_BIND";
		case WatchedImageEvent::DepthTargetBind: return "DEPTH_TARGET_BIND";
		case WatchedImageEvent::CopySrc: return "COPY_SRC";
		case WatchedImageEvent::CopyDst: return "COPY_DST";
		case WatchedImageEvent::Clear: return "CLEAR";
		case WatchedImageEvent::GpuWrite: return "GPU_WRITE";
		case WatchedImageEvent::CpuWrite: return "CPU_WRITE";
		case WatchedImageEvent::Download: return "DOWNLOAD";
		case WatchedImageEvent::AliasResolve: return "ALIAS_RESOLVE";
		case WatchedImageEvent::Retarget: return "RETARGET";
		case WatchedImageEvent::Recreate: return "RECREATE";
		case WatchedImageEvent::Delete: return "DELETE";
		case WatchedImageEvent::GcConsider: return "GC_CONSIDER";
		case WatchedImageEvent::GcSkip: return "GC_SKIP";
		case WatchedImageEvent::GcDelete: return "GC_DELETE";
		case WatchedImageEvent::ViewCreate: return "VIEW_CREATE";
		case WatchedImageEvent::ViewReuse: return "VIEW_REUSE";
		case WatchedImageEvent::ViewChange: return "VIEW_CHANGE";
		case WatchedImageEvent::SnapshotGood: return "SNAPSHOT_GOOD";
		case WatchedImageEvent::SnapshotBad: return "SNAPSHOT_BAD";
		case WatchedImageEvent::BreakBefore: return "BREAK_BEFORE";
		case WatchedImageEvent::BreakAfter: return "BREAK_AFTER";
	}
	return "UNKNOWN";
}

bool WatchedImageEnabled() noexcept {
	return g_enabled.load(std::memory_order_relaxed) || g_ui_probe.load(std::memory_order_relaxed);
}

bool WatchedImageMatches(uint64_t guest_addr, uint64_t guest_size, Common::SlotId image_id) noexcept {
	if (!g_enabled.load(std::memory_order_relaxed)) return false;
	const auto watch_index = g_watch_index.load(std::memory_order_relaxed);
	if (watch_index != Common::SlotId::INVALID_INDEX && image_id) {
		const auto watch_gen = g_watch_generation.load(std::memory_order_relaxed);
		if (image_id.index == watch_index && (watch_gen == 0 || image_id.generation == watch_gen))
			return true;
	}
	const auto watch_addr = g_watch_addr.load(std::memory_order_relaxed);
	return watch_addr != 0 && RangesOverlap(watch_addr, 1, guest_addr, guest_size);
}

bool WatchedImageMatchesImage(const Image& image, Common::SlotId image_id) noexcept {
	return WatchedImageMatches(image.info.data.address, image.info.data.size, image_id);
}

void WatchedImageSetAddress(uint64_t guest_addr) noexcept {
	g_watch_addr.store(guest_addr, std::memory_order_relaxed);
	g_watch_index.store(Common::SlotId::INVALID_INDEX, std::memory_order_relaxed);
	g_watch_generation.store(0, std::memory_order_relaxed);
	g_enabled.store(guest_addr != 0, std::memory_order_relaxed);
	LOGF("WatchImage: watch addr=0x%016" PRIx64 "\n", guest_addr);
}

void WatchedImageSetImageId(uint32_t index, uint32_t generation) noexcept {
	g_watch_index.store(index, std::memory_order_relaxed);
	g_watch_generation.store(generation, std::memory_order_relaxed);
	g_enabled.store(index != Common::SlotId::INVALID_INDEX, std::memory_order_relaxed);
	LOGF("WatchImage: watch image=%u.%u\n", index, generation);
}

void WatchedImageClearWatch() noexcept {
	g_watch_addr.store(0, std::memory_order_relaxed);
	g_watch_index.store(Common::SlotId::INVALID_INDEX, std::memory_order_relaxed);
	g_watch_generation.store(0, std::memory_order_relaxed);
	g_enabled.store(false, std::memory_order_relaxed);
	LOGF("WatchImage: cleared\n");
}

void WatchedImageArmBreakOnWrite() noexcept {
	g_break_on_write.store(true, std::memory_order_relaxed);
	LOGF("WatchImage: armed BREAK_ON_WRITE\n");
}
void WatchedImageArmSnapshotGood() noexcept {
	g_arm_good.store(true, std::memory_order_relaxed);
	LOGF("WatchImage: armed SNAPSHOT_GOOD\n");
}
void WatchedImageArmSnapshotBad() noexcept {
	g_arm_bad.store(true, std::memory_order_relaxed);
	LOGF("WatchImage: armed SNAPSHOT_BAD\n");
}

void WatchedImageRefreshTriggers() noexcept {
	std::string text;
	if (ConsumeTriggerFile(kWatchFile, &text)) {
		uint32_t index = 0, generation = 0; uint64_t addr = 0;
		if (std::sscanf(text.c_str(), "%u.%u", &index, &generation) >= 1 &&
		    index != Common::SlotId::INVALID_INDEX && text.find('.') != std::string::npos) {
			WatchedImageSetImageId(index, generation);
		} else if (std::sscanf(text.c_str(), "%" SCNx64, &addr) == 1 ||
		           std::sscanf(text.c_str(), "0x%" SCNx64, &addr) == 1) {
			WatchedImageSetAddress(addr);
		} else if (text.empty()) {
			WatchedImageClearWatch();
		}
	}
	if (ConsumeTriggerFile(kBreakFile, nullptr)) WatchedImageArmBreakOnWrite();
	if (ConsumeTriggerFile(kGoodFile, nullptr)) WatchedImageArmSnapshotGood();
	if (ConsumeTriggerFile(kBadFile, nullptr)) WatchedImageArmSnapshotBad();
}

void WatchedImageSetContext(uint64_t frame, uint32_t op_index, uint64_t shader_hash,
                            const char* owner) noexcept {
	g_ctx_frame = frame; g_ctx_op = op_index; g_ctx_shader = shader_hash;
	if (owner) std::snprintf(g_ctx_owner, sizeof(g_ctx_owner), "%s", owner);
	else g_ctx_owner[0] = '\0';
}

void WatchedImageNote(WatchedImageEvent event, const Image* image, Common::SlotId image_id,
                      const char* detail, uint32_t mip, uint32_t layer, uint64_t vk_view,
                      const ImageViewInfo* view) {
	if (!g_enabled.load(std::memory_order_relaxed)) return;
	if (image) {
		if (!WatchedImageMatchesImage(*image, image_id)) return;
	} else if (image_id) {
		if (!WatchedImageMatches(0, 0, image_id)) return;
	} else return;

	std::scoped_lock guard{g_lock};
	WatchedImageState state{};
	FillState(&state, image, image_id, vk_view, view);
	if (mip != UINT32_MAX) state.guest_mip = mip;
	if (layer != UINT32_MAX) state.layer = layer;

	const bool do_break = g_break_on_write.load(std::memory_order_relaxed) && IsBreakWrite(event);
	// Capture pre-write metadata before g_last is overwritten (GOOD_AUTO must differ from BAD).
	const WatchedImageState pre_write = g_last.guest_addr ? g_last : state;
	if (do_break) Emit(WatchedImageEvent::BreakBefore, pre_write, "before write");
	Emit(event, state, detail);
	g_last = state;

	if (g_arm_good.exchange(false, std::memory_order_relaxed)) {
		g_good = state; g_have_good = true; WriteJson(kGoodJson, g_good, "GOOD");
		Emit(WatchedImageEvent::SnapshotGood, state, "captured");
		if (g_have_bad) WatchedImageWriteDifferentialReport();
	}
	if (g_arm_bad.exchange(false, std::memory_order_relaxed)) {
		g_bad = state; g_have_bad = true; WriteJson(kBadJson, g_bad, "BAD");
		Emit(WatchedImageEvent::SnapshotBad, state, "captured");
		if (g_have_good) WatchedImageWriteDifferentialReport();
	}
	if (do_break) {
		// Soft break only: log + snapshots. Never DebugBreak — that terminates without a debugger.
		g_break_on_write.store(false, std::memory_order_relaxed);
		Emit(WatchedImageEvent::BreakAfter, state, "after write");
		if (!g_have_good) {
			g_good = pre_write;
			g_have_good = true;
			WriteJson(kGoodJson, g_good, "GOOD_AUTO");
		}
		g_bad = state; g_have_bad = true; WriteJson(kBadJson, g_bad, "BAD_AUTO");
		WatchedImageWriteDifferentialReport();
	}
}

void WatchedImageNoteBind(WatchedImageEvent event, Image& image, Common::SlotId image_id,
                          uint64_t vk_view, const ImageViewInfo& view, uint64_t shader_hash,
                          const char* owner) {
	WatchedImageSetContext(GetInspectorRecordingFrame(),
	                       PeekInspectorOperationIndex(GetInspectorRecordingFrame()), shader_hash, owner);
	if (WatchedImageMatchesImage(image, image_id)) {
		const auto swizzle = PackSwizzle(view.mapping);
		const auto key = (static_cast<uint64_t>(image_id.index) << 32) | image_id.generation;
		if (g_last_view_key == key &&
		    (g_last_view_format != static_cast<int32_t>(view.format) ||
		     g_last_view_type != static_cast<int32_t>(view.type) ||
		     g_last_base_level != view.base_level || g_last_level_count != view.level_count ||
		     g_last_base_layer != view.base_layer || g_last_layer_count != view.layer_count ||
		     g_last_swizzle != swizzle)) {
			char detail[160];
			std::snprintf(detail, sizeof(detail),
			              "prev_fmt=%d->%d type=%d->%d mip=%u->%u levels=%u->%u layer=%u->%u swizzle=0x%x->0x%x",
			              g_last_view_format, static_cast<int>(view.format), g_last_view_type,
			              static_cast<int>(view.type), g_last_base_level, view.base_level,
			              g_last_level_count, view.level_count, g_last_base_layer, view.base_layer,
			              g_last_swizzle, swizzle);
			WatchedImageNote(WatchedImageEvent::ViewChange, &image, image_id, detail,
			                 view.base_level, view.base_layer, vk_view, &view);
		}
		g_last_view_key = key;
		g_last_view_format = static_cast<int32_t>(view.format);
		g_last_view_type = static_cast<int32_t>(view.type);
		g_last_base_level = view.base_level;
		g_last_level_count = view.level_count;
		g_last_base_layer = view.base_layer;
		g_last_layer_count = view.layer_count;
		g_last_swizzle = swizzle;
	}
	WatchedImageNote(event, &image, image_id, nullptr, view.base_level, view.base_layer, vk_view, &view);
}

void WatchedImageBumpContent(Image& image, Common::SlotId image_id, WatchedImageEvent event,
                             const char* writer, uint32_t mip, uint32_t layer) {
	const auto before = image.content_generation;
	++image.content_generation;
	if (mip < image.content_generation_mips.size()) ++image.content_generation_mips[mip];
	else if (mip != UINT32_MAX && mip < 16) {
		image.content_generation_mips.resize(mip + 1, 0);
		++image.content_generation_mips[mip];
	}
	if (writer) std::snprintf(image.last_writer, sizeof(image.last_writer), "%s", writer);
	if (!WatchedImageMatchesImage(image, image_id)) return;
	char detail[128];
	std::snprintf(detail, sizeof(detail), "gen %" PRIu64 "->%" PRIu64 " by %s", before,
	              image.content_generation, writer ? writer : "?");
	WatchedImageNote(event, &image, image_id, detail, mip, layer);
}

void WatchedImageNoteAliasOverlap(uint64_t guest_addr, uint64_t guest_size,
                                  Common::SlotId primary_id, uint32_t overlap_count,
                                  const char* detail) {
	if (!WatchedImageMatches(guest_addr, guest_size, primary_id)) return;
	char buffer[192];
	std::snprintf(buffer, sizeof(buffer), "overlap_count=%u primary=%u.%u %s", overlap_count,
	              primary_id.index, primary_id.generation, detail ? detail : "");
	WatchedImageNote(WatchedImageEvent::AliasResolve, nullptr, primary_id, buffer);
	std::scoped_lock guard{g_lock};
	g_last.alias_count = overlap_count;
	g_last.guest_addr = guest_addr;
	g_last.guest_size = guest_size;
}

void WatchedImageNoteGc(WatchedImageEvent event, const Image* image, Common::SlotId image_id,
                        const char* reason) {
	WatchedImageNote(event, image, image_id, reason);
}

WatchedImageState WatchedImagePeekLastState() noexcept {
	std::scoped_lock guard{g_lock}; return g_last;
}
bool WatchedImageHasGoodSnapshot() noexcept { std::scoped_lock g{g_lock}; return g_have_good; }
bool WatchedImageHasBadSnapshot() noexcept { std::scoped_lock g{g_lock}; return g_have_bad; }

void WatchedImageWriteDifferentialReport() noexcept {
	std::scoped_lock guard{g_lock};
	if (!g_have_good || !g_have_bad) return;
	std::ofstream out{kDiffFile, std::ios::trunc}; if (!out) return;
	auto row = [&](const char* field, const std::string& good, const std::string& bad) {
		out << field << '\t' << good << '\t' << bad << (good == bad ? "\tSAME\n" : "\tDIFF\n");
	};
	out << "field\tGOOD\tBAD\tcmp\n";
	char a[64], b[64];
	row("frame", std::to_string(g_good.frame), std::to_string(g_bad.frame));
	std::snprintf(a, sizeof(a), "0x%016" PRIx64, g_good.guest_addr);
	std::snprintf(b, sizeof(b), "0x%016" PRIx64, g_bad.guest_addr);
	row("guest_addr", a, b);
	std::snprintf(a, sizeof(a), "%u.%u", g_good.image_index, g_good.image_generation);
	std::snprintf(b, sizeof(b), "%u.%u", g_bad.image_index, g_bad.image_generation);
	row("ImageId", a, b);
	std::snprintf(a, sizeof(a), "0x%016" PRIx64, g_good.vk_image);
	std::snprintf(b, sizeof(b), "0x%016" PRIx64, g_bad.vk_image);
	row("VkImage", a, b);
	std::snprintf(a, sizeof(a), "0x%016" PRIx64, g_good.vk_view);
	std::snprintf(b, sizeof(b), "0x%016" PRIx64, g_bad.vk_view);
	row("view", a, b);
	row("mip", std::to_string(g_good.guest_mip), std::to_string(g_bad.guest_mip));
	row("layer", std::to_string(g_good.layer), std::to_string(g_bad.layer));
	row("format", std::to_string(g_good.vk_format), std::to_string(g_bad.vk_format));
	row("view_format", std::to_string(g_good.view_format), std::to_string(g_bad.view_format));
	std::snprintf(a, sizeof(a), "%ux%u", g_good.guest_width, g_good.guest_height);
	std::snprintf(b, sizeof(b), "%ux%u", g_bad.guest_width, g_bad.guest_height);
	row("extent", a, b);
	row("tile", std::to_string(g_good.tile_mode), std::to_string(g_bad.tile_mode));
	row("owner", g_good.owner, g_bad.owner);
	row("content_generation", std::to_string(g_good.content_generation), std::to_string(g_bad.content_generation));
	row("last_writer", g_good.last_writer, g_bad.last_writer);
	std::snprintf(a, sizeof(a), "0x%016" PRIx64, g_good.last_writer_shader);
	std::snprintf(b, sizeof(b), "0x%016" PRIx64, g_bad.last_writer_shader);
	row("last_writer_shader", a, b);
	row("alias_count", std::to_string(g_good.alias_count), std::to_string(g_bad.alias_count));
	LOGF("WatchImage: wrote differential report %s\n", kDiffFile);
}

bool WatchedImageUiProbeEnabled() noexcept { return g_ui_probe.load(std::memory_order_relaxed); }

void WatchedImageNoteUiProbeSample(uint64_t frame, uint64_t shader_hash, Image& image,
                                   Common::SlotId image_id, uint64_t vk_view,
                                   const ImageViewInfo& view) {
	if (!WatchedImageUiProbeEnabled()) return;
	const auto w = image.info.extent.width, h = image.info.extent.height;
	if (w == 0 || h == 0 || w > 512 || h > 512) return;
	const auto key = image.info.data.address ^ (static_cast<uint64_t>(w) << 32) ^ h;
	std::scoped_lock guard{g_lock};
	if (!g_ui_probe_seen.insert(key).second || g_ui_probe_logged >= 64) return;
	++g_ui_probe_logged;
	LOGF("WatchImageUiProbe: frame=%" PRIu64 " shader=0x%016" PRIx64
	     " addr=0x%016" PRIx64 " size=0x%" PRIx64 " image=%u.%u vk_image=0x%016" PRIx64
	     " vk_view=0x%016" PRIx64 " extent=%ux%ux%u type=%u host_type=%d levels=%u/%u layers=%u "
	     "guest_fmt=%d vk_fmt=%d tile=%u view_fmt=%d view_type=%d mip=%u+%u layer=%u+%u "
	     "cpu_dirty=%d gpu_mod=%d\n",
	     frame, shader_hash, image.info.data.address, image.info.data.size, image_id.index,
	     image_id.generation, reinterpret_cast<uintptr_t>(static_cast<VkImage>(image.backing.image)),
	     vk_view, w, h, image.info.extent.depth, static_cast<uint32_t>(image.info.type),
	     static_cast<int>(image.backing.image_type), image.GuestLevelCount(), image.NativeLevelCount(),
	     image.info.resources.layers, static_cast<int>(image.info.guest_format),
	     static_cast<int>(image.info.pixel_format), static_cast<uint32_t>(image.info.tile_mode),
	     static_cast<int>(view.format), static_cast<int>(view.type), view.base_level, view.level_count,
	     view.base_layer, view.layer_count, image.IsCpuDirty() ? 1 : 0,
	     image.IsGpuModified() ? 1 : 0);
}

} // namespace Libs::Graphics
