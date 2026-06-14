// web_runner — Emscripten/WASM host for the SeedSigner LVGL screens.
//
// Renders ONLY the device viewport to the page's <canvas>; all chrome (screen
// picker, scenario list, live JSON editor, on-screen joystick, results log)
// lives in shell.html and drives this module through the exported C API below.
//
// Display + input + screen plumbing is shared with the native screen_runner via
// tools/apps/runner_core/runner_core (SDL-free) and tools/apps/runner_core/runner_sdl (the two SDL
// helpers). Emscripten provides SDL2 (-sUSE_SDL=2), so the SDL window is the
// browser canvas and keyboard/mouse/touch events arrive as ordinary SDL events.

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#endif

#include <emscripten.h>
#include <emscripten/html5.h>

#include "lvgl.h"
#include "seedsigner.h"
#include "input_profile.h"

#include "runner_core.h"
#include "runner_sdl.h"

#include <string>

#ifndef DISPLAY_WIDTH
#error "DISPLAY_WIDTH must be defined by the build system"
#endif
#ifndef DISPLAY_HEIGHT
#error "DISPLAY_HEIGHT must be defined by the build system"
#endif

// ---------------------------------------------------------------------------
// SDL display state
// ---------------------------------------------------------------------------

static SDL_Window*   g_window   = nullptr;
static SDL_Renderer* g_renderer = nullptr;
static SDL_Texture*  g_texture  = nullptr;
static uint32_t      g_last_tick_ms = 0;

// Remember the last loaded screen so a resolution change can re-render it.
static std::string g_cur_screen;
static std::string g_cur_json;

// ---------------------------------------------------------------------------
// Result callbacks → JavaScript
//
// The screens call these weak hooks; we forward each event to the global JS
// function window.ssOnResult({...}) defined in shell.html. UTF8ToString is on
// the exported runtime methods so the string args marshal correctly.
// ---------------------------------------------------------------------------

extern "C" void seedsigner_lvgl_on_button_selected(uint32_t index, const char* label) {
    const char* kind = "body";
    if (index == SEEDSIGNER_RET_BACK_BUTTON || index == SEEDSIGNER_RET_POWER_BUTTON ||
        index == SEEDSIGNER_RET_SCREENSAVER_DISMISS) {
        kind = "reserved";
    }
    EM_ASM({
        if (window.ssOnResult) {
            window.ssOnResult({ type: 'button', kind: UTF8ToString($0),
                                index: $1, label: UTF8ToString($2) });
        }
    }, kind, (int)index, label ? label : "");
}

extern "C" void seedsigner_lvgl_on_text_entered(const char* text) {
    EM_ASM({
        if (window.ssOnResult) {
            window.ssOnResult({ type: 'text', text: UTF8ToString($0) });
        }
    }, text ? text : "");
}

extern "C" void seedsigner_lvgl_on_aux_key(const char* key_name) {
    EM_ASM({
        if (window.ssOnResult) {
            window.ssOnResult({ type: 'aux', key: UTF8ToString($0) });
        }
    }, key_name ? key_name : "");
}

// ---------------------------------------------------------------------------
// SDL surface (re)creation
// ---------------------------------------------------------------------------

static void create_sdl_surface(int w, int h) {
    if (!g_window) {
        g_window = SDL_CreateWindow("seedsigner-web", SDL_WINDOWPOS_UNDEFINED,
                                    SDL_WINDOWPOS_UNDEFINED, w, h, SDL_WINDOW_SHOWN);
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    } else {
        SDL_SetWindowSize(g_window, w, h);
    }
    SDL_RenderSetLogicalSize(g_renderer, w, h);

    if (g_texture) SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
}

// ---------------------------------------------------------------------------
// Exported C API (called from JavaScript via Module.ccall)
// ---------------------------------------------------------------------------

extern "C" {

// Load screen `fn_name` with JSON context `json`. Returns 1 on success, 0 if the
// screen name is unknown, -1 if the JSON was rejected (the screen threw while
// parsing) — in which case the previous render is left untouched.
EMSCRIPTEN_KEEPALIVE int ss_load_screen(const char* fn_name, const char* json) {
    std::string name = fn_name ? fn_name : "";
    std::string ctx = json ? json : "";
    if (!runner_core::has_screen(name)) return 0;
    try {
        runner_core::load_screen(name, ctx);
    } catch (...) {
        // Malformed JSON (or any screen-build error): keep the last good screen.
        return -1;
    }
    g_cur_screen = name;
    g_cur_json = ctx;
    runner_core::tick(0);  // render immediately so the canvas updates this frame
    return 1;
}

// Switch device resolution and re-render the current screen at the new size.
EMSCRIPTEN_KEEPALIVE void ss_set_resolution(int w, int h) {
    runner_core::resize(w, h);
    create_sdl_surface(w, h);
    // Tell the page the canvas backing size changed so it can re-fit CSS.
    EM_ASM({ if (window.ssOnResize) window.ssOnResize($0, $1); }, w, h);
    if (!g_cur_screen.empty()) {
        try { runner_core::load_screen(g_cur_screen, g_cur_json); } catch (...) {}
    }
    runner_core::tick(0);
}

// 0 = touch (pointer), 1 = hardware (keypad / joystick).
EMSCRIPTEN_KEEPALIVE void ss_set_input_mode(int mode) {
    input_profile_set_mode(mode == 1 ? INPUT_MODE_HARDWARE : INPUT_MODE_TOUCH);
    // Re-render so mode-dependent focus highlighting updates immediately.
    if (!g_cur_screen.empty()) {
        try { runner_core::load_screen(g_cur_screen, g_cur_json); } catch (...) {}
    }
}

// Inject a one-shot LVGL key (LV_KEY_* nav code, or aux char '1'/'2'/'3').
// Used by the on-screen D-pad / center / side buttons in the HTML chrome.
EMSCRIPTEN_KEEPALIVE void ss_send_key(int lv_key) {
    runner_core::push_key((uint32_t)lv_key);
}

// Scroll the active screen's scrollable list by dy pixels (positive scrolls up).
// Wired to mouse-wheel / two-finger scroll in the page.
EMSCRIPTEN_KEEPALIVE void ss_scroll(int dy) {
    runner_core::scroll_active(dy);
}

EMSCRIPTEN_KEEPALIVE int ss_get_width()  { return runner_core::width(); }
EMSCRIPTEN_KEEPALIVE int ss_get_height() { return runner_core::height(); }

}  // extern "C"

// ---------------------------------------------------------------------------
// Main loop (cooperative — Emscripten drives it via requestAnimationFrame)
// ---------------------------------------------------------------------------

static void frame() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_MOUSEMOTION:
                runner_core::set_pointer(ev.motion.x, ev.motion.y,
                                         (ev.motion.state & SDL_BUTTON_LMASK) != 0);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    runner_core::set_pointer(ev.button.x, ev.button.y, true);
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    runner_core::set_pointer(ev.button.x, ev.button.y, false);
                break;
            case SDL_KEYDOWN: {
                uint32_t k = runner_sdl::map_sdl_keycode(ev.key.keysym.sym);
                if (k != 0) runner_core::push_key(k);
                break;
            }
            default:
                break;
        }
    }

    uint32_t now = SDL_GetTicks();
    runner_core::tick(now - g_last_tick_ms);
    g_last_tick_ms = now;

    runner_sdl::blit_framebuffer_to_texture(g_texture);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
}

int main() {
    // Scope SDL's keyboard capture to the canvas element only. Without this,
    // Emscripten's SDL port listens on the whole document and preventDefault()s
    // keydowns, which would steal typing from the page's JSON editor / inputs.
    // With "#canvas", keys only reach LVGL when the canvas has focus.
    SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas");

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

    runner_core::init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    create_sdl_surface(DISPLAY_WIDTH, DISPLAY_HEIGHT);

    // Default to hardware (keypad) mode; the page switches to touch on touch
    // devices and via the input-mode toggle.
    input_profile_set_mode(INPUT_MODE_HARDWARE);

    g_last_tick_ms = SDL_GetTicks();

    // Let the page know the runtime is ready and the backing canvas size.
    EM_ASM({ if (window.ssOnReady) window.ssOnReady($0, $1); },
           runner_core::width(), runner_core::height());

    emscripten_set_main_loop(frame, 0, 1);
    return 0;
}
