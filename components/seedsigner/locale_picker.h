#ifndef LOCALE_PICKER_H
#define LOCALE_PICKER_H

// ---------------------------------------------------------------------------
// Locale-picker endonym images (the "show every language in its own script,
// with no runtime font" plumbing for locale_picker_screen).
// ---------------------------------------------------------------------------
//
// The language-selection screen lists every onboard language's name in its own
// native script (日本語, हिन्दी, فارسی, …). Keeping a script font resident per
// language would blow the ESP32's small glyph-cache pool, so for every pack
// locale the name is PRE-RENDERED offline into a small A8 alpha image
// (endonym_<h>.bin, see tools/i18n/render_endonym.py). This module fetches such
// an image through the same byte-provider seam ss_load_locale uses, parses the
// self-describing "SSA8" container, and paints it over a picker row button
// recolored to the row's LIVE text color — so the row highlights/inverts on
// focus with zero runtime fonts. Latin-script rows stay live text (button_ex).
//
// The screen itself is locale_picker_screen() in seedsigner.cpp (it needs the
// shared top-nav scaffold / navigation / screen-load helpers); it calls the
// functions below to dress each image row.

#include "lvgl.h"
#include "locale_loader.h"   // ss_pack_provider_t (the shared byte-provider seam)

#ifdef __cplusplus
extern "C" {
#endif

// Set the byte provider the picker uses to fetch endonym images. This is the
// SAME seam and signature as ss_load_locale's provider — a host that already has
// a filesystem / SD / staging provider reuses it verbatim (the picker just asks
// for "endonym_<h>.bin" instead of a .ttf). Set before building the picker; pass
// NULL to disable image rows (they then fall back to whatever their live label
// renders). The picker copies whatever it keeps, so the bytes need only stay
// valid for the provider call, exactly like ss_load_locale.
void locale_picker_set_image_provider(ss_pack_provider_t provider, void* user);

// Dress a picker row button (built by button_ex with CHECKED_SELECTION) as an
// IMAGE row: fetch `image_file` for `locale` via the provider, parse the SSA8
// blob, suppress the button's live text label, and attach a draw callback that
// paints the endonym image recolored to the label's current text color (so it
// tracks the focus highlight) plus a delete callback that frees it. Returns true
// on success; false (leaving the button untouched, so its live text still shows)
// if there is no provider, the fetch fails, or the blob is malformed — fail
// closed, never crash the picker on a bad/half-copied pack. `btn` must be a
// button built by button()/button_ex() (it is located by its tagged text label).
bool locale_picker_attach_endonym(lv_obj_t* btn, const char* locale,
                                  const char* image_file);

#ifdef __cplusplus
}
#endif

#endif // LOCALE_PICKER_H
