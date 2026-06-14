#ifndef SEEDSIGNER_RUNNER_CORE_H
#define SEEDSIGNER_RUNNER_CORE_H

// runner_core — platform-agnostic LVGL plumbing shared by the desktop
// screen_runner (SDL2) and the browser web_runner (Emscripten/WASM).
//
// This layer owns everything that is identical across both hosts:
//   - the LVGL display + RGB565 framebuffer (and resolution switching),
//   - the keypad / pointer input devices and their read callbacks,
//   - the screen-function registry and JSON-driven screen invocation,
//   - parsing tools/scenarios/scenarios.json into per-screen, pre-merged scenarios.
//
// It deliberately has NO SDL dependency so it stays portable and headlessly
// testable. The two genuinely SDL-specific helpers (SDL keycode mapping and
// framebuffer→SDL_Texture blit) live in runner_sdl.h, compiled by both SDL
// hosts. Host-specific chrome (the native sidebar/badges, the browser's HTML
// controls + JSON editor) stays in each host's own translation unit.
//
// The result callbacks (seedsigner_lvgl_on_button_selected / _on_text_entered /
// _on_aux_key) are provided by the HOST, not by runner_core: the native runner
// renders them into its status bar, the web runner forwards them to JavaScript.

#include <cstdint>
#include <string>
#include <vector>

#include "lvgl.h"

namespace runner_core {

// ---------------------------------------------------------------------------
// Lifecycle / display
// ---------------------------------------------------------------------------

// One-time init: lv_init(), select the display profile for (width,height),
// allocate the framebuffer + draw buffer, create the LVGL display, and create
// the keypad + pointer input devices. Call exactly once at startup.
void init(int width, int height);

// Tear down the current LVGL display (which deletes its screens) and recreate
// it at a new resolution, resizing the framebuffers and reassigning the input
// devices. The caller is responsible for re-invoking the current screen after.
void resize(int width, int height);

int width();
int height();

// The RGB565 framebuffer, width()*height() entries, row-major. Valid after the
// first tick(); the host blits this to its display surface each frame.
const uint16_t* framebuffer();

// Advance LVGL time by elapsed_ms and run the timer handler once (this is what
// actually renders dirty regions into the framebuffer).
void tick(uint32_t elapsed_ms);

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// Queue a single LVGL key, delivered on the next keypad indev read. Accepts the
// LV_KEY_* navigation codes and the aux-key char codes '1'/'2'/'3'. Only honored
// while the active input mode is INPUT_MODE_HARDWARE.
void push_key(uint32_t lv_key);

// Set the pointer state in VIEWPORT-LOCAL coordinates (0,0 == top-left of the
// rendered screen, not the host window). Only honored while the active input
// mode is INPUT_MODE_TOUCH. Hosts translate their own window/canvas coordinates
// into viewport-local before calling this.
void set_pointer(int x, int y, bool pressed);

// Scroll the active screen's scrollable body by dy_px (positive reveals content
// above / scrolls up; negative reveals content below). A no-op if nothing on the
// screen is scrollable or already at the limit. Lets hosts wire mouse-wheel /
// two-finger scroll to long lists in addition to LVGL's native drag-to-scroll.
void scroll_active(int dy_px);

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

// True if fn_name is a known screen entry point.
bool has_screen(const std::string& fn_name);

// Invoke screen fn_name with the given JSON context string (pass an empty
// string for screens that take no context). Returns false if fn_name is
// unknown. The screen functions parse JSON internally and may THROW on
// malformed input — callers that accept untrusted JSON (the live editor) must
// wrap this in try/catch.
bool load_screen(const std::string& fn_name, const std::string& json_ctx);

// ---------------------------------------------------------------------------
// Scenarios (tools/scenarios/scenarios.json)
// ---------------------------------------------------------------------------

struct Scenario {
    std::string name;          // "(default)" for the base, else the variation name
    std::string context_json;  // fully merged context, serialized (may be "{}" / empty)
};

struct ScreenScenarios {
    std::string screen;                 // screen function name
    std::vector<Scenario> scenarios;    // base first, then each variation in file order
};

// Parse scenarios.json, expanding base context + variations (RFC 7396
// merge_patch) into concrete per-variation contexts, grouped by screen and
// preserving file order. Returns false on missing file or malformed JSON.
bool load_scenarios_grouped(const char* path, std::vector<ScreenScenarios>& out);

}  // namespace runner_core

#endif  // SEEDSIGNER_RUNNER_CORE_H
