#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHINSPECTOR_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHINSPECTOR_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

class CommandBuffer;
class RenderContext;
class TextureCache;
struct PreparedBindings;
class Image;

enum class InspectorOperationKind : uint8_t { Draw, Dispatch, Copy, Resolve };
enum class InspectorResourceKind : uint8_t { Buffer, Image, ColorTarget, DepthTarget };
enum class InspectorMatchKind : uint8_t { None, Read, Write, ReadWrite, ResolveCopy };
enum class InspectorCaptureMode : uint8_t { None, Inputs, Outputs, BeforeAfter, Resource };

struct InspectorCallsite {
	uint64_t guest_rip      = 0;
	uint64_t module_base    = 0;
	uint64_t module_offset  = 0;
	char     module_name[80] {};
};

struct InspectorResource {
	InspectorResourceKind kind             = InspectorResourceKind::Buffer;
	uint32_t              index            = 0;
	uint64_t              address          = 0;
	uint64_t              size             = 0;
	uint32_t              image_id         = 0;
	uint32_t              image_generation = 0;
	uint32_t              buffer_id        = 0;
	uint32_t              buffer_generation = 0;
	uint32_t              width            = 0;
	uint32_t              height           = 0;
	uint32_t              depth            = 0;
	int32_t               guest_format     = -1;
	int32_t               actual_vk        = -1;
	uint64_t              vk_handle        = 0;
	uint64_t              vk_offset        = 0;
	uint64_t              last_access_tick = 0;
	bool                  read             = false;
	bool                  written          = false;
	bool                  atomic           = false;
	bool                  gpu_modified     = false;
	bool                  cpu_dirty        = false;
	bool                  buffer_modified  = false;
	bool                  has_coherency    = false;
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
	uint64_t                       submit_id        = 0;
	uint64_t                       submission_tick  = 0;
	float                          viewport[4]      = {};
	int32_t                        scissor[4]       = {};
	bool                           has_viewport     = false;
	InspectorCallsite              callsite;
	std::vector<InspectorStage>    stages;
	std::vector<InspectorResource> attachments;
};

struct InspectorFrame {
	uint64_t                        frame = 0;
	std::vector<InspectorOperation> operations;
	uint32_t                        dropped_operations = 0;
	uint32_t                        dropped_resources  = 0;
};

struct InspectorDumpResult {
	bool        success = false;
	std::string text_path;
	std::string json_path;
	std::string error;
};

struct InspectorUse {
	uint32_t              operation        = 0;
	uint32_t              stage            = 0;
	InspectorResourceKind kind             = InspectorResourceKind::Buffer;
	uint32_t              slot             = 0;
	uint64_t              address          = 0;
	uint64_t              size             = 0;
	uint32_t              image_id         = 0;
	uint32_t              image_generation = 0;
	uint32_t              alias_group      = 0;
	uint64_t              last_access_tick = 0;
	bool                  read             = false;
	bool                  written          = false;
	bool                  atomic           = false;
	bool                  attachment       = false;
	bool                  alias            = false;
};

struct InspectorAliasRepresentation {
	uint32_t image_id          = 0;
	uint32_t image_generation  = 0;
	uint64_t address           = 0;
	uint64_t size              = 0;
	uint32_t width             = 0;
	uint32_t height            = 0;
	uint32_t depth             = 0;
	int32_t  guest_format      = -1;
	int32_t  actual_vk         = -1;
	uint64_t vk_handle         = 0;
	uint32_t last_writer_op    = UINT32_MAX;
	uint32_t last_use_op       = UINT32_MAX;
	uint32_t first_use_op      = UINT32_MAX;
	uint64_t last_access_tick  = 0;
	bool     read              = false;
	bool     written           = false;
};

struct InspectorAliasGroup {
	uint32_t                                 id = 0;
	uint64_t                                 min_address = 0;
	uint64_t                                 max_end     = 0;
	bool                                     overlapping_non_identical = false;
	std::vector<InspectorAliasRepresentation> representations;
};

struct InspectorMatchInfo {
	InspectorMatchKind kind      = InspectorMatchKind::None;
	bool               alias     = false;
	uint64_t           guest_base = 0;
	uint64_t           offset     = 0;
	uint64_t           size       = 0;
	InspectorResourceKind resource_kind = InspectorResourceKind::Buffer;
	uint32_t           image_id   = 0;
	uint32_t           image_generation = 0;
	uint32_t           slot       = 0;
};

struct InspectorCaptureArm {
	InspectorCaptureMode     mode              = InspectorCaptureMode::None;
	InspectorOperationKind   kind              = InspectorOperationKind::Draw;
	uint64_t                 shader_hashes[4]  = {};
	uint32_t                 shader_hash_count = 0;
	uint32_t                 occurrence        = 0;
	int32_t                  expected_index    = -1;
	uint32_t                 image_id          = 0;
	uint32_t                 image_generation  = 0;
	uint64_t                 address           = 0;
};

struct InspectorCaptureRecord {
	std::string stem;
	std::string bin_path;
	std::string json_path;
	uint64_t    xxh3              = 0;
	uint64_t    frame             = 0;
	uint32_t    operation         = 0;
	uint32_t    image_id          = 0;
	uint32_t    image_generation  = 0;
	uint64_t    address           = 0;
	bool        input             = false;
	bool        ready             = false;
	std::string note;
};

struct InspectorFrameIndex {
	uint64_t frame = 0;
	std::vector<InspectorUse> uses;
	std::vector<uint32_t>     use_begin;
	std::unordered_map<uint64_t, std::vector<uint32_t>> uses_by_image;
	std::unordered_map<uint64_t, std::vector<uint32_t>> ops_by_shader;
	std::unordered_map<uint64_t, std::vector<uint32_t>> uses_by_page;
	std::vector<InspectorAliasGroup> alias_groups;
};

// The whole recorder is inert unless KYTY_DEBUG_PANEL is set to a non-zero value.
[[nodiscard]] bool           DispatchInspectorEnabled() noexcept;
[[nodiscard]] InspectorStage CaptureInspectorStage(const PreparedBindings& bindings,
                                                   TextureCache&           texture_cache);
void                         FillInspectorImageResource(InspectorResource& resource,
                                                        TextureCache&      texture_cache);
void                         RecordInspectorOperation(uint64_t frame, InspectorOperation operation);
[[nodiscard]] uint32_t       PeekInspectorOperationIndex(uint64_t frame);
[[nodiscard]] bool           GetDispatchInspectorFrame(InspectorFrame* frame);
[[nodiscard]] InspectorDumpResult DumpDispatchInspectorFrame(const InspectorFrame& frame,
                                                             uint64_t shader_filter = 0,
                                                             int32_t operation_index = -1);
void NoteInspectorShaderCompile(uint64_t shader_hash, uint32_t probe_generation);
[[nodiscard]] uint32_t InspectorShaderProbeGeneration(uint64_t shader_hash);

void NoteInspectorSubmitCallsite(uint64_t guest_rip);
void SetInspectorRecordingContext(const InspectorCallsite& callsite, uint64_t submit_id,
                                  uint64_t frame);
[[nodiscard]] InspectorCallsite PeekInspectorSubmitCallsite();
[[nodiscard]] InspectorCallsite GetInspectorRecordingCallsite();
[[nodiscard]] uint64_t          GetInspectorRecordingSubmitId();
[[nodiscard]] uint64_t          GetInspectorRecordingFrame();

void BuildInspectorIndex(const InspectorFrame& frame, InspectorFrameIndex* index);
[[nodiscard]] bool InspectorRangesOverlap(uint64_t first_address, uint64_t first_size,
                                          uint64_t second_address, uint64_t second_size) noexcept;
void InspectorOpsForGuestAddress(const InspectorFrameIndex& index, uint64_t address, uint64_t size,
                                 std::vector<uint32_t>* operations);
[[nodiscard]] InspectorMatchInfo InspectorMatchForOperation(const InspectorFrameIndex& index,
                                                            const InspectorFrame&      frame,
                                                            uint32_t operation, uint64_t address,
                                                            uint64_t size);
[[nodiscard]] int32_t FindInspectorPreviousWriter(const InspectorFrameIndex& index,
                                                  const InspectorResource& selected,
                                                  uint32_t from_operation);
[[nodiscard]] int32_t FindInspectorNextReader(const InspectorFrameIndex& index,
                                              const InspectorResource& selected,
                                              uint32_t from_operation);
[[nodiscard]] int32_t FindInspectorNextWriter(const InspectorFrameIndex& index,
                                              const InspectorResource& selected,
                                              uint32_t from_operation);
void CollectInspectorResourceTimeline(const InspectorFrameIndex& index,
                                      const InspectorResource& selected,
                                      std::vector<uint32_t>* use_indices);
[[nodiscard]] uint32_t InspectorAliasGroupForResource(const InspectorFrameIndex& index,
                                                      const InspectorResource& resource);

void ArmInspectorCapture(const InspectorCaptureArm& arm);
[[nodiscard]] InspectorCaptureArm PeekInspectorCaptureArm();
[[nodiscard]] bool InspectorShouldCapture(InspectorOperation& operation, uint32_t index,
                                          bool* capture_inputs, bool* capture_outputs);
void InspectorCompleteCapture();
void CaptureInspectorResources(CommandBuffer& command, RenderContext& renderer,
                               const InspectorOperation& operation, uint32_t operation_index,
                               uint64_t frame, bool capture_inputs, bool capture_outputs);
void NoteInspectorCaptureComplete(const InspectorCaptureRecord& record);
[[nodiscard]] std::vector<InspectorCaptureRecord> GetInspectorCaptureRecords();

// Consumes D:/PS5/dumps/DUMP_INSPECTOR and writes the same snapshot shown by the panel to
// paired dispatch-inspector-f<N>.txt/.json files. Optional contents are a shader hash filter.
// CAPTURE_INSPECTOR arms a one-shot dispatch resource capture for the next frame. Its contents
// are: <shader-hash> [inputs|outputs|before-after] [zero-based-occurrence].
void RefreshDispatchInspector();

[[nodiscard]] const char* InspectorStageName(uint32_t stage) noexcept;
[[nodiscard]] const char* InspectorResourceName(InspectorResourceKind kind) noexcept;
[[nodiscard]] const char* InspectorOperationKindName(InspectorOperationKind kind) noexcept;
[[nodiscard]] const char* InspectorMatchKindName(InspectorMatchKind kind) noexcept;
[[nodiscard]] const char* InspectorCoherencyName(const InspectorResource& resource) noexcept;
[[nodiscard]] std::string InspectorCallsiteText(const InspectorCallsite& callsite);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_DISPATCHINSPECTOR_H_
