#include "graphics/presentation/systemOverlay.h"

#include "SDL.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/dispatchInspector.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/translate/ProbeConfig.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "libs/controller.h"
#include "libs/dialog.h"
#include "libs/ime.h"
#include "libs/imeDialog.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

namespace {

namespace CoreIme   = Libs::Ime;
namespace DialogIme = Libs::Dialog::ImeDialog;
namespace ErrorDialog = Libs::Dialog::ErrorDialog;

namespace Ime {

using Type           = ImeCommon::Type;
using EnterLabel     = ImeCommon::EnterLabel;
using Alignment      = ImeCommon::Alignment;
using ExternalAction = ImeCommon::ExternalAction;
using ExternalInput  = ImeCommon::ExternalInput;
using HostSnapshot   = ImeCommon::HostSnapshot;

constexpr uint32_t OPTION_MULTILINE            = ImeCommon::OPTION_MULTILINE;
constexpr uint32_t OPTION_NO_AUTO_CAPITALIZE   = ImeCommon::OPTION_NO_AUTO_CAPITALIZE;
constexpr uint32_t OPTION_PASSWORD             = ImeCommon::OPTION_PASSWORD;
constexpr uint32_t OPTION_FIXED_POSITION       = ImeCommon::OPTION_FIXED_POSITION;
constexpr uint32_t OPTION_DISABLE_POSITION_ADJ = ImeCommon::OPTION_DISABLE_POSITION_ADJUST;
constexpr uint32_t OPTION_USE_OVER_2K          = ImeCommon::OPTION_USE_OVER_2K;
constexpr uint32_t DISABLE_DEVICE_CONTROLLER   = ImeCommon::DISABLE_DEVICE_CONTROLLER;
constexpr uint32_t DISABLE_DEVICE_EXT_KEYBOARD = ImeCommon::DISABLE_DEVICE_EXT_KEYBOARD;
constexpr uint64_t CORE_GENERATION_BIT         = uint64_t {1} << 63;

uint64_t PackCoreGeneration(uint64_t generation) {
	return generation | CORE_GENERATION_BIT;
}

bool IsCoreGeneration(uint64_t generation) {
	return (generation & CORE_GENERATION_BIT) != 0;
}

uint64_t UnpackGeneration(uint64_t generation) {
	return generation & ~CORE_GENERATION_BIT;
}

bool GetHostSnapshot(HostSnapshot* snapshot) {
	if (snapshot == nullptr) {
		return false;
	}
	if (CoreIme::GetHostSnapshot(snapshot)) {
		snapshot->generation = PackCoreGeneration(snapshot->generation);
		return true;
	}
	return DialogIme::GetHostSnapshot(snapshot);
}

bool HostInsertText(uint64_t generation, std::u16string_view text) {
	return IsCoreGeneration(generation)
	           ? CoreIme::HostInsertText(UnpackGeneration(generation), text)
	           : DialogIme::HostInsertText(generation, text);
}

bool HostBackspace(uint64_t generation) {
	return IsCoreGeneration(generation) ? CoreIme::HostBackspace(UnpackGeneration(generation))
	                                    : DialogIme::HostBackspace(generation);
}

bool HostAccept(uint64_t generation) {
	return IsCoreGeneration(generation) ? CoreIme::HostAccept(UnpackGeneration(generation))
	                                    : DialogIme::HostAccept(generation);
}

bool HostCancel(uint64_t generation) {
	return IsCoreGeneration(generation) ? CoreIme::HostCancel(UnpackGeneration(generation))
	                                    : DialogIme::HostCancel(generation);
}

bool HostQueueExternalInput(uint64_t generation, ExternalInput input) {
	return IsCoreGeneration(generation)
	           ? CoreIme::HostQueueExternalInput(UnpackGeneration(generation), std::move(input))
	           : DialogIme::HostQueueExternalInput(generation, std::move(input));
}

} // namespace Ime

enum class OverlayKind : uint8_t { None, Ime, Error, Debug };

struct OverlaySession {
	OverlayKind kind       = OverlayKind::None;
	uint64_t    generation = 0;

	bool operator==(const OverlaySession&) const = default;
};

struct OverlaySnapshot {
	OverlaySession            session;
	Ime::HostSnapshot         ime;
	ErrorDialog::HostSnapshot error;
};

bool GetOverlaySnapshot(OverlaySnapshot* snapshot) {
	if (ErrorDialog::GetHostSnapshot(&snapshot->error)) {
		snapshot->session = {OverlayKind::Error, snapshot->error.generation};
		return true;
	}
	if (Ime::GetHostSnapshot(&snapshot->ime)) {
		snapshot->session = {OverlayKind::Ime, snapshot->ime.generation};
		return true;
	}
	snapshot->session = {};
	return false;
}

constexpr size_t INPUT_QUEUE_CAPACITY = 128;

enum class InputKind : uint8_t {
	Button,
	Axis,
	Key,
	Character,
	MousePosition,
	MouseButton,
	MouseWheel,
	ResetController
};

struct InputEvent {
	InputKind      kind;
	OverlaySession session;
	int            id;
	float          x;
	float          y;
};

struct VisibilityUpdate {
	OverlaySession session;
	bool           visible;
	bool           capture_controller;
	bool           capture_keyboard;
	bool           text_input;
	bool           multiline;
};

std::atomic<Uint32>          g_visibility_event {static_cast<Uint32>(-1)};
std::mutex                   g_visibility_mutex;
std::mutex                   g_input_mutex;
std::deque<InputEvent>       g_input_events;
std::deque<VisibilityUpdate> g_visibility_updates;
size_t                       g_missing_visibility_wakeups = 0;
bool                         g_input_reset_requested      = false;
uint16_t                     g_last_external_keycode      = 0;
uint32_t                     g_last_external_status       = 0;
OverlaySession               g_input_session;
bool                         g_input_active               = false;
bool                         g_input_controller           = false;
bool                         g_input_keyboard             = false;
bool                         g_input_multiline            = false;
bool                         g_input_lifecycle_active     = false;
bool                         g_controller_captured        = false;
OverlaySession               g_session;
std::atomic<bool>            g_debug_visible {DispatchInspectorEnabled()};
std::atomic<uint64_t>        g_debug_revision {DispatchInspectorEnabled() ? 1u : 0u};

constexpr OverlaySession DebugSession() {
	return {OverlayKind::Debug, 1};
}

void ClearInputEvents() {
	std::scoped_lock lock(g_input_mutex);
	g_input_events.clear();
	g_input_reset_requested = false;
}

void QueueInput(InputEvent event) {
	std::scoped_lock lock(g_input_mutex);
	const bool       replaceable =
	    event.kind == InputKind::Axis || event.kind == InputKind::MousePosition;
	if (replaceable && !g_input_events.empty()) {
		auto& last = g_input_events.back();
		if (last.session == event.session && last.kind == event.kind && last.id == event.id) {
			last = event;
			return;
		}
	}
	if (g_input_events.size() == INPUT_QUEUE_CAPACITY) {
		g_input_events.clear();
		g_input_reset_requested = true;
	}
	g_input_events.push_back(event);
}

void RetryVisibilityWakeup() {
	const Uint32 type = g_visibility_event.load(std::memory_order_acquire);
	if (type == static_cast<Uint32>(-1)) {
		return;
	}
	std::scoped_lock lock(g_input_mutex);
	if (g_missing_visibility_wakeups == 0) {
		return;
	}
	SDL_Event event {};
	event.type = type;
	if (SDL_PushEvent(&event) > 0) {
		g_missing_visibility_wakeups--;
	}
}

void RefreshVisibility() {
	std::scoped_lock visibility_lock(g_visibility_mutex);
	if (!g_input_lifecycle_active) {
		return;
	}
	OverlaySnapshot snapshot;
	const bool      visible = GetOverlaySnapshot(&snapshot);
	if (snapshot.session == g_session) {
		return;
	}
	g_session               = snapshot.session;
	bool capture_controller = false;
	bool capture_keyboard   = false;
	bool text_input         = false;
	bool multiline          = false;
	if (snapshot.session.kind == OverlayKind::Error) {
		capture_controller = true;
		capture_keyboard   = true;
	} else if (snapshot.session.kind == OverlayKind::Ime) {
		capture_controller = (snapshot.ime.disable_device & Ime::DISABLE_DEVICE_CONTROLLER) == 0;
		capture_keyboard   = (snapshot.ime.disable_device & Ime::DISABLE_DEVICE_EXT_KEYBOARD) == 0;
		text_input         = capture_keyboard;
		multiline          = (snapshot.ime.option & Ime::OPTION_MULTILINE) != 0;
	}
	const bool was_controller = std::exchange(g_controller_captured, capture_controller);
	if (capture_controller || was_controller) {
		Controller::ResetInputState();
	}
	const Uint32 type = g_visibility_event.load(std::memory_order_acquire);
	if (type != static_cast<Uint32>(-1)) {
		SDL_Event event {};
		event.type = type;
		std::scoped_lock input_lock(g_input_mutex);
		g_visibility_updates.push_back({snapshot.session, visible, capture_controller,
		                                capture_keyboard, text_input, multiline});
		if (g_missing_visibility_wakeups != 0 || SDL_PushEvent(&event) <= 0) {
			g_missing_visibility_wakeups++;
		}
	}
}

void OnCoreVisibilityChanged(bool, uint64_t) {
	RefreshVisibility();
}

void OnDialogVisibilityChanged(bool, uint64_t) {
	RefreshVisibility();
}

uint32_t ExternalKeyStatus(SDL_Keymod modifiers, bool character_valid) {
	uint32_t status = 0x00000001 | (character_valid ? 0x00000002 : 0);
	if ((modifiers & KMOD_LCTRL) != 0) status |= 0x00000100;
	if ((modifiers & KMOD_LSHIFT) != 0) status |= 0x00000200;
	if ((modifiers & KMOD_LALT) != 0) status |= 0x00000400;
	if ((modifiers & KMOD_LGUI) != 0) status |= 0x00000800;
	if ((modifiers & KMOD_RCTRL) != 0) status |= 0x00001000;
	if ((modifiers & KMOD_RSHIFT) != 0) status |= 0x00002000;
	if ((modifiers & KMOD_RALT) != 0) status |= 0x00004000;
	if ((modifiers & KMOD_RGUI) != 0) status |= 0x00008000;
	if ((modifiers & KMOD_NUM) != 0) status |= 0x00010000;
	if ((modifiers & KMOD_CAPS) != 0) status |= 0x00020000;
	return status;
}

Ime::ExternalInput MakeExternalInput(Ime::ExternalAction action, uint16_t keycode,
                                     uint32_t status) {
	Ime::ExternalInput input {};
	input.key.keycode = keycode;
	input.key.status  = status;
	input.key.type    = 4;
	input.action      = action;
	return input;
}

std::u16string Utf8ToUtf16(std::string_view text) {
	std::u16string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size();) {
		const auto first     = static_cast<uint8_t>(text[i++]);
		uint32_t   codepoint = 0;
		uint32_t   remaining = 0;
		if (first < 0x80) {
			codepoint = first;
		} else if ((first & 0xe0) == 0xc0) {
			codepoint = first & 0x1f;
			remaining = 1;
		} else if ((first & 0xf0) == 0xe0) {
			codepoint = first & 0x0f;
			remaining = 2;
		} else if ((first & 0xf8) == 0xf0) {
			codepoint = first & 0x07;
			remaining = 3;
		} else {
			continue;
		}
		if (i + remaining > text.size()) {
			break;
		}
		bool valid = true;
		for (uint32_t j = 0; j < remaining; j++) {
			const auto next = static_cast<uint8_t>(text[i++]);
			if ((next & 0xc0) != 0x80) {
				valid = false;
				break;
			}
			codepoint = (codepoint << 6) | (next & 0x3f);
		}
		if (!valid || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
			continue;
		}
		if (codepoint <= 0xffff) {
			result.push_back(static_cast<char16_t>(codepoint));
		} else {
			codepoint -= 0x10000;
			result.push_back(static_cast<char16_t>(0xd800 + (codepoint >> 10)));
			result.push_back(static_cast<char16_t>(0xdc00 + (codepoint & 0x3ff)));
		}
	}
	return result;
}

std::string VisibleText(const Ime::HostSnapshot& snapshot, size_t max_units) {
	const size_t cursor        = std::min<size_t>(snapshot.cursor, snapshot.text.size());
	const size_t payload_units = max_units > 4 ? max_units - 4 : 1;
	size_t       begin         = cursor > payload_units / 2 ? cursor - payload_units / 2 : 0;
	size_t       end           = std::min(snapshot.text.size(), begin + payload_units);
	if (end == snapshot.text.size() && end - begin < payload_units) {
		begin = end > payload_units ? end - payload_units : 0;
	}
	if (begin > 0 && snapshot.text[begin] >= 0xdc00 && snapshot.text[begin] <= 0xdfff) {
		begin--;
	}
	if (end < snapshot.text.size() && end > begin && snapshot.text[end - 1] >= 0xd800 &&
	    snapshot.text[end - 1] <= 0xdbff) {
		end--;
	}

	std::u16string visible;
	if (begin != 0) {
		visible.push_back(u'\u2026');
	}
	const size_t caret = visible.size() + cursor - begin;
	if ((snapshot.option & Ime::OPTION_PASSWORD) != 0) {
		visible.append(end - begin, u'*');
	} else {
		visible.append(snapshot.text, begin, end - begin);
		std::replace(visible.begin(), visible.end(), u'\n', u'\u21b5');
		std::replace(visible.begin(), visible.end(), u'\r', u'\u21b5');
	}
	visible.insert(std::min(caret, visible.size()), 1, u'|');
	if (end != snapshot.text.size()) {
		visible.push_back(u'\u2026');
	}
	return Common::Utf16ToUtf8(visible.c_str());
}

const char* EnterLabel(Ime::EnterLabel label) {
	switch (label) {
		case Ime::EnterLabel::Send: return "Send";
		case Ime::EnterLabel::Search: return "Search";
		case Ime::EnterLabel::Go: return "Go";
		default: return "Done";
	}
}

float AlignmentPivot(Ime::Alignment alignment) {
	switch (alignment) {
		case Ime::Alignment::Start: return 0.0f;
		case Ime::Alignment::End: return 1.0f;
		default: return 0.5f;
	}
}

ImGuiKey ControllerButtonToKey(int button) {
	switch (button) {
		case SDL_CONTROLLER_BUTTON_A: return ImGuiKey_GamepadFaceDown;
		case SDL_CONTROLLER_BUTTON_B: return ImGuiKey_GamepadFaceRight;
		case SDL_CONTROLLER_BUTTON_X: return ImGuiKey_GamepadFaceLeft;
		case SDL_CONTROLLER_BUTTON_Y: return ImGuiKey_GamepadFaceUp;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
		default: return ImGuiKey_None;
	}
}

ImGuiKey KeyboardToKey(SDL_Keycode key) {
	switch (key) {
		case SDLK_TAB: return ImGuiKey_Tab;
		case SDLK_LEFT: return ImGuiKey_LeftArrow;
		case SDLK_RIGHT: return ImGuiKey_RightArrow;
		case SDLK_UP: return ImGuiKey_UpArrow;
		case SDLK_DOWN: return ImGuiKey_DownArrow;
		case SDLK_PAGEUP: return ImGuiKey_PageUp;
		case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
		case SDLK_HOME: return ImGuiKey_Home;
		case SDLK_END: return ImGuiKey_End;
		case SDLK_INSERT: return ImGuiKey_Insert;
		case SDLK_DELETE: return ImGuiKey_Delete;
		case SDLK_BACKSPACE: return ImGuiKey_Backspace;
		case SDLK_SPACE: return ImGuiKey_Space;
		case SDLK_RETURN:
		case SDLK_KP_ENTER: return ImGuiKey_Enter;
		case SDLK_ESCAPE: return ImGuiKey_Escape;
		case SDLK_a: return ImGuiKey_A;
		case SDLK_c: return ImGuiKey_C;
		case SDLK_v: return ImGuiKey_V;
		case SDLK_x: return ImGuiKey_X;
		case SDLK_y: return ImGuiKey_Y;
		case SDLK_z: return ImGuiKey_Z;
		default: return ImGuiKey_None;
	}
}

PFN_vkVoidFunction LoadVulkanFunction(const char* name, void* user_data) {
	auto& graphics = *static_cast<GraphicContext*>(user_data);
	return graphics.instance.getProcAddr(name);
}

void CheckVulkanResult(VkResult result) {
	EXIT_IF(result != VK_SUCCESS);
}

bool WriteDiagnosticTrigger(const char* path, std::string_view contents, std::string* error) {
	std::error_code ec;
	const std::filesystem::path output_path {path};
	std::filesystem::create_directories(output_path.parent_path(), ec);
	if (ec) {
		*error = "could not create dump directory: " + ec.message();
		return false;
	}
	std::ofstream file {output_path, std::ios::binary | std::ios::trunc};
	if (!file) {
		*error = "could not open " + output_path.string();
		return false;
	}
	file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
	file.close();
	if (!file.good()) {
		*error = "could not finish writing " + output_path.string();
		return false;
	}
	return true;
}

uint64_t ParseExactInspectorHash(const char* text) {
	while (*text == ' ' || *text == '\t') ++text;
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
	char* end = nullptr;
	const auto value = std::strtoull(text, &end, 16);
	if (end == text) return 0;
	while (*end == ' ' || *end == '\t') ++end;
	return *end == '\0' ? value : 0;
}

} // namespace

void InitializeSystemOverlayInput() {
	const Uint32 type = SDL_RegisterEvents(1);
	EXIT_IF(type == static_cast<Uint32>(-1));
	{
		std::scoped_lock lock(g_visibility_mutex);
		g_input_lifecycle_active = true;
		g_visibility_event.store(type, std::memory_order_release);
	}
	CoreIme::SetVisibilityCallback(OnCoreVisibilityChanged);
	DialogIme::SetVisibilityCallback(OnDialogVisibilityChanged);
	ErrorDialog::SetVisibilityCallback(RefreshVisibility);
	RefreshVisibility();
	if (DispatchInspectorEnabled()) {
		LOGF("DispatchInspector: enabled; F10 toggles panel, visible measurements are not "
		     "performance-comparable\n");
	}
}

void ShutdownSystemOverlayInput() {
	CoreIme::SetVisibilityCallback(nullptr);
	DialogIme::SetVisibilityCallback(nullptr);
	ErrorDialog::SetVisibilityCallback(nullptr);
	{
		std::scoped_lock lock(g_visibility_mutex);
		g_input_lifecycle_active = false;
		g_session                = {};
		if (std::exchange(g_controller_captured, false)) {
			Controller::ResetInputState();
		}
		g_visibility_event.store(static_cast<Uint32>(-1), std::memory_order_release);
	}
	ClearInputEvents();
	{
		std::scoped_lock lock(g_input_mutex);
		g_visibility_updates.clear();
		g_missing_visibility_wakeups = 0;
	}
	g_input_active     = false;
	g_input_session    = {};
	g_input_controller = false;
	g_input_keyboard   = false;
	g_input_multiline  = false;
	if (SDL_IsTextInputActive() == SDL_TRUE) {
		SDL_StopTextInput();
	}
}

SystemOverlayVisualState GetSystemOverlayVisualState() noexcept {
	const auto core   = CoreIme::GetVisualState();
	const auto dialog = DialogIme::GetVisualState();
	const auto error  = ErrorDialog::GetVisualState();
	return {core.active || dialog.active || error.active ||
	            g_debug_visible.load(std::memory_order_acquire),
	        core.revision + dialog.revision + error.revision +
	            g_debug_revision.load(std::memory_order_acquire)};
}

bool ProcessSystemOverlayInput(const SDL_Event& event) {
	RetryVisibilityWakeup();
	if (DispatchInspectorEnabled() && (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
	    event.key.keysym.sym == SDLK_F10) {
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
			g_debug_visible.store(!g_debug_visible.load(std::memory_order_acquire),
			                      std::memory_order_release);
			g_debug_revision.fetch_add(1, std::memory_order_acq_rel);
			ClearInputEvents();
		}
		return true;
	}
	if (event.type == g_visibility_event.load(std::memory_order_acquire)) {
		VisibilityUpdate update {};
		{
			std::scoped_lock lock(g_input_mutex);
			if (g_visibility_updates.empty()) {
				return true;
			}
			update = g_visibility_updates.front();
			g_visibility_updates.pop_front();
		}
		ClearInputEvents();
		g_last_external_keycode = 0;
		g_last_external_status  = 0;
		g_input_session         = update.session;
		g_input_active          = update.visible;
		g_input_controller      = update.capture_controller;
		g_input_keyboard        = update.capture_keyboard;
		g_input_multiline       = update.multiline;
		if (update.text_input) {
			SDL_StartTextInput();
		} else if (SDL_IsTextInputActive() == SDL_TRUE) {
			SDL_StopTextInput();
		}
		return true;
	}
	if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
		if (g_input_active && g_input_controller) {
			QueueInput({InputKind::ResetController, g_input_session, 0, 0.0f, 0.0f});
		}
		return false;
	}
	if (!g_input_active && g_debug_visible.load(std::memory_order_acquire)) {
		const auto session = DebugSession();
		switch (event.type) {
			case SDL_KEYDOWN:
			case SDL_KEYUP: {
				const auto key = KeyboardToKey(event.key.keysym.sym);
				if (key != ImGuiKey_None) {
					QueueInput({InputKind::Key, session, static_cast<int>(key),
					            event.type == SDL_KEYDOWN ? 1.0f : 0.0f, 0.0f});
				}
				if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
				    (event.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) == 0 &&
				    event.key.keysym.sym >= 32 && event.key.keysym.sym < 127) {
					QueueInput(
					    {InputKind::Character, session, event.key.keysym.sym, 0.0f, 0.0f});
				}
				return true;
			}
			case SDL_MOUSEMOTION:
				QueueInput({InputKind::MousePosition, session, 0,
				            static_cast<float>(event.motion.x), static_cast<float>(event.motion.y)});
				return true;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				QueueInput({InputKind::MousePosition, session, 0,
				            static_cast<float>(event.button.x), static_cast<float>(event.button.y)});
				QueueInput({InputKind::MouseButton, session, event.button.button,
				            event.type == SDL_MOUSEBUTTONDOWN ? 1.0f : 0.0f, 0.0f});
				return true;
			case SDL_MOUSEWHEEL:
				QueueInput({InputKind::MouseWheel, session, 0, static_cast<float>(event.wheel.x),
				            static_cast<float>(event.wheel.y)});
				return true;
			default: return false;
		}
	}
	if (!g_input_active) {
		return false;
	}

	const auto     session          = g_input_session;
	const uint64_t generation       = session.generation;
	const bool     controller_event = event.type == SDL_CONTROLLERBUTTONDOWN ||
	                                  event.type == SDL_CONTROLLERBUTTONUP ||
	                                  event.type == SDL_CONTROLLERAXISMOTION;
	if (controller_event && !g_input_controller) {
		return false;
	}
	const bool keyboard_event = event.type == SDL_TEXTINPUT || event.type == SDL_TEXTEDITING ||
	                            event.type == SDL_KEYDOWN || event.type == SDL_KEYUP;
	if (keyboard_event && !g_input_keyboard) {
		return false;
	}
	if (keyboard_event && session.kind == OverlayKind::Error) {
		if (event.type == SDL_KEYDOWN && event.key.repeat == 0 &&
		    (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER)) {
			ErrorDialog::HostAccept(generation);
		}
		return true;
	}
	switch (event.type) {
		case SDL_TEXTINPUT: {
			const auto text = Utf8ToUtf16(event.text.text);
			if (!text.empty()) {
				auto input = MakeExternalInput(Ime::ExternalAction::Text, g_last_external_keycode,
				                               g_last_external_status | 0x00000002);
				input.key.character = text.front();
				input.text          = text;
				Ime::HostQueueExternalInput(generation, std::move(input));
			}
			return true;
		}
		case SDL_TEXTEDITING: return true;
		case SDL_KEYDOWN: {
			g_last_external_keycode = static_cast<uint16_t>(event.key.keysym.scancode);
			g_last_external_status =
			    ExternalKeyStatus(static_cast<SDL_Keymod>(event.key.keysym.mod), false);
			auto action = Ime::ExternalAction::Text;
			bool queue  = true;
			if (event.key.keysym.sym == SDLK_BACKSPACE) {
				action = Ime::ExternalAction::Backspace;
			} else if (event.key.keysym.sym == SDLK_LEFT) {
				action = Ime::ExternalAction::MoveLeft;
			} else if (event.key.keysym.sym == SDLK_RIGHT) {
				action = Ime::ExternalAction::MoveRight;
			} else if (event.key.keysym.sym == SDLK_ESCAPE) {
				action = Ime::ExternalAction::Cancel;
			} else if (event.key.keysym.sym == SDLK_RETURN ||
			           event.key.keysym.sym == SDLK_KP_ENTER) {
				action =
				    g_input_multiline ? Ime::ExternalAction::Newline : Ime::ExternalAction::Accept;
			} else if (event.key.keysym.sym == SDLK_TAB) {
				action = Ime::ExternalAction::None;
			} else {
				queue = false;
			}
			if (queue) {
				Ime::HostQueueExternalInput(
				    generation,
				    MakeExternalInput(action, g_last_external_keycode, g_last_external_status));
			}
			return true;
		}
		case SDL_KEYUP: return true;
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
			QueueInput({InputKind::Button, session, event.cbutton.button,
			            event.type == SDL_CONTROLLERBUTTONDOWN ? 1.0f : 0.0f, 0.0f});
			return true;
		case SDL_CONTROLLERAXISMOTION:
			QueueInput({InputKind::Axis, session, event.caxis.axis,
			            static_cast<float>(event.caxis.value), 0.0f});
			return true;
		case SDL_MOUSEMOTION:
			QueueInput({InputKind::MousePosition, session, 0, static_cast<float>(event.motion.x),
			            static_cast<float>(event.motion.y)});
			return true;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			QueueInput({InputKind::MousePosition, session, 0, static_cast<float>(event.button.x),
			            static_cast<float>(event.button.y)});
			QueueInput({InputKind::MouseButton, session, event.button.button,
			            event.type == SDL_MOUSEBUTTONDOWN ? 1.0f : 0.0f, 0.0f});
			return true;
		case SDL_MOUSEWHEEL:
			QueueInput({InputKind::MouseWheel, session, 0, static_cast<float>(event.wheel.x),
			            static_cast<float>(event.wheel.y)});
			return true;
		default: return false;
	}
}

struct SystemOverlay::Impl {
	explicit Impl(GraphicContext& context): graphics(context) {}

	~Impl() {
		ReleaseVulkan();
		if (imgui_context != nullptr) {
			ImGui::DestroyContext(imgui_context);
		}
	}

	void EnsureContext() {
		if (imgui_context != nullptr) {
			ImGui::SetCurrentContext(imgui_context);
			return;
		}
		IMGUI_CHECKVERSION();
		imgui_context = ImGui::CreateContext();
		ImGui::SetCurrentContext(imgui_context);
		auto& io       = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
		io.ConfigNavCursorVisibleAlways = true;
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		io.BackendPlatformName = "Kyty system overlay input";
		ImGui::StyleColorsDark();
		auto& style          = ImGui::GetStyle();
		style.WindowRounding = 10.0f;
		style.FrameRounding  = 6.0f;
		style.ItemSpacing    = {8.0f, 8.0f};
	}

	void EnsureVulkan(vk::Format format, uint32_t image_count) {
		EnsureContext();
		if (vulkan_initialized) {
			return;
		}
		EXIT_IF(image_count < 2);
		EXIT_IF(!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, LoadVulkanFunction, &graphics));

		const VkFormat            color_format = static_cast<VkFormat>(format);
		ImGui_ImplVulkan_InitInfo info {};
		info.ApiVersion                   = VK_API_VERSION_1_3;
		info.Instance                     = static_cast<VkInstance>(graphics.instance);
		info.PhysicalDevice               = static_cast<VkPhysicalDevice>(graphics.physical_device);
		info.Device                       = static_cast<VkDevice>(graphics.device);
		info.QueueFamily                  = graphics.queue_family;
		info.Queue                        = static_cast<VkQueue>(graphics.queue);
		info.DescriptorPoolSize           = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
		info.MinImageCount                = image_count;
		info.ImageCount                   = image_count;
		info.UseDynamicRendering          = true;
		info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
		    VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
		info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount    = 1;
		info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color_format;
		info.CheckVkResultFn = CheckVulkanResult;
		EXIT_IF(!ImGui_ImplVulkan_Init(&info));
		vulkan_initialized = true;
	}

	void DrainInput(OverlaySession session) {
		std::deque<InputEvent> events;
		bool                   reset = false;
		{
			std::scoped_lock lock(g_input_mutex);
			events.swap(g_input_events);
			reset                   = g_input_reset_requested;
			g_input_reset_requested = false;
		}
		auto& io = ImGui::GetIO();
		if (reset) {
			io.ClearEventsQueue();
			io.ClearInputKeys();
			io.ClearInputMouse();
			right_stick = {};
		}
		for (const auto& event: events) {
			if (event.session != session) {
				continue;
			}
			switch (event.kind) {
				case InputKind::Button: {
					const bool down = event.x != 0.0f;
					if (session.kind == OverlayKind::Ime && down) {
						if (event.id == SDL_CONTROLLER_BUTTON_B) {
							Ime::HostCancel(session.generation);
						} else if (event.id == SDL_CONTROLLER_BUTTON_Y) {
							Ime::HostBackspace(session.generation);
						}
					}
					const ImGuiKey key = ControllerButtonToKey(event.id);
					if (key != ImGuiKey_None) {
						io.AddKeyEvent(key, down);
					}
					break;
				}
				case InputKind::Axis: {
					const float value = event.x / (event.x < 0.0f ? 32768.0f : 32767.0f);
					if (event.id == SDL_CONTROLLER_AXIS_LEFTX) {
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, value < -0.25f,
						                     std::max(-value, 0.0f));
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, value > 0.25f,
						                     std::max(value, 0.0f));
					} else if (event.id == SDL_CONTROLLER_AXIS_LEFTY) {
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, value < -0.25f,
						                     std::max(-value, 0.0f));
						io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, value > 0.25f,
						                     std::max(value, 0.0f));
					} else if (event.id == SDL_CONTROLLER_AXIS_RIGHTX) {
						right_stick.x = std::abs(value) > 0.2f ? value : 0.0f;
					} else if (event.id == SDL_CONTROLLER_AXIS_RIGHTY) {
						right_stick.y = std::abs(value) > 0.2f ? value : 0.0f;
					}
					break;
				}
				case InputKind::Key:
					io.AddKeyEvent(static_cast<ImGuiKey>(event.id), event.x != 0.0f);
					break;
				case InputKind::Character:
					io.AddInputCharacter(static_cast<unsigned int>(event.id));
					break;
				case InputKind::MousePosition: io.AddMousePosEvent(event.x, event.y); break;
				case InputKind::MouseButton: {
					int button = -1;
					if (event.id == SDL_BUTTON_LEFT) button = 0;
					if (event.id == SDL_BUTTON_RIGHT) button = 1;
					if (event.id == SDL_BUTTON_MIDDLE) button = 2;
					if (button >= 0) io.AddMouseButtonEvent(button, event.x != 0.0f);
					break;
				}
				case InputKind::MouseWheel: io.AddMouseWheelEvent(event.x, event.y); break;
				case InputKind::ResetController:
					io.ClearEventsQueue();
					io.ClearInputKeys();
					right_stick = {};
					break;
			}
		}
	}

	void KeyButton(std::string_view label, char16_t value, uint64_t generation, float width,
	               bool default_focus) {
		const bool pressed = ImGui::Button(label.data(), {width, button_height});
		if (default_focus) {
			ImGui::SetItemDefaultFocus();
		}
		if (pressed) {
			Ime::HostInsertText(generation, std::u16string_view(&value, 1));
		}
	}

	void DrawKeyRows(const Ime::HostSnapshot& snapshot, float width) {
		static constexpr std::array<std::string_view, 4> lower   = {"1234567890", "qwertyuiop",
		                                                            "asdfghjkl", "zxcvbnm"};
		static constexpr std::array<std::string_view, 4> upper   = {"1234567890", "QWERTYUIOP",
		                                                            "ASDFGHJKL", "ZXCVBNM"};
		static constexpr std::array<std::string_view, 3> symbols = {"1234567890", "!@#$%^&*()",
		                                                            "-_=+/:?."};
		static constexpr std::array<std::string_view, 4> number  = {"123", "456", "789", "-0."};

		std::span<const std::string_view> rows;
		if (snapshot.type == Ime::Type::Number) {
			rows = number;
		} else if (symbol_mode) {
			rows = symbols;
		} else {
			rows = shift ? std::span<const std::string_view>(upper)
			             : std::span<const std::string_view>(lower);
		}
		bool   first     = focus_pending;
		size_t row_index = 0;
		for (const auto row: rows) {
			const float key_width =
			    std::min(56.0f * ui_scale, (width - 8.0f * (row.size() - 1)) / row.size());
			const float row_width = key_width * row.size() + 8.0f * (row.size() - 1);
			ImGui::SetCursorPosX((width - row_width) * 0.5f);
			ImGui::PushID(static_cast<int>(row_index++));
			for (size_t i = 0; i < row.size(); i++) {
				ImGui::PushID(static_cast<int>(i));
				const char label[2] = {row[i], '\0'};
				KeyButton(label, static_cast<char16_t>(row[i]), snapshot.generation, key_width,
				          first);
				first = false;
				ImGui::PopID();
				if (i + 1 < row.size()) ImGui::SameLine();
			}
			ImGui::PopID();
		}
		focus_pending = false;
	}

	void DrawIme(const Ime::HostSnapshot& snapshot, vk::Extent2D extent) {
		const ImVec2 display(static_cast<float>(extent.width), static_cast<float>(extent.height));
		const bool   over_2k          = (snapshot.option & Ime::OPTION_USE_OVER_2K) != 0;
		const float  reference_width  = over_2k ? 3840.0f : 1920.0f;
		const float  reference_height = over_2k ? 2160.0f : 1080.0f;
		const float  scale_x          = display.x / reference_width;
		const float  scale_y          = display.y / reference_height;
		const float  width            = static_cast<float>(snapshot.panel_width) * scale_x;
		const float  height           = static_cast<float>(snapshot.panel_height) * scale_y;
		ui_scale                      = std::min(scale_x, scale_y) * (over_2k ? 2.0f : 1.0f);
		button_height                 = std::max(28.0f, 42.0f * ui_scale);

		const ImVec2 pivot {AlignmentPivot(snapshot.horizontal_alignment),
		                    AlignmentPivot(snapshot.vertical_alignment)};
		if ((snapshot.option & Ime::OPTION_FIXED_POSITION) == 0) {
			const float movement = 600.0f * ImGui::GetIO().DeltaTime;
			panel_offset.x += right_stick.x * movement;
			panel_offset.y += right_stick.y * movement;
		}
		const ImVec2 base_position {snapshot.posx * scale_x - pivot.x * width,
		                            snapshot.posy * scale_y - pivot.y * height};
		ImVec2       position {base_position.x + panel_offset.x, base_position.y + panel_offset.y};
		if ((snapshot.option & Ime::OPTION_DISABLE_POSITION_ADJ) == 0) {
			position.x   = std::clamp(position.x, 0.0f, std::max(display.x - width, 0.0f));
			position.y   = std::clamp(position.y, 0.0f, std::max(display.y - height, 0.0f));
			panel_offset = {position.x - base_position.x, position.y - base_position.y};
		}
		ImGui::SetNextWindowPos(position, ImGuiCond_Always);
		ImGui::SetNextWindowSize({width, height}, ImGuiCond_Always);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings;
		ImGui::Begin("##Ime", nullptr, flags);

		const std::u16string title = snapshot.title.empty() ? u"Enter text" : snapshot.title;
		if (!snapshot.key_panel_visible) {
			std::string compact     = Common::Utf16ToUtf8(title.c_str()) + ": ";
			const float glyph_width = std::max(ImGui::CalcTextSize("M").x, 1.0f);
			const float text_width =
			    std::max(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(compact.c_str()).x,
			             glyph_width);
			const size_t visible_units =
			    std::max<size_t>(6, static_cast<size_t>(text_width / glyph_width));
			compact += snapshot.text.empty() && !snapshot.placeholder.empty()
			               ? Common::Utf16ToUtf8(snapshot.placeholder.c_str())
			               : VisibleText(snapshot, visible_units);
			ImGui::TextUnformatted(compact.c_str());
			const float compact_height = std::min(button_height, 28.0f * ui_scale);
			if (ImGui::Button("Cancel", {90.0f * ui_scale, compact_height})) {
				Ime::HostCancel(snapshot.generation);
			}
			ImGui::SameLine();
			if (ImGui::Button(EnterLabel(snapshot.enter_label),
			                  {90.0f * ui_scale, compact_height})) {
				Ime::HostAccept(snapshot.generation);
			}
			ImGui::End();
			return;
		}

		ImGui::TextUnformatted(Common::Utf16ToUtf8(title.c_str()).c_str());
		ImGui::Separator();
		const float text_height = std::max(
		    32.0f, ((snapshot.option & Ime::OPTION_MULTILINE) != 0 ? 80.0f : 48.0f) * ui_scale);
		ImGui::BeginChild("##ImeText", {0.0f, text_height}, true);
		if (snapshot.text.empty() && !snapshot.placeholder.empty()) {
			ImGui::TextDisabled("%s", Common::Utf16ToUtf8(snapshot.placeholder.c_str()).c_str());
		} else {
			const float  glyph_width = std::max(ImGui::CalcTextSize("M").x, 1.0f);
			const size_t columns     = std::max<size_t>(
			    8, static_cast<size_t>(ImGui::GetContentRegionAvail().x / glyph_width));
			const size_t lines =
			    std::max<size_t>(1, static_cast<size_t>(ImGui::GetContentRegionAvail().y /
			                                            ImGui::GetTextLineHeightWithSpacing()));
			const auto text = VisibleText(snapshot, columns * lines);
			ImGui::TextWrapped("%s", text.c_str());
		}
		ImGui::EndChild();
		ImGui::TextDisabled("%zu / %u", snapshot.text.size(), snapshot.max_text_length);
		ImGui::Separator();

		const float content_width = ImGui::GetContentRegionAvail().x;
		DrawKeyRows(snapshot, content_width);
		if (snapshot.type != Ime::Type::Number) {
			if (ImGui::Button(shift ? "Lower" : "Shift", {84.0f * ui_scale, button_height})) {
				shift = !shift;
			}
			ImGui::SameLine();
			if (ImGui::Button(symbol_mode ? "ABC" : "Symbols", {84.0f * ui_scale, button_height})) {
				symbol_mode = !symbol_mode;
			}
			ImGui::SameLine();
			if (ImGui::Button("Space", {120.0f * ui_scale, button_height})) {
				Ime::HostInsertText(snapshot.generation, u" ");
			}
			ImGui::SameLine();
		}
		const float action_width = snapshot.type == Ime::Type::Number
		                               ? std::max((content_width - 16.0f) / 3.0f, 48.0f)
		                               : 100.0f * ui_scale;
		if (ImGui::Button("Backspace", {action_width, button_height})) {
			Ime::HostBackspace(snapshot.generation);
		}
		ImGui::SameLine();
		if ((snapshot.option & Ime::OPTION_MULTILINE) != 0) {
			if (ImGui::Button("Newline", {action_width, button_height})) {
				Ime::HostInsertText(snapshot.generation, u"\n");
			}
			ImGui::SameLine();
		}
		if (ImGui::Button("Cancel", {action_width, button_height})) {
			Ime::HostCancel(snapshot.generation);
		}
		ImGui::SameLine();
		if (ImGui::Button(EnterLabel(snapshot.enter_label), {action_width, button_height})) {
			Ime::HostAccept(snapshot.generation);
		}
		ImGui::End();
	}

	void DrawError(const ErrorDialog::HostSnapshot& snapshot, vk::Extent2D extent) {
		const ImVec2 display(static_cast<float>(extent.width), static_cast<float>(extent.height));
		const float  scale = std::max(std::min(display.x / 1280.0f, display.y / 720.0f), 0.5f);
		ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, display,
		                                              IM_COL32(0, 0, 0, 160));
		ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always,
		                        {0.5f, 0.5f});
		ImGui::SetNextWindowSize(
		    {std::max(std::min(620.0f * scale, display.x - 32.0f), 1.0f), 0.0f}, ImGuiCond_Always);
		if (focus_pending) {
			ImGui::SetNextWindowFocus();
		}
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.0f * scale, 24.0f * scale});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.0f * scale, 16.0f * scale});
		ImGui::PushFont(nullptr, 20.0f * scale);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
		                                   ImGuiWindowFlags_NoSavedSettings |
		                                   ImGuiWindowFlags_AlwaysAutoResize;
		ImGui::Begin("##SystemError", nullptr, flags);
		ImGui::TextUnformatted("Error");
		ImGui::Separator();
		const auto error_code = static_cast<uint32_t>(snapshot.error_code);
		ImGui::TextWrapped("%s", error_code == 0x80550006u
		                             ? "You are not signed in to PlayStation Network."
		                             : "An error has occurred.");
		ImGui::TextDisabled("Error code: 0x%08X", error_code);
		const float button_width = std::min(140.0f * scale, ImGui::GetContentRegionAvail().x);
		ImGui::SetCursorPosX((ImGui::GetWindowSize().x - button_width) * 0.5f);
		const bool accepted = ImGui::Button("OK", {button_width, 44.0f * scale});
		if (focus_pending) {
			ImGui::SetItemDefaultFocus();
			focus_pending = false;
		}
		ImGui::End();
		ImGui::PopFont();
		ImGui::PopStyleVar(2);
		if (accepted) {
			ErrorDialog::HostAccept(snapshot.generation);
		}
	}

	void DrawDebugPanel(vk::Extent2D frame_extent) {
		InspectorFrame newest;
		if (GetDispatchInspectorFrame(&newest)) {
			const bool take = inspector_live || inspector_frame.operations.empty() ||
			                  (inspector_waiting_frame != 0 &&
			                   newest.frame > inspector_waiting_frame);
			if (take) {
				const bool new_frame = newest.frame != inspector_frame.frame;
				const bool grew = newest.operations.size() != inspector_frame.operations.size();
				inspector_frame = std::move(newest);
				if (new_frame) {
					selected_operation = UINT32_MAX;
					selected_resource_address = 0;
				}
				if (new_frame || grew || inspector_index.frame != inspector_frame.frame) {
					BuildInspectorIndex(inspector_frame, &inspector_index);
				}
				if (inspector_waiting_frame != 0 &&
				    inspector_frame.frame > inspector_waiting_frame) {
					inspector_waiting_frame = 0;
				}
			}
		}

		const ImVec2 display {static_cast<float>(frame_extent.width),
		                      static_cast<float>(frame_extent.height)};
		ImGui::SetNextWindowPos({16.0f, 16.0f}, ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize({std::min(display.x - 32.0f, 1120.0f),
		                          std::min(display.y - 32.0f, 760.0f)},
		                         ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Kyty GPU Dispatch / Draw Inspector")) {
			ImGui::End();
			return;
		}
		const auto capture_records = GetInspectorCaptureRecords();
		ImGui::TextColored({1.0f, 0.72f, 0.2f, 1.0f},
		                   "DIAGNOSTIC OVERLAY ACTIVE - do not compare performance to the 5 fps baseline");
		ImGui::Separator();

		if (ImGui::Checkbox("Live", &inspector_live) && inspector_live) {
			inspector_waiting_frame = 0;
		}
		ImGui::SameLine();
		if (ImGui::Button("Hold current")) {
			inspector_live          = false;
			inspector_waiting_frame = 0;
		}
		ImGui::SameLine();
		if (ImGui::Button("Step one frame")) {
			inspector_live          = false;
			inspector_waiting_frame = inspector_frame.frame;
		}
		ImGui::SameLine();
		ImGui::Text("frame=%" PRIu64 "  operations=%zu%s", inspector_frame.frame,
		            inspector_frame.operations.size(),
		            inspector_waiting_frame != 0 ? "  (waiting for next frame)" : "");
		if (inspector_frame.dropped_operations != 0 || inspector_frame.dropped_resources != 0) {
			ImGui::TextColored({1.0f, 0.35f, 0.25f, 1.0f},
			                   "capture capped: dropped operations=%u resources=%u",
			                   inspector_frame.dropped_operations, inspector_frame.dropped_resources);
		}

		ImGui::SetNextItemWidth(230.0f);
		ImGui::InputTextWithHint("##ShaderFilter", "shader hash filter", inspector_filter.data(),
		                         inspector_filter.size());
		ImGui::SameLine();
		ImGui::SetNextItemWidth(210.0f);
		ImGui::InputTextWithHint("##AddressFilter", "guest address filter",
		                         inspector_address_filter.data(), inspector_address_filter.size());
		ImGui::SameLine();
		ImGui::Checkbox("Dispatches", &inspector_show_dispatches);
		ImGui::SameLine();
		ImGui::Checkbox("Draws", &inspector_show_draws);
		ImGui::SameLine();
		ImGui::Checkbox("Copies", &inspector_show_copies);

		const uint64_t exact_filter = ParseExactInspectorHash(inspector_filter.data());
		const uint64_t address_filter = ParseExactInspectorHash(inspector_address_filter.data());
		if (ImGui::Button("Dump frame (.txt + .json)")) {
			const auto result = DumpDispatchInspectorFrame(inspector_frame);
			inspector_status = result.success ? "wrote " + result.text_path + " and " + result.json_path
			                                  : "dump failed: " + result.error;
		}
		ImGui::SameLine();
		if (ImGui::Button("Dump exact hash")) {
			if (exact_filter == 0) {
				inspector_status = "enter a complete non-zero shader hash before dumping a filter";
			} else {
				const auto result = DumpDispatchInspectorFrame(inspector_frame, exact_filter);
				inspector_status = result.success
				                       ? "wrote " + result.text_path + " and " + result.json_path
				                       : "dump failed: " + result.error;
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Dump selected")) {
			const auto operation = selected_operation < inspector_frame.operations.size()
			                           ? static_cast<int32_t>(selected_operation)
			                           : -1;
			if (operation < 0) {
				inspector_status = "select an operation before dumping it";
			} else {
				const auto result = DumpDispatchInspectorFrame(inspector_frame, 0, operation);
				inspector_status = result.success
				                       ? "wrote " + result.text_path + " and " + result.json_path
				                       : "dump failed: " + result.error;
			}
		}
		if (!inspector_status.empty()) ImGui::TextWrapped("%s", inspector_status.c_str());

		const char* filter = inspector_filter.data();
		while (filter[0] == '0' && (filter[1] == 'x' || filter[1] == 'X')) {
			filter += 2;
		}
		std::vector<uint32_t> address_ops;
		if (address_filter != 0) {
			InspectorOpsForGuestAddress(inspector_index, address_filter, 1, &address_ops);
		}
		std::vector<uint32_t> visible;
		visible.reserve(address_filter != 0 ? address_ops.size() : inspector_frame.operations.size());
		auto consider = [&](uint32_t index) {
			if (index >= inspector_frame.operations.size()) {
				return;
			}
			const auto& operation = inspector_frame.operations[index];
			if ((operation.kind == InspectorOperationKind::Draw && !inspector_show_draws) ||
			    (operation.kind == InspectorOperationKind::Dispatch &&
			     !inspector_show_dispatches) ||
			    ((operation.kind == InspectorOperationKind::Copy ||
			      operation.kind == InspectorOperationKind::Resolve) &&
			     !inspector_show_copies)) {
				return;
			}
			bool matches = filter[0] == '\0';
			for (const auto& stage: operation.stages) {
				char hash[17] {};
				std::snprintf(hash, sizeof(hash), "%016" PRIx64, stage.shader_hash);
				matches |= std::strstr(hash, filter) != nullptr;
			}
			if (matches || (filter[0] != '\0' && (operation.kind == InspectorOperationKind::Copy ||
			                                      operation.kind == InspectorOperationKind::Resolve) &&
			                address_filter != 0)) {
				visible.push_back(index);
			}
		};
		if (address_filter != 0) {
			for (const auto index: address_ops) consider(index);
		} else {
			for (uint32_t index = 0; index < inspector_frame.operations.size(); ++index) {
				consider(index);
			}
		}
		if (address_filter != 0) {
			ImGui::Text("guest filter 0x%016" PRIx64 " matches %zu / %zu operations", address_filter,
			            visible.size(), inspector_frame.operations.size());
		}
		constexpr ImGuiTableFlags list_flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
		                                             ImGuiTableFlags_Resizable |
		                                             ImGuiTableFlags_ScrollY |
		                                             ImGuiTableFlags_SizingFixedFit;
		if (!inspector_index.alias_groups.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Button, {0.45f, 0.22f, 0.05f, 1.0f});
			if (ImGui::Button(fmt::format("ALIAS WARNING: {} guest range(s) resolve to multiple image IDs",
			                              inspector_index.alias_groups.size())
			                      .c_str())) {
				inspector_show_alias = true;
			}
			ImGui::PopStyleColor();
		}

		if (inspector_show_alias && ImGui::CollapsingHeader("Alias Inspector",
		                                                    ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::TextDisabled("Evidence only - overlapping host representations of guest memory. "
			                    "This list does not declare a correct image.");
			if (ImGui::BeginTable("##AliasGroups", 4, list_flags, {0.0f, 140.0f})) {
				ImGui::TableSetupColumn("Group");
				ImGui::TableSetupColumn("Guest range");
				ImGui::TableSetupColumn("Representations");
				ImGui::TableSetupColumn("Identical range?");
				ImGui::TableHeadersRow();
				for (const auto& group: inspector_index.alias_groups) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					char label[32] {};
					std::snprintf(label, sizeof(label), "%u##alias%u", group.id, group.id);
					if (ImGui::Selectable(label, selected_alias_group == group.id,
					                      ImGuiSelectableFlags_SpanAllColumns)) {
						selected_alias_group = group.id;
						selected_alias_repr  = 0;
					}
					ImGui::TableNextColumn();
					ImGui::Text("0x%016" PRIx64 " .. 0x%016" PRIx64, group.min_address, group.max_end);
					ImGui::TableNextColumn();
					ImGui::Text("%zu", group.representations.size());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(group.overlapping_non_identical ? "overlapping, not identical"
					                                                       : "same range");
				}
				ImGui::EndTable();
			}
			const InspectorAliasGroup* alias_group = nullptr;
			for (const auto& group: inspector_index.alias_groups) {
				if (group.id == selected_alias_group) {
					alias_group = &group;
					break;
				}
			}
			if (alias_group != nullptr) {
				ImGui::SeparatorText("Representations");
				for (uint32_t repr = 0; repr < alias_group->representations.size(); ++repr) {
					const auto& item = alias_group->representations[repr];
					ImGui::PushID(static_cast<int>(repr));
					const bool chosen = selected_alias_repr == repr;
					if (ImGui::RadioButton("##repr", chosen)) {
						selected_alias_repr = repr;
					}
					ImGui::SameLine();
					ImGui::Text("image %u.%u  addr=0x%016" PRIx64 " +%" PRIu64
					            "  %ux%ux%u guest_fmt=%d vk=%d last_writer=%s last_use=%u tick=%" PRIu64
					            " %s%s",
					            item.image_id, item.image_generation, item.address, item.size,
					            item.width, item.height, item.depth, item.guest_format, item.actual_vk,
					            item.last_writer_op == UINT32_MAX ? "-"
					                                              : std::to_string(item.last_writer_op).c_str(),
					            item.last_use_op, item.last_access_tick, item.read ? "R" : "-",
					            item.written ? "W" : "-");
					ImGui::PopID();
				}
				const auto& chosen = alias_group->representations[std::min(
				    selected_alias_repr, static_cast<uint32_t>(alias_group->representations.size() - 1))];
				if (ImGui::Button("Filter operations to this alias")) {
					std::snprintf(inspector_address_filter.data(), inspector_address_filter.size(),
					              "0x%llx", static_cast<unsigned long long>(chosen.address));
					inspector_status = "address filter set to the selected alias range";
				}
				ImGui::SameLine();
				if (ImGui::Button("Jump to last writer") && chosen.last_writer_op != UINT32_MAX) {
					selected_operation  = chosen.last_writer_op;
					scroll_to_operation = chosen.last_writer_op;
				}
				ImGui::SameLine();
				if (ImGui::Button("Capture representation")) {
					InspectorCaptureArm arm;
					arm.mode             = InspectorCaptureMode::Resource;
					arm.image_id         = chosen.image_id;
					arm.image_generation = chosen.image_generation;
					arm.address          = chosen.address;
					ArmInspectorCapture(arm);
					inspector_status =
					    "armed capture of this representation on the next use. Step one frame.";
				}
				ImGui::SameLine();
				if (ImGui::Button("Compare representations")) {
					std::string report;
					for (const auto& item: alias_group->representations) {
						report += fmt::format("img {}.{} addr=0x{:x} {}x{}x{} guest_fmt={} vk={} ",
						                      item.image_id, item.image_generation, item.address,
						                      item.width, item.height, item.depth, item.guest_format,
						                      item.actual_vk);
						bool found = false;
						for (const auto& record: capture_records) {
							if (record.image_id == item.image_id &&
							    record.image_generation == item.image_generation && record.ready) {
								report += fmt::format("xxh3=0x{:016x} ({})\n", record.xxh3,
								                      record.stem);
								found = true;
								break;
							}
						}
						if (!found) {
							report += "no capture yet\n";
						}
					}
					inspector_status = report;
				}
			}
		}

		const int columns = address_filter != 0 ? 6 : 5;
		if (ImGui::BeginTable("##Operations", columns, list_flags, {0.0f, 300.0f})) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 48.0f);
			ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 120.0f);
			ImGui::TableSetupColumn("Shader", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Work", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Resources", ImGuiTableColumnFlags_WidthFixed, 76.0f);
			if (address_filter != 0) {
				ImGui::TableSetupColumn("Match", ImGuiTableColumnFlags_WidthFixed, 110.0f);
			}
			ImGui::TableHeadersRow();
			if (scroll_to_operation != UINT32_MAX) {
				for (int row = 0; row < static_cast<int>(visible.size()); ++row) {
					if (visible[static_cast<size_t>(row)] == scroll_to_operation) {
						ImGui::SetScrollY(static_cast<float>(row) *
						                  ImGui::GetTextLineHeightWithSpacing());
						break;
					}
				}
				scroll_to_operation = UINT32_MAX;
			}
			ImGuiListClipper clipper;
			clipper.Begin(static_cast<int>(visible.size()));
			while (clipper.Step()) {
				for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
					const uint32_t index     = visible[static_cast<size_t>(row)];
					const auto&    operation = inspector_frame.operations[index];
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					char id[32] {};
					std::snprintf(id, sizeof(id), "%u##operation%u", index, index);
					if (ImGui::Selectable(id, selected_operation == index,
					                      ImGuiSelectableFlags_SpanAllColumns)) {
						selected_operation = index;
					}
					ImGui::TableSetColumnIndex(1);
					if (operation.kind == InspectorOperationKind::Dispatch) {
						ImGui::Text("DISPATCH %u/%u", operation.occurrence + 1,
						            operation.occurrence_count);
					} else if (operation.kind == InspectorOperationKind::Draw) {
						ImGui::TextUnformatted("DRAW");
					} else {
						ImGui::TextUnformatted(InspectorOperationKindName(operation.kind));
					}
					ImGui::TableSetColumnIndex(2);
					if (operation.stages.empty()) {
						ImGui::TextDisabled("-");
					}
					for (size_t stage_index = 0; stage_index < operation.stages.size(); ++stage_index) {
						const auto& stage = operation.stages[stage_index];
						if (stage_index != 0) ImGui::SameLine();
						ImGui::Text("%s 0x%016" PRIx64, InspectorStageName(stage.stage),
						            stage.shader_hash);
					}
					ImGui::TableSetColumnIndex(3);
					if (operation.kind == InspectorOperationKind::Dispatch) {
						const uint64_t invocations =
						    static_cast<uint64_t>(operation.groups[0]) * operation.groups[1] *
						    operation.groups[2] * operation.local[0] * operation.local[1] *
						    operation.local[2];
						ImGui::Text("%ux%ux%u / %ux%ux%u inv=%" PRIu64 "%s", operation.groups[0],
						            operation.groups[1], operation.groups[2], operation.local[0],
						            operation.local[1], operation.local[2], invocations,
						            operation.indirect_address != 0 ? " indirect" : "");
					} else if (operation.kind == InspectorOperationKind::Draw) {
						ImGui::Text("indices=%u instances=%u", operation.index_count,
						            operation.instance_count);
					} else {
						ImGui::TextDisabled("copy/resolve");
					}
					ImGui::TableSetColumnIndex(4);
					size_t resources = operation.attachments.size();
					for (const auto& stage: operation.stages) resources += stage.resources.size();
					ImGui::Text("%zu", resources);
					if (address_filter != 0) {
						ImGui::TableSetColumnIndex(5);
						const auto match = InspectorMatchForOperation(
						    inspector_index, inspector_frame, index, address_filter, 1);
						ImGui::Text("%s%s", InspectorMatchKindName(match.kind),
						            match.alias ? " ALIAS" : "");
					}
				}
			}
			ImGui::EndTable();
		}

		if (selected_operation < inspector_frame.operations.size()) {
			const auto& selected = inspector_frame.operations[selected_operation];
			const auto selected_hash = selected.stages.empty() ? 0 : selected.stages.back().shader_hash;
			if (ImGui::Button("Filter to selected shader") && selected_hash != 0) {
				std::snprintf(inspector_filter.data(), inspector_filter.size(), "%016" PRIx64,
				              selected_hash);
			}
			ImGui::SameLine();
			if (ImGui::Button("Arm shader input capture")) {
				if (selected_hash == 0) {
					inspector_status = "selected operation has no shader hash";
				} else {
					char hash_text[32] {};
					char occurrence_text[16] {};
					std::snprintf(hash_text, sizeof(hash_text), "%016" PRIx64 "\n", selected_hash);
					std::snprintf(occurrence_text, sizeof(occurrence_text), "%u\n",
					              std::max(selected.occurrence_count, 1u));
					std::string error;
					if (WriteDiagnosticTrigger("D:/PS5/dumps/CAPTURE_HASHES", hash_text, &error) &&
					    WriteDiagnosticTrigger("D:/PS5/dumps/DUMP_INPUTS", occurrence_text, &error)) {
						inspector_status = "armed input capture for selected hash (all observed occurrences)";
					} else {
						inspector_status = "capture arm failed: " + error;
					}
				}
			}
			auto arm_selected = [&](InspectorCaptureMode mode) {
				InspectorCaptureArm arm;
				arm.mode           = mode;
				arm.kind           = selected.kind;
				arm.occurrence     = selected.occurrence;
				arm.expected_index = static_cast<int32_t>(selected_operation);
				for (const auto& stage: selected.stages) {
					if (arm.shader_hash_count < 4) {
						arm.shader_hashes[arm.shader_hash_count++] = stage.shader_hash;
					}
				}
				ArmInspectorCapture(arm);
				inspector_status =
				    "armed " + std::string(mode == InspectorCaptureMode::Inputs       ? "input"
				                           : mode == InspectorCaptureMode::Outputs    ? "output"
				                                                                      : "before+after") +
				    " capture for this operation. Hold is not enough - Step one frame so the "
				    "matching draw/dispatch runs. Capture inserts a GPU copy into the command "
				    "buffer and restores image layout; it does not Finish/wait. Output capture "
				    "ends the current Vulkan render pass.";
			};
			ImGui::SameLine();
			if (ImGui::Button("Capture Inputs")) {
				arm_selected(InspectorCaptureMode::Inputs);
			}
			ImGui::SameLine();
			if (ImGui::Button("Capture Outputs")) {
				arm_selected(InspectorCaptureMode::Outputs);
			}
			ImGui::SameLine();
			if (ImGui::Button("Capture Before + After")) {
				arm_selected(InspectorCaptureMode::BeforeAfter);
			}
			const auto armed = PeekInspectorCaptureArm();
			if (armed.mode != InspectorCaptureMode::None) {
				ImGui::SameLine();
				ImGui::TextColored({1.0f, 0.72f, 0.2f, 1.0f}, "CAPTURE ARMED");
			}

			if (ImGui::CollapsingHeader("Operation details", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Text("op=%u  %s  submit=%" PRIu64 " tick=%" PRIu64, selected_operation,
				            InspectorOperationKindName(selected.kind), selected.submit_id,
				            selected.submission_tick);
				ImGui::Text("Submitted from: %s", InspectorCallsiteText(selected.callsite).c_str());
				if (selected.callsite.guest_rip != 0) {
					ImGui::TextDisabled("rip=0x%016" PRIx64 "  base=0x%016" PRIx64 "  +0x%llX",
					                    selected.callsite.guest_rip, selected.callsite.module_base,
					                    static_cast<unsigned long long>(selected.callsite.module_offset));
				}
				if (selected.kind == InspectorOperationKind::Draw) {
					uint64_t vs = 0;
					uint64_t ps = 0;
					for (const auto& stage: selected.stages) {
						if (stage.stage == static_cast<uint32_t>(ShaderType::Vertex) ||
						    stage.stage == static_cast<uint32_t>(ShaderType::Mesh)) {
							vs = stage.shader_hash;
						}
						if (stage.stage == static_cast<uint32_t>(ShaderType::Pixel)) {
							ps = stage.shader_hash;
						}
					}
					ImGui::Text("VS 0x%016" PRIx64 "  PS 0x%016" PRIx64
					            "  indices=%u instances=%u",
					            vs, ps, selected.index_count, selected.instance_count);
					if (selected.has_viewport) {
						ImGui::Text("viewport=(%.1f,%.1f %.1fx%.1f) scissor=(%d,%d)-(%d,%d)",
						            selected.viewport[0], selected.viewport[1], selected.viewport[2],
						            selected.viewport[3], selected.scissor[0], selected.scissor[1],
						            selected.scissor[2], selected.scissor[3]);
					}
				} else if (selected.kind == InspectorOperationKind::Dispatch) {
					const uint64_t invocations =
					    static_cast<uint64_t>(selected.groups[0]) * selected.groups[1] *
					    selected.groups[2] * selected.local[0] * selected.local[1] *
					    selected.local[2];
					ImGui::Text("CS 0x%016" PRIx64 "  groups=%ux%ux%u local=%ux%ux%u invocations=%" PRIu64,
					            selected_hash, selected.groups[0], selected.groups[1],
					            selected.groups[2], selected.local[0], selected.local[1],
					            selected.local[2], invocations);
				}
			}

			InspectorResource selected_resource {};
			selected_resource.address          = selected_resource_address;
			selected_resource.size             = selected_resource_size;
			selected_resource.image_id         = selected_resource_image_id;
			selected_resource.image_generation = selected_resource_generation;
			const auto last_writer =
			    FindInspectorPreviousWriter(inspector_index, selected_resource, selected_operation);
			const auto next_reader =
			    FindInspectorNextReader(inspector_index, selected_resource, selected_operation);
			const auto next_writer =
			    FindInspectorNextWriter(inspector_index, selected_resource, selected_operation);
			auto describe_nav = [&](const char* label, int32_t index) {
				if (index < 0 || static_cast<uint32_t>(index) >= inspector_frame.operations.size()) {
					ImGui::Text("%s: none in this frame", label);
					return;
				}
				const auto& op = inspector_frame.operations[static_cast<uint32_t>(index)];
				ImGui::Text("%s: op %d  %s  hash=0x%016" PRIx64 " tick=%" PRIu64, label, index,
				            InspectorOperationKindName(op.kind),
				            op.stages.empty() ? 0 : op.stages.back().shader_hash, op.submission_tick);
			};
			if (selected_resource_address != 0 || selected_resource_image_id != 0) {
				if (ImGui::Button("< Previous Writer") && last_writer >= 0) {
					selected_operation  = static_cast<uint32_t>(last_writer);
					scroll_to_operation = selected_operation;
				}
				ImGui::SameLine();
				if (ImGui::Button("> Next Reader") && next_reader >= 0) {
					selected_operation  = static_cast<uint32_t>(next_reader);
					scroll_to_operation = selected_operation;
				}
				ImGui::SameLine();
				if (ImGui::Button(">> Next Writer") && next_writer >= 0) {
					selected_operation  = static_cast<uint32_t>(next_writer);
					scroll_to_operation = selected_operation;
				}
				describe_nav("Last writer", last_writer);
				describe_nav("Next reader", next_reader);
				describe_nav("Next writer", next_writer);
			}

			constexpr ImGuiTableFlags resource_flags =
			    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
			    ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
			uint32_t resource_row = 0;
			auto draw_resource = [&](const char* stage_name, const InspectorResource& resource) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(stage_name);
				ImGui::TableNextColumn();
				char resource_label[64] {};
				std::snprintf(resource_label, sizeof(resource_label), "%s[%u]##resource%u",
				              InspectorResourceName(resource.kind), resource.index, resource_row++);
				const bool resource_selected = (resource.address != 0 || resource.image_id != 0) &&
				                               resource.address == selected_resource_address &&
				                               resource.image_id == selected_resource_image_id &&
				                               resource.image_generation == selected_resource_generation;
				if (ImGui::Selectable(resource_label, resource_selected,
				                      ImGuiSelectableFlags_SpanAllColumns)) {
					selected_resource_address    = resource.address;
					selected_resource_size       = resource.size;
					selected_resource_image_id   = resource.image_id;
					selected_resource_generation = resource.image_generation;
				}
				ImGui::TableNextColumn();
				ImGui::Text("0x%016" PRIx64 " +%" PRIu64, resource.address, resource.size);
				ImGui::TableNextColumn();
				if (resource.image_id != 0) {
					ImGui::Text("img %u.%u", resource.image_id, resource.image_generation);
				} else if (resource.buffer_id != 0) {
					ImGui::Text("buf %u.%u", resource.buffer_id, resource.buffer_generation);
				} else {
					ImGui::TextUnformatted("-");
				}
				ImGui::TableNextColumn();
				const auto alias = InspectorAliasGroupForResource(inspector_index, resource);
				if (alias != 0) ImGui::Text("%u", alias);
				else ImGui::TextUnformatted("-");
				ImGui::TableNextColumn();
				if (resource.width != 0) {
					ImGui::Text("%ux%ux%u", resource.width, resource.height, resource.depth);
				} else {
					ImGui::TextUnformatted("-");
				}
				ImGui::TableNextColumn();
				ImGui::Text("%d / %d", resource.guest_format, resource.actual_vk);
				ImGui::TableNextColumn();
				if (resource.vk_handle != 0) {
					ImGui::Text("0x%016" PRIx64, resource.vk_handle);
				} else {
					ImGui::TextUnformatted("-");
				}
				ImGui::TableNextColumn();
				ImGui::Text("%s%s%s", resource.read ? "R" : "-", resource.written ? "W" : "-",
				            resource.atomic ? "A" : "-");
				ImGui::TableNextColumn();
				ImGui::Text("%" PRIu64, resource.size);
				ImGui::TableNextColumn();
				const auto writer = FindInspectorPreviousWriter(inspector_index, resource,
				                                                selected_operation);
				if (writer >= 0) ImGui::Text("%d", writer);
				else ImGui::TextUnformatted("-");
				ImGui::TableNextColumn();
				ImGui::Text("%" PRIu64, resource.last_access_tick);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(InspectorCoherencyName(resource));
				ImGui::TableNextColumn();
				bool captured = false;
				for (const auto& record: capture_records) {
					if (record.image_id == resource.image_id &&
					    record.image_generation == resource.image_generation && record.ready) {
						ImGui::Text("xxh3=0x%016" PRIx64, record.xxh3);
						captured = true;
						break;
					}
				}
				if (!captured) {
					if (ImGui::SmallButton("Capture")) {
						InspectorCaptureArm arm;
						arm.mode             = InspectorCaptureMode::Resource;
						arm.image_id         = resource.image_id;
						arm.image_generation = resource.image_generation;
						arm.address          = resource.address;
						ArmInspectorCapture(arm);
						inspector_status = "armed resource capture on next use. Step one frame. "
						                   "No silent GPU readback.";
					}
				}
			};
			auto resource_table = [&](const char* id, auto predicate) {
				if (ImGui::BeginTable(id, 14, resource_flags, {0.0f, 180.0f})) {
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableSetupColumn("Stage");
					ImGui::TableSetupColumn("Slot");
					ImGui::TableSetupColumn("Guest range");
					ImGui::TableSetupColumn("ID");
					ImGui::TableSetupColumn("Alias");
					ImGui::TableSetupColumn("Extent");
					ImGui::TableSetupColumn("guest/vk fmt");
					ImGui::TableSetupColumn("Vk handle");
					ImGui::TableSetupColumn("Access");
					ImGui::TableSetupColumn("Bytes");
					ImGui::TableSetupColumn("Last writer");
					ImGui::TableSetupColumn("Tick");
					ImGui::TableSetupColumn("State");
					ImGui::TableSetupColumn("Preview");
					ImGui::TableHeadersRow();
					for (const auto& stage: selected.stages) {
						for (const auto& resource: stage.resources) {
							if (predicate(resource)) {
								draw_resource(InspectorStageName(stage.stage), resource);
							}
						}
					}
					for (const auto& resource: selected.attachments) {
						if (predicate(resource)) {
							draw_resource("RT", resource);
						}
					}
					ImGui::EndTable();
				}
			};
			ImGui::SeparatorText("INPUTS");
			resource_table("##Inputs", [](const InspectorResource& resource) {
				return resource.read && !resource.written;
			});
			ImGui::SeparatorText("OUTPUTS / READ_WRITE");
			resource_table("##Outputs", [](const InspectorResource& resource) {
				return resource.written;
			});

			if (selected.kind == InspectorOperationKind::Dispatch && selected_hash != 0 &&
			    ImGui::CollapsingHeader("Repeated-dispatch comparison",
			                            ImGuiTreeNodeFlags_DefaultOpen)) {
				int32_t previous_index = -1;
				for (int32_t index = static_cast<int32_t>(selected_operation) - 1; index >= 0; --index) {
					const auto& candidate = inspector_frame.operations[static_cast<size_t>(index)];
					if (candidate.kind == InspectorOperationKind::Dispatch && !candidate.stages.empty() &&
					    candidate.stages.back().shader_hash == selected_hash) {
						previous_index = index;
						break;
					}
				}
				if (previous_index < 0) {
					ImGui::TextDisabled("No earlier dispatch with this shader in the captured frame.");
				} else {
					struct ComparedBinding {
						uint32_t stage = 0;
						const InspectorResource* resource = nullptr;
					};
					std::unordered_map<uint64_t, ComparedBinding> before;
					std::unordered_map<uint64_t, ComparedBinding> after;
					auto collect = [](const InspectorOperation& operation, auto& bindings) {
						for (const auto& stage: operation.stages) {
							for (const auto& resource: stage.resources) {
								const uint64_t key = (static_cast<uint64_t>(stage.stage) << 40) |
								                     (static_cast<uint64_t>(resource.kind) << 32) |
								                     resource.index;
								bindings[key] = {stage.stage, &resource};
							}
						}
					};
					collect(inspector_frame.operations[static_cast<size_t>(previous_index)], before);
					collect(selected, after);
					std::unordered_set<uint64_t> keys;
					for (const auto& [key, binding]: before) keys.insert(key);
					for (const auto& [key, binding]: after) keys.insert(key);
					auto differs = [](const InspectorResource* lhs, const InspectorResource* rhs) {
						return lhs == nullptr || rhs == nullptr || lhs->address != rhs->address ||
						       lhs->size != rhs->size || lhs->image_id != rhs->image_id ||
						       lhs->image_generation != rhs->image_generation ||
						       lhs->actual_vk != rhs->actual_vk || lhs->width != rhs->width ||
						       lhs->height != rhs->height || lhs->depth != rhs->depth ||
						       lhs->read != rhs->read || lhs->written != rhs->written ||
						       lhs->atomic != rhs->atomic;
					};
					size_t changed = 0;
					for (const auto key: keys) {
						const auto old = before.find(key);
						const auto now = after.find(key);
						changed += differs(old == before.end() ? nullptr : old->second.resource,
						                   now == after.end() ? nullptr : now->second.resource);
					}
					ImGui::Text("Compared with operation %d: %zu changed binding(s)", previous_index,
					            changed);
					if (changed != 0 && ImGui::BeginTable("##DispatchDiff", 4, resource_flags,
					                                       {0.0f, 150.0f})) {
						ImGui::TableSetupColumn("Binding");
						ImGui::TableSetupColumn("Previous address / image");
						ImGui::TableSetupColumn("Selected address / image");
						ImGui::TableSetupColumn("Selected access");
						ImGui::TableHeadersRow();
						for (const auto key: keys) {
							const auto old = before.find(key);
							const auto now = after.find(key);
							const auto* old_resource = old == before.end() ? nullptr : old->second.resource;
							const auto* now_resource = now == after.end() ? nullptr : now->second.resource;
							if (!differs(old_resource, now_resource)) continue;
							const auto& binding = now != after.end() ? now->second : old->second;
							const auto* resource = now_resource != nullptr ? now_resource : old_resource;
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::Text("%s %s[%u]", InspectorStageName(binding.stage),
							            InspectorResourceName(resource->kind), resource->index);
							auto draw_identity = [](const InspectorResource* item) {
								if (item == nullptr) ImGui::TextUnformatted("missing");
								else ImGui::Text("0x%016" PRIx64 " / %u.%u", item->address,
								                 item->image_id, item->image_generation);
							};
							ImGui::TableNextColumn();
							draw_identity(old_resource);
							ImGui::TableNextColumn();
							draw_identity(now_resource);
							ImGui::TableNextColumn();
							if (now_resource != nullptr) {
								ImGui::Text("%s%s%s", now_resource->read ? "R" : "-",
								            now_resource->written ? "W" : "-",
								            now_resource->atomic ? "A" : "-");
							} else {
								ImGui::TextUnformatted("removed");
							}
						}
						ImGui::EndTable();
					}
				}
			}

			if ((selected_resource_address != 0 || selected_resource_image_id != 0) &&
			    ImGui::CollapsingHeader("Selected resource timeline",
			                            ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::Text("address=0x%016" PRIx64 " bytes=%" PRIu64 " image=%u.%u",
				            selected_resource_address, selected_resource_size,
				            selected_resource_image_id, selected_resource_generation);
				const auto alias_group =
				    InspectorAliasGroupForResource(inspector_index, selected_resource);
				if (alias_group != 0) {
					ImGui::TextColored({1.0f, 0.45f, 0.1f, 1.0f},
					                   "This identity is in alias group %u.", alias_group);
				}
				if (ImGui::Button("Trace address")) {
					char address[32] {};
					std::snprintf(address, sizeof(address), "%016" PRIx64 "\n",
					              selected_resource_address);
					std::string error;
					inspector_status = WriteDiagnosticTrigger("D:/PS5/dumps/TRACE_ADDRS", address, &error)
					                       ? "TRACE_ADDRS now watches the selected address"
					                       : "trace request failed: " + error;
				}
				ImGui::SameLine();
				if (ImGui::Button("Clear images once at address")) {
					char address[32] {};
					std::snprintf(address, sizeof(address), "%016" PRIx64 "\n",
					              selected_resource_address);
					std::string error;
					inspector_status = WriteDiagnosticTrigger("D:/PS5/dumps/CLEAR_IMAGES", address, &error)
					                       ? "armed one-frame image clear for selected address"
					                       : "clear request failed: " + error;
				}

				std::vector<uint32_t> timeline;
				CollectInspectorResourceTimeline(inspector_index, selected_resource, &timeline);
				ImGui::Text("%zu producer/consumer use(s) for this identity in this frame",
				            timeline.size());
				if (ImGui::BeginTable("##ResourceTimeline", 7, resource_flags, {0.0f, 210.0f})) {
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableSetupColumn("Op");
					ImGui::TableSetupColumn("Stage / shader");
					ImGui::TableSetupColumn("Binding");
					ImGui::TableSetupColumn("Address");
					ImGui::TableSetupColumn("image_id");
					ImGui::TableSetupColumn("Access");
					ImGui::TableSetupColumn("Format / extent");
					ImGui::TableHeadersRow();
					for (const auto use_index: timeline) {
						const auto& use = inspector_index.uses[use_index];
						const auto& op  = inspector_frame.operations[use.operation];
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						char op_label[32] {};
						std::snprintf(op_label, sizeof(op_label), "%u##tl%u", use.operation, use_index);
						if (ImGui::Selectable(op_label, selected_operation == use.operation,
						                      ImGuiSelectableFlags_SpanAllColumns)) {
							selected_operation  = use.operation;
							scroll_to_operation = use.operation;
						}
						ImGui::TableNextColumn();
						uint64_t hash = 0;
						for (const auto& stage: op.stages) {
							if (stage.stage == use.stage) {
								hash = stage.shader_hash;
								break;
							}
						}
						if (hash != 0) {
							ImGui::Text("%s 0x%016" PRIx64, InspectorStageName(use.stage), hash);
						} else {
							ImGui::TextUnformatted(use.attachment ? "RT"
							                                      : InspectorStageName(use.stage));
						}
						ImGui::TableNextColumn();
						ImGui::Text("%s[%u]", InspectorResourceName(use.kind), use.slot);
						ImGui::TableNextColumn();
						ImGui::Text("0x%016" PRIx64 " +%" PRIu64, use.address, use.size);
						ImGui::TableNextColumn();
						ImGui::Text("%u.%u%s", use.image_id, use.image_generation,
						            use.alias ? " ALIAS" : "");
						ImGui::TableNextColumn();
						ImGui::Text("%s%s%s", use.read ? "R" : "-", use.written ? "W" : "-",
						            use.atomic ? "A" : "-");
						ImGui::TableNextColumn();
						ImGui::Text("%s", InspectorOperationKindName(op.kind));
					}
					ImGui::EndTable();
				}
			}
		} else {
			ImGui::TextDisabled("Select an operation to inspect every resolved binding.");
			ImGui::TextDisabled("Headless snapshot: create D:/PS5/dumps/DUMP_INSPECTOR (optional hash contents).");
		}

		if (ImGui::CollapsingHeader("Shader probe / capture controls")) {
			namespace Probe = ShaderRecompiler::Frontend;
			auto load_probe_editor = [&] {
				const auto text = Probe::GetProbeConfigText();
				inspector_probe_text.fill('\0');
				std::memcpy(inspector_probe_text.data(), text.data(),
				            std::min(text.size(), inspector_probe_text.size() - 1));
				inspector_probe_initialized = true;
			};
			if (!inspector_probe_initialized) load_probe_editor();
			const auto config = Probe::GetProbeConfig();
			const auto generation = ShaderRecompiler::ProbeConfigGeneration();
			ImGui::Text("PROBE: %s", Probe::GetProbeConfigFilePath());
			ImGui::Text("active generation=%u hash=0x%016" PRIx64 " pc=0x%x taps=%zu",
			            generation, config->hash, config->store_pc, config->taps.size());
			if (config->hash != 0) {
				const auto translated_generation = InspectorShaderProbeGeneration(config->hash);
				if (translated_generation == generation && generation != 0) {
					ImGui::TextColored({0.3f, 1.0f, 0.4f, 1.0f},
					                   "target shader translated for generation %u", generation);
				} else {
					ImGui::TextColored({1.0f, 0.72f, 0.2f, 1.0f},
					                   "waiting for target dispatch/retranslation (compiled generation %u)",
					                   translated_generation);
				}
			}
			ImGui::InputTextMultiline("##ProbeEditor", inspector_probe_text.data(),
			                          inspector_probe_text.size(), {0.0f, 145.0f},
			                          ImGuiInputTextFlags_AllowTabInput);
			if (ImGui::Button("Apply PROBE file")) {
				std::string error;
				if (Probe::WriteProbeConfigText(inspector_probe_text.data(), &error)) {
					inspector_status = "PROBE written without BOM; waiting for reload and target dispatch";
				} else {
					inspector_status = "PROBE write failed: " + error;
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Reload editor")) {
				load_probe_editor();
				inspector_status = "reloaded editor from the authoritative PROBE state";
			}
			ImGui::SameLine();
			if (ImGui::Button("Remove PROBE (revert to env)")) {
				std::string error;
				if (Probe::RemoveProbeConfigFile(&error)) {
					inspector_probe_initialized = false;
					inspector_status = "PROBE removed; waiting for reload to restore environment settings";
				} else {
					inspector_status = "PROBE remove failed: " + error;
				}
			}
			ImGui::TextDisabled(
			    "Keys: hash, pc, vgpr, tap, mark, markx, execlo, execz, loop_header, "
			    "loop_iterations, gds_limit, group_cap, sync, execute_once, execute_count.");
			ImGui::TextDisabled("File/env state remains authoritative; execute_count defaults to 1.");
		}
		ImGui::End();
	}

	bool PrepareFrame(vk::Extent2D frame_extent, vk::Format format, uint32_t image_count) {
		OverlaySnapshot snapshot;
		const bool      system_visible = GetOverlaySnapshot(&snapshot);
		const bool      debug_visible  = g_debug_visible.load(std::memory_order_acquire);
		if (!system_visible && !debug_visible) {
			return false;
		}
		const auto prepared_session = system_visible ? snapshot.session : DebugSession();
		EnsureVulkan(format, image_count);
		if (session != prepared_session) {
			session       = prepared_session;
			focus_pending = true;
			shift         = !system_visible ||
			                (snapshot.ime.option & Ime::OPTION_NO_AUTO_CAPITALIZE) == 0;
			symbol_mode   = false;
			panel_offset  = {};
			right_stick   = {};
			auto& io      = ImGui::GetIO();
			io.ClearEventsQueue();
			io.ClearInputKeys();
			io.ClearInputMouse();
		}
		DrainInput(prepared_session);

		auto& io       = ImGui::GetIO();
		io.DisplaySize = {static_cast<float>(frame_extent.width),
		                  static_cast<float>(frame_extent.height)};
		const auto now = std::chrono::steady_clock::now();
		io.DeltaTime   = last_frame == std::chrono::steady_clock::time_point {}
		                     ? 1.0f / 60.0f
		                     : std::clamp(std::chrono::duration<float>(now - last_frame).count(),
		                                  1.0f / 1000.0f, 0.1f);
		last_frame     = now;
		ImGui_ImplVulkan_NewFrame();
		ImGui::NewFrame();
		if (debug_visible) {
			DrawDebugPanel(frame_extent);
		}
		OverlaySnapshot current;
		if (system_visible && GetOverlaySnapshot(&current) && current.session == prepared_session) {
			if (current.session.kind == OverlayKind::Error) {
				DrawError(current.error, frame_extent);
			} else {
				DrawIme(current.ime, frame_extent);
			}
		}
		ImGui::Render();
		extent = frame_extent;
		return true;
	}

	void Record(vk::CommandBuffer command, vk::ImageView target) {
		vk::RenderingAttachmentInfo color {};
		color.sType       = vk::StructureType::eRenderingAttachmentInfo;
		color.imageView   = target;
		color.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
		color.loadOp      = vk::AttachmentLoadOp::eLoad;
		color.storeOp     = vk::AttachmentStoreOp::eStore;
		vk::RenderingInfo rendering {};
		rendering.sType                = vk::StructureType::eRenderingInfo;
		rendering.renderArea.extent    = extent;
		rendering.layerCount           = 1;
		rendering.colorAttachmentCount = 1;
		rendering.pColorAttachments    = &color;
		command.beginRendering(rendering);
		{
			Common::LockGuard queue_lock(graphics.queue_mutex);
			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
			                                static_cast<VkCommandBuffer>(command));
		}
		command.endRendering();
	}

	void ReleaseVulkan() {
		if (!vulkan_initialized) {
			return;
		}
		ImGui::SetCurrentContext(imgui_context);
		ImGui_ImplVulkan_Shutdown();
		vulkan_initialized = false;
	}

	GraphicContext&                       graphics;
	ImGuiContext*                         imgui_context      = nullptr;
	bool                                  vulkan_initialized = false;
	bool                                  shift              = false;
	bool                                  symbol_mode        = false;
	bool                                  focus_pending      = true;
	float                                 ui_scale           = 1.0f;
	float                                 button_height      = 42.0f;
	ImVec2                                panel_offset {};
	ImVec2                                right_stick {};
	OverlaySession                        session;
	vk::Extent2D                          extent {};
	std::chrono::steady_clock::time_point last_frame;
	InspectorFrame                        inspector_frame;
	InspectorFrameIndex                   inspector_index;
	std::array<char, 32>                  inspector_filter {};
	std::array<char, 32>                  inspector_address_filter {};
	std::array<char, 4096>                inspector_probe_text {};
	std::string                           inspector_status;
	uint64_t                              inspector_waiting_frame = 0;
	uint64_t                              selected_resource_address = 0;
	uint64_t                              selected_resource_size = 0;
	uint32_t                              selected_resource_image_id = 0;
	uint32_t                              selected_resource_generation = 0;
	uint32_t                              selected_operation = UINT32_MAX;
	uint32_t                              scroll_to_operation = UINT32_MAX;
	uint32_t                              selected_alias_group = 0;
	uint32_t                              selected_alias_repr = 0;
	bool                                  inspector_probe_initialized = false;
	bool                                  inspector_live = true;
	bool                                  inspector_show_dispatches = true;
	bool                                  inspector_show_draws = true;
	bool                                  inspector_show_copies = true;
	bool                                  inspector_show_alias = false;
};

SystemOverlay::SystemOverlay(GraphicContext& graphics): m_impl(std::make_unique<Impl>(graphics)) {}

SystemOverlay::~SystemOverlay() = default;

bool SystemOverlay::PrepareFrame(vk::Extent2D extent, vk::Format format, uint32_t image_count) {
	return m_impl->PrepareFrame(extent, format, image_count);
}

void SystemOverlay::Record(vk::CommandBuffer command, vk::ImageView target) {
	m_impl->Record(command, target);
}

void SystemOverlay::ReleaseVulkan() {
	m_impl->ReleaseVulkan();
}

} // namespace Libs::Graphics
