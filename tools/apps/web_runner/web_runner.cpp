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
#include "locale_loader.h"
#include "locale_picker.h"   // locale_picker_set_image_provider (endonym images)

#include "runner_core.h"
#include "runner_sdl.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

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

// Option B for the browser's async I/O: JavaScript fetches every pack blob a
// locale needs (ss_pack_files), stages each here keyed by filename (ss_stage_blob),
// then calls ss_load_staged once. The shared loader (ss_load_locale) reads blobs
// back through staged_pack_provider — so the browser runs the EXACT same
// orchestration as every other host. Cleared after each load (the loader keeps its
// own copies of the buffers it registers).
static std::map<std::string, std::vector<uint8_t>> g_staged_blobs;

// Endonym images for the locale picker (locale_picker_screen). Unlike the font
// staging above — one active locale at a time, filenames unique within it — the
// picker needs MANY locales' endonym images resident at once, and they share a
// filename (every pack has an "endonym_<height>.bin"). So this staging is keyed by
// "<locale>/<file>", and JS stages one per image row before rendering the picker.
// The picker copies each blob into its own A8 buffer at attach time, so these need
// only outlive the render call; kept cached (tiny) rather than cleared per render.
static std::map<std::string, std::vector<uint8_t>> g_endonym_blobs;

static bool endonym_provider(const char* locale, const char* file,
                             const uint8_t** bytes, size_t* len, void* user) {
    auto* staged = static_cast<std::map<std::string, std::vector<uint8_t>>*>(user);
    std::string key = std::string(locale ? locale : "") + "/" + (file ? file : "");
    auto it = staged->find(key);
    if (it == staged->end()) return false;
    *bytes = it->second.data();
    *len   = it->second.size();
    return true;
}

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
        index == SEEDSIGNER_RET_SCREENSAVER_DISMISS || index == SEEDSIGNER_RET_SPLASH_COMPLETE) {
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

// Switch device resolution. Deliberately does NOT reload/redraw the screen:
// resize() rebuilt the active profile with the BAKED baseline fonts, so redrawing
// now would flash a pack locale's text (Greek/Cyrillic/…) as .notdef boxes for one
// frame until the locale's fonts are re-registered at the new px. The JS host
// re-registers the fonts (from its byte cache, synchronously) and reloads the
// screen right after this returns — before the next animation frame — so the first
// frame at the new resolution already has the correct glyphs.
EMSCRIPTEN_KEEPALIVE void ss_set_resolution(int w, int h) {
    runner_core::resize(w, h);
    create_sdl_surface(w, h);
    // Tell the page the canvas backing size changed so it can re-fit CSS.
    EM_ASM({ if (window.ssOnResize) window.ssOnResize($0, $1); }, w, h);
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

// ---- Locale packs (option B: JS pre-fetches, the shared loader orchestrates) --
// Packs are NOT baked into the bundle: JavaScript fetches each per-locale file
// (.ttf packs + runs.bin for complex scripts) at runtime, so a font/translation
// change is just a file swap (no WASM rebuild). Browser I/O is async but the shared
// loader is synchronous, so JS stages every blob first, then drives the loader once
// — which keeps the clear-old + register-new burst atomic (no frame redraws with a
// freed font or the baked baseline mid-switch).

// The list of files JS must fetch + stage for `locale` ("["ur.ttf","runs.bin"]",
// or "[]" for a baked-floor locale). Comes from the render layer's manifest via
// the shared loader, so the web host never duplicates that policy.
EMSCRIPTEN_KEEPALIVE const char* ss_pack_files(const char* locale) {
    return ss_locale_pack_files(locale);
}

// Stage one fetched pack blob under its filename ("ur.ttf", "runs.bin", ...). The
// bytes are COPIED into a buffer this module owns, so the caller may free its
// WASM-heap copy immediately. Call once per file from ss_pack_files, THEN
// ss_load_staged.
EMSCRIPTEN_KEEPALIVE void ss_stage_blob(const char* file, const uint8_t* data, int len) {
    if (!file || !data || len <= 0) return;
    g_staged_blobs[file] = std::vector<uint8_t>(data, data + len);
}

// Stage one endonym image for the picker, keyed by "<locale>/<file>" (see
// g_endonym_blobs). JS calls this once per image row before ss_load_screen(
// "locale_picker_screen", ...); the picker then fetches them via endonym_provider.
EMSCRIPTEN_KEEPALIVE void ss_stage_endonym(const char* locale, const char* file,
                                           const uint8_t* data, int len) {
    if (!locale || !file || !data || len <= 0) return;
    g_endonym_blobs[std::string(locale) + "/" + file] =
        std::vector<uint8_t>(data, data + len);
}

// Provider that feeds the shared loader from the staged blobs, keyed by filename
// (the loader requests exactly the files ss_pack_files listed).
static bool staged_pack_provider(const char* /*locale*/, const char* file,
                                 const uint8_t** bytes, size_t* len, void* user) {
    auto* staged = static_cast<std::map<std::string, std::vector<uint8_t>>*>(user);
    auto it = staged->find(file ? file : "");
    if (it == staged->end()) return false;
    *bytes = it->second.data();
    *len   = it->second.size();
    return true;
}

// Switch to `locale` using the staged blobs: runs the shared loader (clears the
// previous locale's fonts + glyph runs, then registers this locale's fonts and
// installs its glyph-run table), then drops the staging area (the loader kept its
// own copies of what it registered). Returns 1 on success, else 0. An empty /
// baked-floor locale needs no staged blobs.
EMSCRIPTEN_KEEPALIVE int ss_load_staged(const char* locale) {
    bool ok = ss_load_locale(locale ? locale : "", staged_pack_provider, &g_staged_blobs);
    g_staged_blobs.clear();
    return ok ? 1 : 0;
}

// Re-render the current screen with context `json` (the locale's translated
// scenario text). Equivalent to ss_load_screen for the current screen, but named
// for the "fonts changed, redraw" intent. Returns the ss_load_screen code.
EMSCRIPTEN_KEEPALIVE int ss_reload(const char* json) {
    if (g_cur_screen.empty()) return 0;
    return ss_load_screen(g_cur_screen.c_str(), json);
}

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

    // The locale picker fetches endonym images through the same provider seam as
    // the font loader; back it with the JS-staged, locale-keyed blob store.
    locale_picker_set_image_provider(endonym_provider, &g_endonym_blobs);

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
