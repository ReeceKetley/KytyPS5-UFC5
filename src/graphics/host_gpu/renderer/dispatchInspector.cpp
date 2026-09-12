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
#include <unordered_map>

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
	const auto    path = fmt::format("D:/PS5/dumps/dispatch-inspector-f{}.txt", frame.frame);
	std::ofstream file {path, std::ios::trunc};
	file << fmt::format("frame={} operations={} dropped_operations={} dropped_resources={} "
	                    "filter=0x{:016x}\n",
	                    frame.frame, frame.operations.size(), frame.dropped_operations,
	                    frame.dropped_resources, filter);
	for (uint32_t index = 0; index < frame.operations.size(); ++index) {
		const auto& operation = frame.operations[index];
		if (!MatchesHash(operation, filter)) {
			continue;
		}
		if (operation.kind == InspectorOperationKind::Dispatch) {
			file << fmt::format("[{}] DISPATCH pass={}/{} groups={}x{}x{} local={}x{}x{} "
			                    "indirect=0x{:016x}\n",
			                    index, operation.occurrence + 1, operation.occurrence_count,
			                    operation.groups[0], operation.groups[1], operation.groups[2],
			                    operation.local[0], operation.local[1], operation.local[2],
			                    operation.indirect_address);
		} else {
			file << fmt::format("[{}] DRAW indices={} instances={}\n", index, operation.index_count,
			                    operation.instance_count);
		}
		for (const auto& stage: operation.stages) {
			file << fmt::format("  {} hash=0x{:016x}\n", InspectorStageName(stage.stage),
			                    stage.shader_hash);
			for (const auto& resource: stage.resources) {
				WriteResource(file, resource);
			}
		}
		for (const auto& resource: operation.attachments) {
			WriteResource(file, resource);
		}
	}
	file.close();
	LOGF("DispatchInspector: wrote %s frame=%" PRIu64 " filter=0x%016" PRIx64 "\n", path.c_str(),
	     frame.frame, filter);
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
