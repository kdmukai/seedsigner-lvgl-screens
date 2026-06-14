#ifndef SEEDSIGNER_RUNNER_SDL_H
#define SEEDSIGNER_RUNNER_SDL_H

// runner_sdl — the two genuinely SDL-specific helpers shared by both SDL hosts
// (the native screen_runner and the Emscripten web_runner, which also builds
// against SDL via -sUSE_SDL=2). Kept separate from runner_core so that core
// stays SDL-free and headlessly testable.

#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#else
#error "SDL header not found (expected SDL2/SDL.h or SDL.h)"
#endif

#include <cstdint>

namespace runner_sdl {

// Map an SDL keycode to the LVGL key the screens expect, or 0 if unmapped.
// Arrow keys → LV_KEY_*, RETURN/KP_ENTER → LV_KEY_ENTER (center select), and
// number keys 1/2/3 (with F1/F2/F3 aliases) → the aux-key char codes '1'/'2'/'3'
// consumed by the passphrase KEY1/KEY2/KEY3 panel.
uint32_t map_sdl_keycode(SDL_Keycode key);

// Blit runner_core's RGB565 framebuffer into an ARGB8888 streaming SDL texture.
// The texture must be runner_core::width() × runner_core::height().
void blit_framebuffer_to_texture(SDL_Texture* texture);

}  // namespace runner_sdl

#endif  // SEEDSIGNER_RUNNER_SDL_H
