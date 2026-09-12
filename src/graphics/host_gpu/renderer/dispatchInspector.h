#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHINSPECTOR_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHINSPECTOR_H_

#include <cstdint>
#include <string>
#include <vector>

namespace Libs::Graphics {

class TextureCache;
struct PreparedBindings;

enum class InspectorOperationKind : uint8_t { Draw, Dispatch };
enum class InspectorResourceKind : uint8_t { Buffer, Image, ColorTarget, DepthTarget };

struct InspectorResource {
	InspectorResourceKind kind             = InspectorResourceKind::Buffer;
	uint32_t              index            = 0;
	uint64_t              address          = 0;
	uint64_t              size             = 0;
	uint32_t              image_id         = 0;
	uint32_t              image_generation = 0;
	uint32_t              width            = 0;
	uint32_t              height           = 0;
	uint32_t              depth            = 0;
	int32_t               actual_vk        = -1;
	bool                  read             = false;
	bool                  written          = false;
	bool                  atomic           = false;
};

struct InspectorStage {
	uint32_t                       stage       = 0;
	uint64_t                       shader_hash = 0;
	std::vector<InspectorResource> resources;
};

struct InspectorOperation {
	InspectorOperationKind         kind             = InspectorOperationKind::Draw;
	uint32_t                       occurrence       = 0;
	uint32_t                       occurrence_count = 0;
	uint32_t                       groups[3]        = {};
	uint32_t                       local[3]         = {};
	uint64_t                       indirect_address = 0;
	uint32_t                       index_count      = 0;
	uint32_t                       instance_count   = 0;
	std::vector<InspectorStage>    stages;
	std::vector<InspectorResource> attachments;
};

struct InspectorFrame {
	uint64_t                        frame = 0;
	std::vector<InspectorOperation> operations;
	uint32_t                        dropped_operations = 0;
	uint32_t                        dropped_resources  = 0;
};

// The whole recorder is inert unless KYTY_DEBUG_PANEL is set to a non-zero value.
[[nodiscard]] bool           DispatchInspectorEnabled() noexcept;
[[nodiscard]] InspectorStage CaptureInspectorStage(const PreparedBindings& bindings,
                                                   TextureCache&           texture_cache);
void                         RecordInspectorOperation(uint64_t frame, InspectorOperation operation);
[[nodiscard]] bool           GetDispatchInspectorFrame(InspectorFrame* frame);

// Consumes D:/PS5/dumps/DUMP_INSPECTOR and writes the same snapshot shown by the panel to
// dispatch-inspector-f<N>.txt. The optional file contents are a shader hash filter.
void RefreshDispatchInspector();

[[nodiscard]] const char* InspectorStageName(uint32_t stage) noexcept;
[[nodiscard]] const char* InspectorResourceName(InspectorResourceKind kind) noexcept;

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHINSPECTOR_H_
