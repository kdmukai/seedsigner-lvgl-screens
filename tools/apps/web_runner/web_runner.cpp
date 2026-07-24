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

// Presentation orientation. The window/canvas is ALWAYS the landscape panel (g_land_*).
// For portrait-authored screens (camera_preview_pillarboxed) g_present_rotate flips on:
// LVGL renders the panel's native PORTRAIT (canvas swapped to g_land_h x g_land_w, profile
// unchanged) and we present it rotated 90° CW into the landscape canvas — mirroring the
// screenshot generator's physical-mount simulation. Pointer coords are inverse-rotated to
// match. All other screens render landscape 1:1 (g_present_rotate = false).
static int  g_land_w = DISPLAY_WIDTH;
static int  g_land_h = DISPLAY_HEIGHT;
static bool g_present_rotate = false;

// Degrees clockwise to rotate the portrait render onto the landscape canvas. Matches the
// screenshot generator's portrait->landscape mapping (back button -> top-left, text upright).
static const double PRESENT_ROTATE_CW_DEG = 90.0;

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

// Endonym images for the locale picker (settings_locale_picker_screen). Unlike the font
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

// Window/canvas is win_w x win_h (the landscape panel); the streaming texture is tex_w x
// tex_h (the LVGL framebuffer — portrait when presenting rotated). They differ only in the
// rotated case; the present step (frame()) bridges them.
static void create_sdl_surface(int win_w, int win_h, int tex_w, int tex_h) {
    if (!g_window) {
        g_window = SDL_CreateWindow("seedsigner-web", SDL_WINDOWPOS_UNDEFINED,
                                    SDL_WINDOWPOS_UNDEFINED, win_w, win_h, SDL_WINDOW_SHOWN);
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
    } else {
        SDL_SetWindowSize(g_window, win_w, win_h);
    }
    SDL_RenderSetLogicalSize(g_renderer, win_w, win_h);

    if (g_texture) SDL_DestroyTexture(g_texture);
    g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h);
}

// Feed a pointer event to LVGL, inverse-rotating the landscape canvas coords into the
// portrait LVGL frame when presenting rotated (the inverse of the 90° CW present:
// portrait (px,py) = (my, land_w-1-mx)). 1:1 otherwise.
static void set_pointer_mapped(int mx, int my, bool pressed) {
    if (g_present_rotate) {
        runner_core::set_pointer(my, (g_land_w - 1) - mx, pressed);
    } else {
        runner_core::set_pointer(mx, my, pressed);
    }
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
    g_land_w = w;
    g_land_h = h;
    g_present_rotate = false;   // a resolution change resets to landscape; the page re-applies
                                // rotation per screen (ss_set_present_rotation) before reload.
    runner_core::resize(w, h);
    create_sdl_surface(w, h, w, h);
    // Tell the page the canvas backing size changed so it can re-fit CSS.
    EM_ASM({ if (window.ssOnResize) window.ssOnResize($0, $1); }, w, h);
}

// Toggle rotated presentation for portrait-authored screens (camera_preview_pillarboxed).
// rotate != 0: recreate the LVGL canvas in the panel's native portrait (swapped dims, same
// profile) so the chrome lays out correctly, and present it 90° CW into the landscape canvas.
// rotate == 0: back to a 1:1 landscape canvas. Re-renders the current screen at the new
// orientation. The page calls this per screen (from the scenario's render_as_landscape flag)
// right before ss_load_screen.
EMSCRIPTEN_KEEPALIVE void ss_set_present_rotation(int rotate) {
    bool want = rotate != 0;
    if (want == g_present_rotate) return;
    g_present_rotate = want;

    if (want) {
        // Native portrait canvas (short x long); profile stays the physical landscape panel.
        runner_core::resize_oriented(g_land_h, g_land_w, g_land_w, g_land_h);
        create_sdl_surface(g_land_w, g_land_h, runner_core::width(), runner_core::height());
    } else {
        runner_core::resize(g_land_w, g_land_h);
        create_sdl_surface(g_land_w, g_land_h, g_land_w, g_land_h);
    }

    // resize_oriented()/resize() deleted the screen; re-render the current one (never blank).
    if (!g_cur_screen.empty()) {
        try { runner_core::load_screen(g_cur_screen, g_cur_json); } catch (...) {}
        runner_core::tick(0);
    }
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

// The RGB565 framebuffer as a byte offset into the WASM heap (Emscripten marshals
// the pointer as a plain number). width()*height() uint16_t entries, row-major — JS
// reads them straight from HEAPU16 and expands RGB565 -> RGBA to paint an offscreen
// 2D canvas. This is the shared capture primitive behind the runner's "download PNG"
// button and the web gallery's grid: NEVER canvas.toBlob() the live WebGL canvas
// (returns black without preserveDrawingBuffer). Valid after the first tick(); since
// ss_load_screen ticks synchronously, the buffer holds the just-loaded screen when
// this is called right after it. The pixel plane is always the LVGL canvas' NATIVE
// orientation (portrait for the rotated-present screens) — the caller rotates in JS,
// mirroring the SDL present in frame().
EMSCRIPTEN_KEEPALIVE const uint16_t* ss_get_framebuffer() { return runner_core::framebuffer(); }

// Settle the current screen to a fully-painted frame SYNCHRONOUSLY. ss_load_screen ticks
// once, which lays out and starts the render — but at larger resolutions, or for screens
// with a load transition, one pass hasn't flushed the whole framebuffer yet (the interactive
// runner never notices because its rAF present loop finishes the paint over the next frames).
// The gallery and the download button read the framebuffer directly with no such loop, so
// they call this first: a few extra timer-handler passes advance a couple of frames' worth
// of time and complete the paint. Cheap, and a no-op in effect for already-settled screens.
EMSCRIPTEN_KEEPALIVE void ss_render_now() {
    for (int i = 0; i < 3; ++i) runner_core::tick(16);
}

// ---- Locale packs (option B: JS pre-fetches, the shared loader orchestrates) --
// Packs are NOT baked into the bundle: JavaScript fetches each per-locale file
// (.ttf packs + runs.bin for complex scripts) at runtime, so a font/translation
// change is just a file swap (no WASM rebuild). Browser I/O is async but the shared
// loader is synchronous, so JS stages every blob first, then drives the loader once
// — which keeps the clear-old + register-new burst atomic (no frame redraws with a
// freed font or the baked baseline mid-switch).

// Register a language pack from its manifest.json so the render layer learns the
// locale's policy (chain / rtl / shaping / role sizes). Screens bakes NO locale table,
// so JS must call this once per available pack (fetch assets/lang-packs/<loc>/manifest.json)
// BEFORE ss_pack_files / ss_load_staged — a locale is described entirely by its pack.
// Returns 1 on success, 0 if the manifest is rejected (fail-closed). This is the browser
// equivalent of the device's SD-card pack discovery.
EMSCRIPTEN_KEEPALIVE int ss_register_manifest(const char* json, int len) {
    if (!json || len <= 0) return 0;
    return ss_register_pack_manifest(json, static_cast<size_t>(len)) ? 1 : 0;
}

// The list of files JS must fetch + stage for `locale` ("["ur.ttf","runs.bin"]",
// or "[]" for a baked-floor / unregistered locale). Comes from the render layer's
// manifest via the shared loader, so the web host never duplicates that policy.
// Requires the locale's manifest to have been registered (ss_register_manifest) first.
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
// "settings_locale_picker_screen", ...); the picker then fetches them via endonym_provider.
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
                set_pointer_mapped(ev.motion.x, ev.motion.y,
                                   (ev.motion.state & SDL_BUTTON_LMASK) != 0);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    set_pointer_mapped(ev.button.x, ev.button.y, true);
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    set_pointer_mapped(ev.button.x, ev.button.y, false);
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
    if (g_present_rotate) {
        // Rotate the portrait texture 90° CW so it fills the landscape canvas. Placing the
        // portrait rect (tw x th) centered on the landscape canvas and rotating about its
        // center lands its footprint exactly on the landscape bounds.
        int tw = runner_core::width();
        int th = runner_core::height();
        SDL_Rect dst = { (g_land_w - tw) / 2, (g_land_h - th) / 2, tw, th };
        SDL_RenderCopyEx(g_renderer, g_texture, nullptr, &dst,
                         PRESENT_ROTATE_CW_DEG, nullptr, SDL_FLIP_NONE);
    } else {
        SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    }
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
    g_land_w = DISPLAY_WIDTH;
    g_land_h = DISPLAY_HEIGHT;
    create_sdl_surface(DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_WIDTH, DISPLAY_HEIGHT);

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
