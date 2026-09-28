#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEMOVERLAY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEMOVERLAY_H_

#include "common/common.h"
#include "graphics/host_gpu/vulkanCommon.h"

#include <memory>

union SDL_Event;

namespace Libs::Graphics {

struct GraphicContext;

struct SystemOverlayVisualState {
	bool     active;
	uint64_t revision;
};

void                     InitializeSystemOverlayInput();
void                     ShutdownSystemOverlayInput();
bool                     ProcessSystemOverlayInput(const SDL_Event& event);
SystemOverlayVisualState GetSystemOverlayVisualState() noexcept;

class SystemOverlay final {
public:
	explicit SystemOverlay(GraphicContext& graphics);
	~SystemOverlay();
	KYTY_CLASS_NO_COPY(SystemOverlay);

	[[nodiscard]] bool PrepareFrame(vk::Extent2D extent, vk::Format format, uint32_t image_count);
	void               Record(vk::CommandBuffer command, vk::ImageView target);
	void               ReleaseVulkan();

	// HUD overlay support (Problem 3: 3D scene under HUD).
	// Call before PrepareFrame to register a HUD texture for alpha compositing
	// on top of the 3D scene. The image must be in VK_IMAGE_LAYOUT_SHADER_READ_ONLY
	// and have a view with the given format. Pass {} to clear the overlay.
	void               SetHudOverlay(vk::ImageView image_view, vk::Extent2D extent, vk::Format format);
	bool               HasHudOverlay() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEMOVERLAY_H_
