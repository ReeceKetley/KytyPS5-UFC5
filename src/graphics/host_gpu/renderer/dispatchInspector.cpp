#include "graphics/host_gpu/renderer/dispatchInspector.h"

#include "common/logging/log.h"
#include "common/singleton.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/debug.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "loader/runtimeLinker.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics {

namespace {

constexpr size_t kMaxOperations = 32768;
constexpr size_t kMaxResources  = 524288;
constexpr auto   kDumpTrigger   = "D:/PS5/dumps/DUMP_INSPECTOR";
constexpr uint64_t kPageShift   = 20;

struct InspectorState {
	std::mutex                             mutex;
	InspectorFrame                         current;
	InspectorFrame                         complete;
	std::unordered_map<uint64_t, uint32_t> dispatch_counts;
	std::unordered_map<uint64_t, uint32_t> shader_probe_generations;
	size_t                                 resource_count = 0;
	InspectorCaptureArm                    capture_arm;
	std::vector<InspectorCaptureRecord>    captures;
};

thread_local InspectorCallsite g_submit_callsite;
thread_local InspectorCallsite g_recording_callsite;
thread_local uint64_t          g_recording_submit_id = 0;
thread_local uint64_t          g_recording_frame     = 0;

InspectorState& State() {
	static InspectorState state;
	return state;
}

uint64_t OperationHash(const InspectorOperation& operation) {
	return operation.stages.empty() ? 0 : operation.stages.back().shader_hash;
}

size_t OperationResourceCount(const InspectorOperation& operation) {
	size_t count = operation.attachments.size();
	for (const auto& stage: operation.stages) {
		count += stage.resources.size();
	}
	return count;
}

void FinalizeOccurrenceCounts(InspectorFrame& frame) {
	std::unordered_map<uint64_t, uint32_t> totals;
	for (const auto& operation: frame.operations) {
		if (operation.kind == InspectorOperationKind::Dispatch) {
			totals[OperationHash(operation)]++;
		}
	}
	for (auto& operation: frame.operations) {
		if (operation.kind == InspectorOperationKind::Dispatch) {
			operation.occurrence_count = totals[OperationHash(operation)];
		}
	}
}

bool MatchesHash(const InspectorOperation& operation, uint64_t filter) {
	return filter == 0 || std::ranges::any_of(operation.stages, [filter](const auto& stage) {
		       return stage.shader_hash == filter;
	       });
}

uint64_t ImageKey(uint32_t image_id, uint32_t generation) {
	return (static_cast<uint64_t>(image_id) << 32) | generation;
}

uint64_t RangeEnd(uint64_t address, uint64_t size) {
	const auto length = std::max<uint64_t>(size, 1);
	return length > std::numeric_limits<uint64_t>::max() - address
	           ? std::numeric_limits<uint64_t>::max()
	           : address + length;
}

nlohmann::json ResourceJson(const InspectorResource& resource) {
	return {{"kind", InspectorResourceName(resource.kind)},
	        {"index", resource.index},
	        {"address", fmt::format("0x{:016x}", resource.address)},
	        {"bytes", resource.size},
	        {"image_id", resource.image_id},
	        {"image_generation", resource.image_generation},
	        {"buffer_id", resource.buffer_id},
	        {"buffer_generation", resource.buffer_generation},
	        {"extent", {resource.width, resource.height, resource.depth}},
	        {"guest_format", resource.guest_format},
	        {"actual_vk", resource.actual_vk},
	        {"vk_handle", fmt::format("0x{:016x}", resource.vk_handle)},
	        {"last_access_tick", resource.last_access_tick},
	        {"read", resource.read},
	        {"write", resource.written},
	        {"atomic", resource.atomic},
	        {"gpu_modified", resource.gpu_modified},
	        {"cpu_dirty", resource.cpu_dirty},
	        {"buffer_modified", resource.buffer_modified},
	        {"coherency", InspectorCoherencyName(resource)}};
}

void WriteResource(std::ofstream& file, const InspectorResource& resource) {
	file << fmt::format("    {}[{}] addr=0x{:016x} bytes={} image_id={}.{} buffer_id={}.{} "
	                    "extent={}x{}x{} guest_fmt={} actual_vk={} vk=0x{:016x} tick={} "
	                    "read={} write={} atomic={} coherency={}\n",
	                    InspectorResourceName(resource.kind), resource.index, resource.address,
	                    resource.size, resource.image_id, resource.image_generation,
	                    resource.buffer_id, resource.buffer_generation, resource.width,
	                    resource.height, resource.depth, resource.guest_format, resource.actual_vk,
	                    resource.vk_handle, resource.last_access_tick, resource.read,
	                    resource.written, resource.atomic, InspectorCoherencyName(resource));
}

void AddUsePages(InspectorFrameIndex* index, uint64_t address, uint64_t size, uint32_t use_index) {
	if (address == 0) {
		return;
	}
	const auto begin = address >> kPageShift;
	const auto end   = (RangeEnd(address, size) - 1) >> kPageShift;
	for (auto page = begin; page <= end; ++page) {
		index->uses_by_page[page].push_back(use_index);
	}
}

bool UseMatchesSelected(const InspectorUse& use, const InspectorResource& selected) {
	if (selected.image_id != 0 && use.image_id != 0) {
		return selected.image_id == use.image_id &&
		       selected.image_generation == use.image_generation;
	}
	if (selected.image_id != 0 || use.image_id != 0) {
		return false;
	}
	return InspectorRangesOverlap(selected.address, selected.size, use.address, use.size);
}

void CollectMatchingUses(const InspectorFrameIndex& index, const InspectorResource& selected,
                         std::vector<uint32_t>* use_indices) {
	use_indices->clear();
	if (selected.image_id != 0) {
		const auto found = index.uses_by_image.find(ImageKey(selected.image_id, selected.image_generation));
		if (found != index.uses_by_image.end()) {
			*use_indices = found->second;
		}
		return;
	}
	if (selected.address == 0) {
		return;
	}
	const auto begin = selected.address >> kPageShift;
	const auto end   = (RangeEnd(selected.address, selected.size) - 1) >> kPageShift;
	std::unordered_set<uint32_t> seen;
	for (auto page = begin; page <= end; ++page) {
		const auto found = index.uses_by_page.find(page);
		if (found == index.uses_by_page.end()) {
			continue;
		}
		for (const auto use_index: found->second) {
			const auto& use = index.uses[use_index];
			if (UseMatchesSelected(use, selected) && seen.insert(use_index).second) {
				use_indices->push_back(use_index);
			}
		}
	}
	std::ranges::sort(*use_indices);
}

int32_t FindRelativeUse(const InspectorFrameIndex& index, const InspectorResource& selected,
                        uint32_t from_operation, bool previous, bool writer, bool reader) {
	std::vector<uint32_t> uses;
	CollectMatchingUses(index, selected, &uses);
	if (previous) {
		for (auto it = uses.rbegin(); it != uses.rend(); ++it) {
			const auto& use = index.uses[*it];
			if (use.operation >= from_operation) {
				continue;
			}
			if ((writer && use.written) || (reader && use.read && !writer)) {
				return static_cast<int32_t>(use.operation);
			}
		}
		return -1;
	}
	for (const auto use_index: uses) {
		const auto& use = index.uses[use_index];
		if (use.operation <= from_operation) {
			continue;
		}
		if ((writer && use.written) || (reader && use.read && !writer)) {
			return static_cast<int32_t>(use.operation);
		}
	}
	return -1;
}

bool CaptureHashesMatch(const InspectorOperation& operation, const InspectorCaptureArm& arm) {
	if (arm.shader_hash_count == 0) {
		return arm.mode == InspectorCaptureMode::Resource;
	}
	for (uint32_t index = 0; index < arm.shader_hash_count; ++index) {
		const auto hash = arm.shader_hashes[index];
		if (!std::ranges::any_of(operation.stages, [hash](const auto& stage) {
			    return stage.shader_hash == hash;
		    })) {
			return false;
		}
	}
	return true;
}

bool OperationUsesArmedResource(const InspectorOperation& operation, const InspectorCaptureArm& arm) {
	auto matches = [&](const InspectorResource& resource) {
		if (arm.image_id != 0) {
			return resource.image_id == arm.image_id &&
			       (arm.image_generation == 0 || resource.image_generation == arm.image_generation);
		}
		return arm.address != 0 &&
		       InspectorRangesOverlap(arm.address, 1, resource.address, resource.size);
	};
	for (const auto& stage: operation.stages) {
		if (std::ranges::any_of(stage.resources, matches)) {
			return true;
		}
	}
	return std::ranges::any_of(operation.attachments, matches);
}

std::string ShaderTag(const InspectorOperation& operation) {
	if (operation.stages.empty()) {
		return std::string {InspectorOperationKindName(operation.kind)};
	}
	const auto& stage = operation.stages.back();
	return fmt::format("{}-{:016x}", InspectorStageName(stage.stage), stage.shader_hash);
}

nlohmann::json CaptureMetadata(uint64_t frame, uint32_t operation_index,
                               const InspectorOperation& operation, const InspectorResource& resource,
                               bool input, uint32_t alias_group) {
	nlohmann::json hashes = nlohmann::json::array();
	for (const auto& stage: operation.stages) {
		hashes.push_back({{"stage", InspectorStageName(stage.stage)},
		                  {"shader_hash", fmt::format("0x{:016x}", stage.shader_hash)}});
	}
	return {{"frame", frame},
	        {"operation", operation_index},
	        {"kind", InspectorOperationKindName(operation.kind)},
	        {"direction", input ? "input" : "output"},
	        {"shader_hashes", hashes},
	        {"groups", {operation.groups[0], operation.groups[1], operation.groups[2]}},
	        {"local", {operation.local[0], operation.local[1], operation.local[2]}},
	        {"index_count", operation.index_count},
	        {"instance_count", operation.instance_count},
	        {"submit_id", operation.submit_id},
	        {"submission_tick", operation.submission_tick},
	        {"callsite",
	         {{"rip", fmt::format("0x{:016x}", operation.callsite.guest_rip)},
	          {"module", operation.callsite.module_name},
	          {"module_base", fmt::format("0x{:016x}", operation.callsite.module_base)},
	          {"module_offset", fmt::format("0x{:x}", operation.callsite.module_offset)}}},
	        {"resource", ResourceJson(resource)},
	        {"alias_group", alias_group},
	        {"access", resource.written && resource.read ? "READ_WRITE"
	                   : resource.written                ? "WRITE"
	                   : resource.read                   ? "READ"
	                                                     : "NONE"},
	        {"capture_sync",
	         "inserts a GPU copy into the current command buffer and restores image layout; "
	         "does not call Finish/wait on the host. Output capture ends the current render pass "
	         "(same as DumpShaderInput)."}};
}

void CaptureOneResource(CommandBuffer& command, RenderContext& renderer, uint64_t frame,
                        uint32_t operation_index, const InspectorOperation& operation,
                        const InspectorResource& resource, bool input, const char* slot_prefix) {
	const auto stem = fmt::format(
	    "D:/PS5/dumps/frame-{}_op-{}_{}_{}-slot{}_addr-{:x}_img-{}", frame, operation_index,
	    ShaderTag(operation), input ? "input" : "output", resource.index, resource.address,
	    resource.image_id);
	InspectorCaptureRecord record;
	record.stem             = stem;
	record.bin_path         = stem + ".bin";
	record.json_path        = stem + ".json";
	record.frame            = frame;
	record.operation        = operation_index;
	record.image_id         = resource.image_id;
	record.image_generation = resource.image_generation;
	record.address          = resource.address;
	record.input            = input;
	record.note             = slot_prefix;
	const auto meta = CaptureMetadata(frame, operation_index, operation, resource, input, 0).dump();
	if (resource.kind != InspectorResourceKind::Buffer && resource.image_id != 0) {
		auto& cache = renderer.GetTextureCache();
		auto& image = cache.GetImage({resource.image_id, resource.image_generation});
		DumpInspectorGpuImage(command, renderer, image, record, meta);
		return;
	}
	if (resource.vk_handle != 0 && resource.size != 0) {
		DumpInspectorGpuBuffer(command, renderer, vk::Buffer {reinterpret_cast<VkBuffer>(resource.vk_handle)},
		                       0, resource.size, record, meta);
		return;
	}
	std::error_code ec;
	std::filesystem::create_directories("D:/PS5/dumps", ec);
	nlohmann::json json = nlohmann::json::parse(meta, nullptr, false);
	if (json.is_discarded()) {
		json = nlohmann::json::object();
	}
	json["status"] = "skipped_no_host_handle";
	std::ofstream file {record.json_path, std::ios::trunc};
	file << json.dump(2) << '\n';
	record.note = "skipped_no_host_handle";
	NoteInspectorCaptureComplete(record);
}

} // namespace

bool DispatchInspectorEnabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DEBUG_PANEL");
		return value != nullptr && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
	}();
	return enabled;
}

bool InspectorRangesOverlap(uint64_t first_address, uint64_t first_size, uint64_t second_address,
                            uint64_t second_size) noexcept {
	if (first_address == 0 || second_address == 0) {
		return false;
	}
	return first_address < RangeEnd(second_address, second_size) &&
	       second_address < RangeEnd(first_address, first_size);
}

void FillInspectorImageResource(InspectorResource& resource, TextureCache& texture_cache) {
	if (resource.image_id == 0) {
		return;
	}
	auto& image              = texture_cache.GetImage({resource.image_id, resource.image_generation});
	resource.guest_format    = static_cast<int32_t>(image.info.guest_format);
	resource.vk_handle       = static_cast<uint64_t>(
        reinterpret_cast<uintptr_t>(static_cast<VkImage>(image.backing.image)));
	resource.last_access_tick = image.tick_accessed_last;
	resource.gpu_modified     = image.IsGpuModified();
	resource.cpu_dirty        = image.IsCpuDirty();
	resource.buffer_modified  = image.IsBufferModified();
	resource.has_coherency    = true;
	if (resource.width == 0) {
		resource.width  = image.info.extent.width;
		resource.height = image.info.extent.height;
		resource.depth  = image.info.extent.depth;
	}
}

InspectorStage CaptureInspectorStage(const PreparedBindings& bindings,
                                     TextureCache&           texture_cache) {
	InspectorStage result;
	if (bindings.program == nullptr) {
		return result;
	}
	const auto& program = *bindings.program;
	result.stage        = static_cast<uint32_t>(program.stage);
	result.shader_hash  = program.shader_hash;
	result.resources.reserve(bindings.buffer_sources.size() + bindings.images.size());
	for (uint32_t index = 0; index < bindings.buffer_sources.size(); ++index) {
		const auto& source   = bindings.buffer_sources[index];
		const auto& resource = program.info.buffers[index];
		InspectorResource captured {.kind              = InspectorResourceKind::Buffer,
		                            .index             = index,
		                            .address           = source.address,
		                            .size              = source.size,
		                            .buffer_id         = source.id.index,
		                            .buffer_generation = source.id.generation,
		                            .read              = resource.read,
		                            .written           = resource.written,
		                            .atomic            = resource.atomic};
		if (index < bindings.buffers.size() && bindings.buffers[index].buffer) {
			captured.vk_handle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
			    static_cast<VkBuffer>(bindings.buffers[index].buffer)));
		}
		result.resources.push_back(captured);
	}
	for (uint32_t index = 0; index < bindings.images.size(); ++index) {
		const auto&       binding  = bindings.images[index];
		const auto&       resource = program.info.images[index];
		InspectorResource captured {.kind             = InspectorResourceKind::Image,
		                            .index            = index,
		                            .address          = binding.desc.info.data.address,
		                            .size             = binding.desc.info.data.size,
		                            .image_id         = binding.image_id.index,
		                            .image_generation = binding.image_id.generation,
		                            .width            = binding.desc.info.extent.width,
		                            .height           = binding.desc.info.extent.height,
		                            .depth            = binding.desc.info.extent.depth,
		                            .guest_format     = static_cast<int32_t>(binding.desc.info.guest_format),
		                            .read             = resource.read,
		                            .written          = resource.written,
		                            .atomic           = resource.atomic};
		if (binding.image_id) {
			auto&      image = texture_cache.GetImage(binding.image_id);
			const auto actual =
			    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
			captured.actual_vk =
			    actual == image.views.end() ? -1 : static_cast<int32_t>(actual->info.format);
			captured.vk_handle = static_cast<uint64_t>(
			    reinterpret_cast<uintptr_t>(static_cast<VkImage>(image.backing.image)));
			captured.last_access_tick = image.tick_accessed_last;
			captured.gpu_modified     = image.IsGpuModified();
			captured.cpu_dirty        = image.IsCpuDirty();
			captured.buffer_modified  = image.IsBufferModified();
			captured.has_coherency    = true;
		}
		result.resources.push_back(captured);
	}
	return result;
}

void RecordInspectorOperation(uint64_t frame, InspectorOperation operation) {
	if (!DispatchInspectorEnabled()) {
		return;
	}
	if (operation.callsite.guest_rip == 0) {
		operation.callsite = g_recording_callsite;
	}
	if (operation.submit_id == 0) {
		operation.submit_id = g_recording_submit_id;
	}
	auto&            state = State();
	std::scoped_lock lock(state.mutex);
	if (state.current.frame != frame) {
		if (!state.current.operations.empty()) {
			FinalizeOccurrenceCounts(state.current);
			state.complete = std::move(state.current);
		}
		state.current       = {};
		state.current.frame = frame;
		state.dispatch_counts.clear();
		state.resource_count = 0;
	}
	if (state.current.operations.size() >= kMaxOperations) {
		state.current.dropped_operations++;
		return;
	}
	const auto resources = OperationResourceCount(operation);
	if (state.resource_count + resources > kMaxResources) {
		state.current.dropped_resources += static_cast<uint32_t>(resources);
		return;
	}
	if (operation.kind == InspectorOperationKind::Dispatch) {
		operation.occurrence = state.dispatch_counts[OperationHash(operation)]++;
	}
	state.resource_count += resources;
	state.current.operations.push_back(std::move(operation));
}

uint32_t PeekInspectorOperationIndex(uint64_t frame) {
	if (!DispatchInspectorEnabled()) {
		return 0;
	}
	auto&            state = State();
	std::scoped_lock lock(state.mutex);
	if (state.current.frame != frame) {
		return 0;
	}
	return static_cast<uint32_t>(state.current.operations.size());
}

bool GetDispatchInspectorFrame(InspectorFrame* frame) {
	if (!DispatchInspectorEnabled() || frame == nullptr) {
		return false;
	}
	auto&            state = State();
	std::scoped_lock lock(state.mutex);
	if (!state.current.operations.empty()) {
		*frame = state.current;
		FinalizeOccurrenceCounts(*frame);
		return true;
	}
	if (!state.complete.operations.empty()) {
		*frame = state.complete;
		return true;
	}
	return false;
}

InspectorDumpResult DumpDispatchInspectorFrame(const InspectorFrame& frame, uint64_t shader_filter,
                                               int32_t operation_index) {
	InspectorDumpResult result;
	if (frame.operations.empty()) {
		result.error = "no captured operations";
		return result;
	}
	if (operation_index >= static_cast<int32_t>(frame.operations.size())) {
		result.error = "selected operation is outside the captured frame";
		return result;
	}
	std::string suffix;
	if (operation_index >= 0) {
		suffix = fmt::format("-op{}", operation_index);
	} else if (shader_filter != 0) {
		suffix = fmt::format("-{:016x}", shader_filter);
	}
	const auto stem = fmt::format("D:/PS5/dumps/dispatch-inspector-f{}{}", frame.frame, suffix);
	result.text_path = stem + ".txt";
	result.json_path = stem + ".json";
	std::ofstream text_file {result.text_path, std::ios::trunc};
	std::ofstream json_file {result.json_path, std::ios::trunc};
	if (!text_file || !json_file) {
		result.error = "could not create inspector dump files";
		return result;
	}

	nlohmann::json root {{"frame", frame.frame},
	                     {"operation_count", frame.operations.size()},
	                     {"dropped_operations", frame.dropped_operations},
	                     {"dropped_resources", frame.dropped_resources},
	                     {"shader_filter", fmt::format("0x{:016x}", shader_filter)},
	                     {"selected_operation", operation_index},
	                     {"operations", nlohmann::json::array()},
	                     {"image_aliases", nlohmann::json::array()}};
	std::unordered_map<uint64_t, std::unordered_set<uint64_t>> image_ids_by_address;
	auto note_alias = [&](const InspectorResource& resource) {
		if (resource.address != 0 && resource.image_id != 0) {
			image_ids_by_address[resource.address].insert(
			    ImageKey(resource.image_id, resource.image_generation));
		}
	};
	text_file << fmt::format("frame={} operations={} dropped_operations={} dropped_resources={} "
	                         "filter=0x{:016x} selected_operation={}\n",
	                         frame.frame, frame.operations.size(), frame.dropped_operations,
	                         frame.dropped_resources, shader_filter, operation_index);
	for (uint32_t index = 0; index < frame.operations.size(); ++index) {
		const auto& operation = frame.operations[index];
		if ((operation_index >= 0 && index != static_cast<uint32_t>(operation_index)) ||
		    !MatchesHash(operation, shader_filter)) {
			continue;
		}
		nlohmann::json operation_json {
		    {"index", index},
		    {"kind", InspectorOperationKindName(operation.kind)},
		    {"occurrence", operation.occurrence + 1},
		    {"occurrence_count", operation.occurrence_count},
		    {"groups", {operation.groups[0], operation.groups[1], operation.groups[2]}},
		    {"local", {operation.local[0], operation.local[1], operation.local[2]}},
		    {"indirect_address", fmt::format("0x{:016x}", operation.indirect_address)},
		    {"index_count", operation.index_count},
		    {"instance_count", operation.instance_count},
		    {"submit_id", operation.submit_id},
		    {"submission_tick", operation.submission_tick},
		    {"callsite", InspectorCallsiteText(operation.callsite)},
		    {"guest_rip", fmt::format("0x{:016x}", operation.callsite.guest_rip)},
		    {"stages", nlohmann::json::array()},
		    {"attachments", nlohmann::json::array()},
		};
		if (operation.has_viewport) {
			operation_json["viewport"] = {operation.viewport[0], operation.viewport[1],
			                              operation.viewport[2], operation.viewport[3]};
			operation_json["scissor"]  = {operation.scissor[0], operation.scissor[1],
			                              operation.scissor[2], operation.scissor[3]};
		}
		if (operation.kind == InspectorOperationKind::Dispatch) {
			text_file << fmt::format("[{}] DISPATCH pass={}/{} groups={}x{}x{} local={}x{}x{} "
			                         "indirect=0x{:016x} submit={} tick={} {}\n",
			                         index, operation.occurrence + 1, operation.occurrence_count,
			                         operation.groups[0], operation.groups[1], operation.groups[2],
			                         operation.local[0], operation.local[1], operation.local[2],
			                         operation.indirect_address, operation.submit_id,
			                         operation.submission_tick, InspectorCallsiteText(operation.callsite));
		} else if (operation.kind == InspectorOperationKind::Draw) {
			text_file << fmt::format("[{}] DRAW indices={} instances={} submit={} tick={} {}\n",
			                         index, operation.index_count, operation.instance_count,
			                         operation.submit_id, operation.submission_tick,
			                         InspectorCallsiteText(operation.callsite));
		} else {
			text_file << fmt::format("[{}] {} submit={} tick={} {}\n", index,
			                         InspectorOperationKindName(operation.kind), operation.submit_id,
			                         operation.submission_tick,
			                         InspectorCallsiteText(operation.callsite));
		}
		for (const auto& stage: operation.stages) {
			text_file << fmt::format("  {} hash=0x{:016x}\n", InspectorStageName(stage.stage),
			                         stage.shader_hash);
			nlohmann::json stage_json {{"stage", InspectorStageName(stage.stage)},
			                           {"shader_hash", fmt::format("0x{:016x}", stage.shader_hash)},
			                           {"resources", nlohmann::json::array()}};
			for (const auto& resource: stage.resources) {
				WriteResource(text_file, resource);
				stage_json["resources"].push_back(ResourceJson(resource));
				note_alias(resource);
			}
			operation_json["stages"].push_back(std::move(stage_json));
		}
		for (const auto& resource: operation.attachments) {
			WriteResource(text_file, resource);
			operation_json["attachments"].push_back(ResourceJson(resource));
			note_alias(resource);
		}
		root["operations"].push_back(std::move(operation_json));
	}
	root["matched_operation_count"] = root["operations"].size();
	for (const auto& [address, identities]: image_ids_by_address) {
		if (identities.size() < 2) continue;
		nlohmann::json alias {{"address", fmt::format("0x{:016x}", address)},
		                      {"image_ids", nlohmann::json::array()}};
		text_file << fmt::format("ALIAS addr=0x{:016x} image_ids=", address);
		bool first = true;
		for (const auto identity: identities) {
			const auto image_id = static_cast<uint32_t>(identity >> 32);
			const auto generation = static_cast<uint32_t>(identity);
			if (!first) text_file << ',';
			text_file << fmt::format("{}.{}", image_id, generation);
			alias["image_ids"].push_back(
			    {{"id", image_id}, {"generation", generation}});
			first = false;
		}
		text_file << '\n';
		root["image_aliases"].push_back(std::move(alias));
	}
	root["image_alias_count"] = root["image_aliases"].size();
	json_file << root.dump(2) << '\n';
	text_file.close();
	json_file.close();
	result.success = text_file.good() && json_file.good();
	if (!result.success) {
		result.error = "inspector dump did not flush completely";
	}
	return result;
}

void NoteInspectorShaderCompile(uint64_t shader_hash, uint32_t probe_generation) {
	if (!DispatchInspectorEnabled()) {
		return;
	}
	auto& state = State();
	std::scoped_lock lock(state.mutex);
	state.shader_probe_generations[shader_hash] = probe_generation;
}

uint32_t InspectorShaderProbeGeneration(uint64_t shader_hash) {
	if (!DispatchInspectorEnabled()) {
		return 0;
	}
	auto& state = State();
	std::scoped_lock lock(state.mutex);
	const auto found = state.shader_probe_generations.find(shader_hash);
	return found == state.shader_probe_generations.end() ? 0 : found->second;
}

void NoteInspectorSubmitCallsite(uint64_t guest_rip) {
	if (!DispatchInspectorEnabled() || guest_rip == 0) {
		return;
	}
	InspectorCallsite callsite;
	callsite.guest_rip = guest_rip;
	auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
	if (auto* program = linker->FindProgramByAddr(guest_rip)) {
		callsite.module_base   = program->base_vaddr;
		callsite.module_offset = guest_rip - program->base_vaddr;
		const auto name        = program->file_name.filename().string();
		std::snprintf(callsite.module_name, sizeof(callsite.module_name), "%s", name.c_str());
	}
	g_submit_callsite = callsite;
}

void SetInspectorRecordingContext(const InspectorCallsite& callsite, uint64_t submit_id,
                                  uint64_t frame) {
	g_recording_callsite  = callsite;
	g_recording_submit_id = submit_id;
	g_recording_frame     = frame;
}

InspectorCallsite PeekInspectorSubmitCallsite() {
	return g_submit_callsite;
}

InspectorCallsite GetInspectorRecordingCallsite() {
	return g_recording_callsite;
}

uint64_t GetInspectorRecordingSubmitId() {
	return g_recording_submit_id;
}

uint64_t GetInspectorRecordingFrame() {
	return g_recording_frame;
}

void BuildInspectorIndex(const InspectorFrame& frame, InspectorFrameIndex* index) {
	*index        = {};
	index->frame  = frame.frame;
	index->use_begin.resize(frame.operations.size() + 1);
	index->uses.reserve(frame.operations.size() * 4);
	struct Seed {
		uint64_t address = 0;
		uint64_t size    = 0;
		uint32_t image_id = 0;
		uint32_t generation = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t depth = 0;
		int32_t  guest_format = -1;
		int32_t  actual_vk = -1;
		uint64_t vk_handle = 0;
		uint32_t first_use = UINT32_MAX;
		uint32_t last_use = UINT32_MAX;
		uint32_t last_writer = UINT32_MAX;
		uint64_t last_access_tick = 0;
		bool     read = false;
		bool     written = false;
	};
	std::vector<Seed> seeds;
	std::unordered_map<uint64_t, uint32_t> seed_by_image;
	auto add_resource = [&](uint32_t operation_index, uint32_t stage, const InspectorResource& resource,
	                        bool attachment) {
		InspectorUse use;
		use.operation        = operation_index;
		use.stage            = stage;
		use.kind             = resource.kind;
		use.slot             = resource.index;
		use.address          = resource.address;
		use.size             = resource.size;
		use.image_id         = resource.image_id;
		use.image_generation = resource.image_generation;
		use.last_access_tick = resource.last_access_tick;
		use.read             = resource.read;
		use.written          = resource.written;
		use.atomic           = resource.atomic;
		use.attachment       = attachment;
		const auto use_index = static_cast<uint32_t>(index->uses.size());
		index->uses.push_back(use);
		AddUsePages(index, resource.address, resource.size, use_index);
		if (resource.image_id != 0) {
			index->uses_by_image[ImageKey(resource.image_id, resource.image_generation)].push_back(
			    use_index);
			const auto key = ImageKey(resource.image_id, resource.image_generation);
			auto found = seed_by_image.find(key);
			if (found == seed_by_image.end()) {
				found = seed_by_image.emplace(key, static_cast<uint32_t>(seeds.size())).first;
				seeds.push_back({.address = resource.address,
				                 .size = resource.size,
				                 .image_id = resource.image_id,
				                 .generation = resource.image_generation,
				                 .width = resource.width,
				                 .height = resource.height,
				                 .depth = resource.depth,
				                 .guest_format = resource.guest_format,
				                 .actual_vk = resource.actual_vk,
				                 .vk_handle = resource.vk_handle,
				                 .first_use = operation_index,
				                 .last_use = operation_index,
				                 .last_writer = resource.written ? operation_index : UINT32_MAX,
				                 .last_access_tick = resource.last_access_tick,
				                 .read = resource.read,
				                 .written = resource.written});
			} else {
				auto& seed = seeds[found->second];
				seed.last_use = operation_index;
				if (resource.written) {
					seed.last_writer = operation_index;
				}
				seed.read |= resource.read;
				seed.written |= resource.written;
				seed.last_access_tick = std::max(seed.last_access_tick, resource.last_access_tick);
			}
		}
	};
	for (uint32_t operation_index = 0; operation_index < frame.operations.size(); ++operation_index) {
		index->use_begin[operation_index] = static_cast<uint32_t>(index->uses.size());
		const auto& operation = frame.operations[operation_index];
		for (const auto& stage: operation.stages) {
			index->ops_by_shader[stage.shader_hash].push_back(operation_index);
			for (const auto& resource: stage.resources) {
				add_resource(operation_index, stage.stage, resource, false);
			}
		}
		for (const auto& resource: operation.attachments) {
			add_resource(operation_index, 0, resource, true);
		}
	}
	index->use_begin.back() = static_cast<uint32_t>(index->uses.size());

	std::vector<uint32_t> parent(seeds.size());
	for (uint32_t i = 0; i < parent.size(); ++i) {
		parent[i] = i;
	}
	auto find = [&](auto& self, uint32_t value) -> uint32_t {
		if (parent[value] != value) {
			parent[value] = self(self, parent[value]);
		}
		return parent[value];
	};
	for (uint32_t i = 0; i < seeds.size(); ++i) {
		for (uint32_t j = i + 1; j < seeds.size(); ++j) {
			if (InspectorRangesOverlap(seeds[i].address, seeds[i].size, seeds[j].address,
			                           seeds[j].size)) {
				parent[find(find, i)] = find(find, j);
			}
		}
	}
	std::unordered_map<uint32_t, std::vector<uint32_t>> grouped;
	for (uint32_t i = 0; i < seeds.size(); ++i) {
		grouped[find(find, i)].push_back(i);
	}
	uint32_t group_id = 1;
	std::unordered_map<uint64_t, uint32_t> group_by_image;
	for (auto& [root, members]: grouped) {
		std::unordered_set<uint64_t> identities;
		std::unordered_set<uint64_t> ranges;
		for (const auto member: members) {
			identities.insert(ImageKey(seeds[member].image_id, seeds[member].generation));
			ranges.insert((seeds[member].address << 12) ^ seeds[member].size);
		}
		if (identities.size() < 2) {
			continue;
		}
		InspectorAliasGroup group;
		group.id = group_id++;
		group.min_address = UINT64_MAX;
		group.overlapping_non_identical = ranges.size() > 1;
		for (const auto member: members) {
			const auto& seed = seeds[member];
			group.min_address = std::min(group.min_address, seed.address);
			group.max_end     = std::max(group.max_end, RangeEnd(seed.address, seed.size));
			group.representations.push_back({.image_id = seed.image_id,
			                                 .image_generation = seed.generation,
			                                 .address = seed.address,
			                                 .size = seed.size,
			                                 .width = seed.width,
			                                 .height = seed.height,
			                                 .depth = seed.depth,
			                                 .guest_format = seed.guest_format,
			                                 .actual_vk = seed.actual_vk,
			                                 .vk_handle = seed.vk_handle,
			                                 .last_writer_op = seed.last_writer,
			                                 .last_use_op = seed.last_use,
			                                 .first_use_op = seed.first_use,
			                                 .last_access_tick = seed.last_access_tick,
			                                 .read = seed.read,
			                                 .written = seed.written});
			group_by_image[ImageKey(seed.image_id, seed.generation)] = group.id;
		}
		index->alias_groups.push_back(std::move(group));
	}
	for (auto& use: index->uses) {
		if (use.image_id == 0) {
			continue;
		}
		const auto found = group_by_image.find(ImageKey(use.image_id, use.image_generation));
		if (found != group_by_image.end()) {
			use.alias_group = found->second;
			use.alias       = true;
		}
	}
}

void InspectorOpsForGuestAddress(const InspectorFrameIndex& index, uint64_t address, uint64_t size,
                                 std::vector<uint32_t>* operations) {
	if (operations != nullptr) {
		operations->clear();
	}
	if (address == 0) {
		return;
	}
	const auto begin = address >> kPageShift;
	const auto end   = (RangeEnd(address, size) - 1) >> kPageShift;
	std::unordered_set<uint32_t> seen;
	for (auto page = begin; page <= end; ++page) {
		const auto found = index.uses_by_page.find(page);
		if (found == index.uses_by_page.end()) {
			continue;
		}
		for (const auto use_index: found->second) {
			const auto& use = index.uses[use_index];
			if (!InspectorRangesOverlap(address, size, use.address, use.size)) {
				continue;
			}
			if (operations != nullptr && seen.insert(use.operation).second) {
				operations->push_back(use.operation);
			}
		}
	}
	if (operations != nullptr) {
		std::ranges::sort(*operations);
	}
}

InspectorMatchInfo InspectorMatchForOperation(const InspectorFrameIndex& index,
                                              const InspectorFrame& frame, uint32_t operation,
                                              uint64_t address, uint64_t size) {
	InspectorMatchInfo info;
	if (operation >= frame.operations.size() || operation + 1 >= index.use_begin.size()) {
		return info;
	}
	bool read = false;
	bool written = false;
	for (auto use_index = index.use_begin[operation]; use_index < index.use_begin[operation + 1];
	     ++use_index) {
		const auto& use = index.uses[use_index];
		if (!InspectorRangesOverlap(address, size, use.address, use.size)) {
			continue;
		}
		read |= use.read;
		written |= use.written;
		info.alias |= use.alias;
		if (info.guest_base == 0) {
			info.guest_base        = use.address;
			info.offset            = address >= use.address ? address - use.address : 0;
			info.size              = use.size;
			info.resource_kind     = use.kind;
			info.image_id          = use.image_id;
			info.image_generation  = use.image_generation;
			info.slot              = use.slot;
		}
	}
	const auto& op = frame.operations[operation];
	if (op.kind == InspectorOperationKind::Copy || op.kind == InspectorOperationKind::Resolve) {
		info.kind = InspectorMatchKind::ResolveCopy;
	} else if (read && written) {
		info.kind = InspectorMatchKind::ReadWrite;
	} else if (written) {
		info.kind = InspectorMatchKind::Write;
	} else if (read) {
		info.kind = InspectorMatchKind::Read;
	} else if (info.guest_base != 0) {
		info.kind = info.alias ? InspectorMatchKind::Read : InspectorMatchKind::None;
	}
	return info;
}

int32_t FindInspectorPreviousWriter(const InspectorFrameIndex& index,
                                    const InspectorResource& selected, uint32_t from_operation) {
	return FindRelativeUse(index, selected, from_operation, true, true, false);
}

int32_t FindInspectorNextReader(const InspectorFrameIndex& index, const InspectorResource& selected,
                                uint32_t from_operation) {
	return FindRelativeUse(index, selected, from_operation, false, false, true);
}

int32_t FindInspectorNextWriter(const InspectorFrameIndex& index, const InspectorResource& selected,
                                uint32_t from_operation) {
	return FindRelativeUse(index, selected, from_operation, false, true, false);
}

void CollectInspectorResourceTimeline(const InspectorFrameIndex& index,
                                      const InspectorResource& selected,
                                      std::vector<uint32_t>* use_indices) {
	CollectMatchingUses(index, selected, use_indices);
}

uint32_t InspectorAliasGroupForResource(const InspectorFrameIndex& index,
                                        const InspectorResource& resource) {
	if (resource.image_id == 0) {
		return 0;
	}
	const auto found = index.uses_by_image.find(ImageKey(resource.image_id, resource.image_generation));
	if (found == index.uses_by_image.end() || found->second.empty()) {
		return 0;
	}
	return index.uses[found->second.front()].alias_group;
}

void ArmInspectorCapture(const InspectorCaptureArm& arm) {
	auto&            state = State();
	std::scoped_lock lock(state.mutex);
	state.capture_arm = arm;
}

InspectorCaptureArm PeekInspectorCaptureArm() {
	auto&            state = State();
	std::scoped_lock lock(state.mutex);
	return state.capture_arm;
}

bool InspectorShouldCapture(const InspectorOperation& operation, uint32_t index,
                            bool* capture_inputs, bool* capture_outputs) {
	if (capture_inputs != nullptr) {
		*capture_inputs = false;
	}
	if (capture_outputs != nullptr) {
		*capture_outputs = false;
	}
	if (!DispatchInspectorEnabled()) {
		return false;
	}
	auto&            state = State();
	std::scoped_lock lock(state.mutex);
	const auto&      arm   = state.capture_arm;
	if (arm.mode == InspectorCaptureMode::None) {
		return false;
	}
	bool matches = false;
	if (arm.mode == InspectorCaptureMode::Resource) {
		matches = OperationUsesArmedResource(operation, arm);
	} else if (operation.kind == arm.kind && CaptureHashesMatch(operation, arm)) {
		matches = operation.kind != InspectorOperationKind::Dispatch ||
		          operation.occurrence == arm.occurrence || arm.expected_index == static_cast<int32_t>(index);
		if (arm.expected_index >= 0 && static_cast<int32_t>(index) == arm.expected_index &&
		    CaptureHashesMatch(operation, arm)) {
			matches = true;
		}
	}
	if (!matches) {
		return false;
	}
	const bool inputs  = arm.mode == InspectorCaptureMode::Inputs ||
	                    arm.mode == InspectorCaptureMode::BeforeAfter ||
	                    arm.mode == InspectorCaptureMode::Resource;
	const bool outputs = arm.mode == InspectorCaptureMode::Outputs ||
	                     arm.mode == InspectorCaptureMode::BeforeAfter ||
	                     arm.mode == InspectorCaptureMode::Resource;
	if (capture_inputs != nullptr) {
		*capture_inputs = inputs;
	}
	if (capture_outputs != nullptr) {
		*capture_outputs = outputs;
	}
	return true;
}

void InspectorCompleteCapture() {
	auto&            state = State();
	std::scoped_lock lock(state.mutex);
	state.capture_arm = {};
}

void CaptureInspectorResources(CommandBuffer& command, RenderContext& renderer,
                               const InspectorOperation& operation, uint32_t operation_index,
                               uint64_t frame, bool capture_inputs, bool capture_outputs) {
	if (!capture_inputs && !capture_outputs) {
		return;
	}
	const auto arm = PeekInspectorCaptureArm();
	auto want = [&](const InspectorResource& resource, bool input) {
		if (arm.mode == InspectorCaptureMode::Resource) {
			if (arm.image_id != 0) {
				return resource.image_id == arm.image_id &&
				       (arm.image_generation == 0 ||
				        resource.image_generation == arm.image_generation);
			}
			return InspectorRangesOverlap(arm.address, 1, resource.address, resource.size);
		}
		return input ? resource.read : resource.written;
	};
	auto capture = [&](const InspectorResource& resource, bool input, const char* prefix) {
		if (!want(resource, input)) {
			return;
		}
		CaptureOneResource(command, renderer, frame, operation_index, operation, resource, input,
		                   prefix);
	};
	if (capture_inputs) {
		for (const auto& stage: operation.stages) {
			for (const auto& resource: stage.resources) {
				capture(resource, true, InspectorStageName(stage.stage));
			}
		}
		for (const auto& resource: operation.attachments) {
			if (resource.read) {
				capture(resource, true, "RT");
			}
		}
	}
	if (capture_outputs) {
		for (const auto& stage: operation.stages) {
			for (const auto& resource: stage.resources) {
				capture(resource, false, InspectorStageName(stage.stage));
			}
		}
		for (const auto& resource: operation.attachments) {
			if (resource.written) {
				capture(resource, false, "RT");
			}
		}
	}
}

void NoteInspectorCaptureComplete(const InspectorCaptureRecord& record) {
	auto&            state = State();
	std::scoped_lock lock(state.mutex);
	if (state.captures.size() >= 64) {
		state.captures.erase(state.captures.begin());
	}
	state.captures.push_back(record);
	LOGF("DispatchInspector: capture %s frame=%" PRIu64 " op=%u xxh3=0x%016" PRIx64 " %s\n",
	     record.stem.c_str(), record.frame, record.operation, record.xxh3, record.note.c_str());
}

std::vector<InspectorCaptureRecord> GetInspectorCaptureRecords() {
	auto&            state = State();
	std::scoped_lock lock(state.mutex);
	return state.captures;
}

void RefreshDispatchInspector() {
	if (!DispatchInspectorEnabled()) {
		return;
	}
	std::error_code error;
	if (!std::filesystem::exists(kDumpTrigger, error)) {
		return;
	}
	std::string filter_text;
	{
		std::ifstream trigger {kDumpTrigger};
		std::getline(trigger, filter_text, '\0');
	}
	std::filesystem::remove(kDumpTrigger, error);
	const auto     begin  = filter_text.find_first_not_of(" \t\r\n\xef\xbb\xbf");
	const auto*    text   = begin == std::string::npos ? "" : filter_text.c_str() + begin;
	const uint64_t filter = std::strtoull(text, nullptr, 16);
	InspectorFrame frame;
	if (!GetDispatchInspectorFrame(&frame)) {
		LOGF("DispatchInspector: dump requested before a frame was captured\n");
		return;
	}
	const auto result = DumpDispatchInspectorFrame(frame, filter);
	if (result.success) {
		LOGF("DispatchInspector: wrote %s and %s frame=%" PRIu64 " filter=0x%016" PRIx64 "\n",
		     result.text_path.c_str(), result.json_path.c_str(), frame.frame, filter);
	} else {
		LOGF("DispatchInspector: dump failed: %s\n", result.error.c_str());
	}
}

const char* InspectorStageName(uint32_t stage) noexcept {
	switch (static_cast<ShaderType>(stage)) {
		case ShaderType::Vertex: return "VS";
		case ShaderType::Pixel: return "PS";
		case ShaderType::Compute: return "CS";
		case ShaderType::Mesh: return "MS";
		case ShaderType::Fetch: return "FS";
		default: return "?S";
	}
}

const char* InspectorResourceName(InspectorResourceKind kind) noexcept {
	switch (kind) {
		case InspectorResourceKind::Buffer: return "buffer";
		case InspectorResourceKind::Image: return "image";
		case InspectorResourceKind::ColorTarget: return "color";
		case InspectorResourceKind::DepthTarget: return "depth";
	}
	return "resource";
}

const char* InspectorOperationKindName(InspectorOperationKind kind) noexcept {
	switch (kind) {
		case InspectorOperationKind::Draw: return "draw";
		case InspectorOperationKind::Dispatch: return "dispatch";
		case InspectorOperationKind::Copy: return "copy";
		case InspectorOperationKind::Resolve: return "resolve";
	}
	return "op";
}

const char* InspectorMatchKindName(InspectorMatchKind kind) noexcept {
	switch (kind) {
		case InspectorMatchKind::None: return "";
		case InspectorMatchKind::Read: return "READ";
		case InspectorMatchKind::Write: return "WRITE";
		case InspectorMatchKind::ReadWrite: return "READ_WRITE";
		case InspectorMatchKind::ResolveCopy: return "RESOLVE/COPY";
	}
	return "";
}

const char* InspectorCoherencyName(const InspectorResource& resource) noexcept {
	if (!resource.has_coherency) {
		return "unknown";
	}
	if (resource.cpu_dirty) {
		return "cpu_dirty";
	}
	if (resource.buffer_modified) {
		return "buffer_modified";
	}
	if (resource.gpu_modified) {
		return "gpu_modified";
	}
	return "uninitialized";
}

std::string InspectorCallsiteText(const InspectorCallsite& callsite) {
	if (callsite.guest_rip == 0) {
		return "unavailable";
	}
	if (callsite.module_name[0] == '\0') {
		return fmt::format("guest_rip=0x{:016x} (unresolved module)", callsite.guest_rip);
	}
	return fmt::format("{} + 0x{:X}", callsite.module_name, callsite.module_offset);
}

} // namespace Libs::Graphics
