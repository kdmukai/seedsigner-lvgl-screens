#ifndef FONT_REGISTRY_H
#define FONT_REGISTRY_H

// ---------------------------------------------------------------------------
// Font registration seam (render layer; I/O- and crypto-agnostic)
// ---------------------------------------------------------------------------
//
// The render layer turns already-verified font *bytes in RAM* into LVGL fonts
// and wires them into the active display profile per the canonical locale table
// (locale_fonts.h). It never opens files and never verifies signatures — the
// host (platform/policy) acquires + verifies bytes and pushes them in here.
//
// Typical host flow (see docs):
//   set_display(w, h);
//   manifest = supported_locales_json();          // discover required files
//   ... host loads/verifies the active locale's files ...
//   seedsigner_set_locale("zh_Hans_CN");
//   for each required file: seedsigner_register_font(role, bytes, len, px);  // free bytes after
//   ... render ...
//   seedsigner_clear_registered_fonts();           // restore baked-in fonts

#include <cstddef>
#include <cstdint>

// Select the active locale for subsequent register calls. Looks up the
// canonical table to determine chain roles + expected sizes. Passing "en" (or
// any locale absent from the table) is valid and registers nothing.
void seedsigner_set_locale(const char* locale);

// True if the active locale (per seedsigner_set_locale) is a right-to-left
// script. The screen layer uses this to apply RTL text direction to labels in
// one global post-pass — see load_screen_and_cleanup_previous(). False for "en",
// any LTR locale, or no locale set.
bool seedsigner_locale_is_rtl();

// Register one already-verified subset font for a text role at the active
// profile. `logical_name` is the role id ("body", "button", "large_button",
// "top_nav_title", "main_menu_title"). `buf`/`len` are the subset .ttf bytes.
// IMPORTANT: tiny_ttf reads glyph outlines lazily, so `buf` MUST remain valid
// for the lifetime of the font — until seedsigner_clear_registered_fonts(). The
// caller owns the buffer and frees it only after clearing. `font_px_size` must
// match the size the table expects for this role at the active profile
// (validated). Returns true on success.
bool seedsigner_register_font(const char* logical_name, const uint8_t* buf, size_t len, int font_px_size);

// Detach and destroy every registered font, restoring the compiled-in fonts on
// whichever profile each was installed on. Call before switching resolution.
void seedsigner_clear_registered_fonts();

#endif // FONT_REGISTRY_H
