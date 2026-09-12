#include "graphics/host_gpu/renderer/dispatchInspector.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shader.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics {

namespace {

// Loading/menu transition frames can exceed eight thousand real operations. Keep a runaway
// guard, but place it well above observed workloads so "every dispatch/draw" remains true.
constexpr size_t kMaxOperations = 32768;
constexpr size_t kMaxResources  = 524288;
constexpr auto   kDumpTrigger   = "D:/PS5/dumps/DUMP_INSPECTOR";

struct InspectorState {
	std::mutex                             mutex;
	InspectorFrame                         current;
	InspectorFrame                         complete;
	std::unordered_map<uint64_t, uint32_t> dispatch_counts;
	std::unordered_map<uint64_t, uint32_t> shader_probe_generations;
	size_t                                 resource_count = 0;
};

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

void WriteResource(std::ofstream& file, const InspectorResource& resource) {
	file << fmt::format("    {}[{}] addr=0x{:016x} bytes={} image_id={}.{} extent={}x{}x{} "
	                    "actual_vk={} read={} write={} atomic={}\n",
	                    InspectorResourceName(resource.kind), resource.index, resource.address,
	                    resource.size, resource.image_id, resource.image_generation, resource.width,
	                    resource.height, resource.depth, resource.actual_vk, resource.read,
	                    resource.written, resource.atomic);
}

nlohmann::json ResourceJson(const InspectorResource& resource) {
	return {{"kind", InspectorResourceName(resource.kind)},
	        {"index", resource.index},
	        {"address", fmt::format("0x{:016x}", resource.address)},
	        {"bytes", resource.size},
	        {"image_id", resource.image_id},
	        {"image_generation", resource.image_generation},
	        {"extent", {resource.width, resource.height, resource.depth}},
	        {"actual_vk", resource.actual_vk},
	        {"read", resource.read},
	        {"write", resource.written},
	        {"atomic", resource.atomic}};
}

} // namespace

bool DispatchInspectorEnabled() noexcept {
	static const bool enabled = [] {
		const char* value = std::getenv("KYTY_DEBUG_PANEL");
		return value != nullptr && value[0] != '\0' && !(value[0] == '0' && value[1] == '\0');
	}();
	return enabled;
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
		result.resources.push_back({.kind    = InspectorResourceKind::Buffer,
		                            .index   = index,
		                            .address = source.address,
		                            .size    = source.size,
		                            .read    = resource.read,
		                            .written = resource.written,
		                            .atomic  = resource.atomic});
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
		                            .read             = resource.read,
		                            .written          = resource.written,
		                            .atomic           = resource.atomic};
		if (binding.image_id) {
			auto&      image = texture_cache.GetImage(binding.image_id);
			const auto actual =
			    std::ranges::find(image.views, binding.image_view, &CachedImageView::view);
			captured.actual_vk =
			    actual == image.views.end() ? -1 : static_cast<int32_t>(actual->info.format);
		}
		result.resources.push_back(captured);
	}
	return result;
}

void RecordInspectorOperation(uint64_t frame, InspectorOperation operation) {
	if (!DispatchInspectorEnabled()) {
		return;
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
			    (static_cast<uint64_t>(resource.image_id) << 32) | resource.image_generation);
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
		    {"kind", operation.kind == InspectorOperationKind::Dispatch ? "dispatch" : "draw"},
		    {"occurrence", operation.occurrence + 1},
		    {"occurrence_count", operation.occurrence_count},
		    {"groups", {operation.groups[0], operation.groups[1], operation.groups[2]}},
		    {"local", {operation.local[0], operation.local[1], operation.local[2]}},
		    {"indirect_address", fmt::format("0x{:016x}", operation.indirect_address)},
		    {"index_count", operation.index_count},
		    {"instance_count", operation.instance_count},
		    {"stages", nlohmann::json::array()},
		    {"attachments", nlohmann::json::array()},
		};
		if (operation.kind == InspectorOperationKind::Dispatch) {
			text_file << fmt::format("[{}] DISPATCH pass={}/{} groups={}x{}x{} local={}x{}x{} "
			                         "indirect=0x{:016x}\n",
			                         index, operation.occurrence + 1, operation.occurrence_count,
			                         operation.groups[0], operation.groups[1], operation.groups[2],
			                         operation.local[0], operation.local[1], operation.local[2],
			                         operation.indirect_address);
		} else {
			text_file << fmt::format("[{}] DRAW indices={} instances={}\n", index,
			                         operation.index_count, operation.instance_count);
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

} // namespace Libs::Graphics
