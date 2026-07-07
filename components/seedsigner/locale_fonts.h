#ifndef LOCALE_FONTS_H
#define LOCALE_FONTS_H

// ---------------------------------------------------------------------------
// Canonical locale -> {font, size, chain} table (owned by the render layer)
// ---------------------------------------------------------------------------
//
// This is the single source of truth for which *additional* (non-baked-in)
// fonts a locale needs, at what sizes, and how they chain with the baked-in
// OpenSans + icon "floor". The render layer owns this mapping (it answers "how
// does a glyph get on screen"); the platform/policy layer only reports the
// active locale and supplies already-verified font bytes through the seam in
// font_registry.h.
//
// English (and any locale whose glyphs are all covered by the baked floor) has
// NO entry here: an absent locale means "no additional fonts required".
//
// See docs/font-and-i18n-rendering.md / docs/font-and-i18n-implementation-plan.md.

#include <string>
#include <vector>

// Text roles a per-locale script font can occupy. Deliberately excludes the icon
// fonts and the keyboard/text-entry font: passphrase input is ASCII (closed
// corpus), so the keyboard never needs non-Latin glyphs.
enum class TextRole {
    Body,
    Button,
    LargeButton,
    TopNavTitle,
    MainMenuTitle,
};

// How a locale's script font relates to the baked-in OpenSans floor.
enum class ChainRole {
    // Dominant-script locale (CJK): the script font becomes the PRIMARY for each
    // text role at a per-role, legibility-bumped size; the baked OpenSans stays
    // as its fallback, so embedded ASCII (technical terms) still renders at the
    // normal English size. Line metrics come from the (taller) script primary.
    Primary,
    // Same-size script (e.g. Greek/Cyrillic subset of OpenSans): the script font
    // chains as a FALLBACK under the baked OpenSans primary at the native size.
    // No size bump, so no line-metric adjustment is needed.
    Fallback,
};

// One text role served by a locale's script font, with the BASE px size
// (pre-PX_MULTIPLIER) at which to bake/wire it. The actual per-profile px is
// base_size * PX_MULTIPLIER / 100.
struct LocaleRoleSize {
    TextRole role;
    int base_size;
};

struct LocaleFontEntry {
    std::string locale;        // e.g. "zh_Hans_CN"
    std::string source_family; // source TTF family for offline subsetting, e.g. "NotoSansSC"
    ChainRole chain_role;
    std::vector<LocaleRoleSize> roles;
    // Non-empty ⇒ a same-size SCRIPT pack: subset OpenSans by this fixed Unicode
    // block range (pyftsubset --unicodes form, e.g. "U+0400-04FF"), NOT by the
    // translation corpus. The pack chains as a Fallback under the baked OpenSans
    // Western baseline at the native role sizes. Empty ⇒ a corpus-subset pack
    // (the Noto Primary/CJK locales), built from the .po glyph set.
    std::string unicode_range;
    // Right-to-left script (Arabic/Persian/Hebrew). The screen layer mirrors
    // layout (base direction, top-nav button sides, text alignment) for these
    // locales; bidi reordering itself is handled by LVGL (LV_USE_BIDI). Surfaced
    // in the manifest as "rtl" so every consumer/platform learns it the same way.
    bool rtl = false;
    // Complex script rendered from OFFLINE-shaped glyph runs (Devanagari, Thai,
    // Nastaliq/Urdu, …) rather than by codepoint. The pack ships a per-locale
    // run table (runs.bin) alongside the subset .ttf; the screen layer draws the
    // pre-shaped glyph-ids (see glyph_runs.*). The subset still registers as the
    // role font (so the device can rasterize those gids), but tiny_ttf's own
    // codepoint path is bypassed for shaped text. Distinct from `fa`, which still
    // uses LVGL's presentation-form path (LV_USE_ARABIC_PERSIAN_CHARS) — false.
    bool shaping = false;
    // ISO-15924 script tag for the offline shaper (e.g. "Deva", "Thai", "Arab").
    // Only meaningful when `shaping` is true; surfaced in the manifest so the
    // offline builder shapes with the right script without duplicating policy.
    std::string script;
};

// Locale lookup. Every non-English locale is described by a runtime-registered pack
// manifest — there is no compiled-in locale table. Returns nullptr for an unregistered
// locale, which then renders on the baked English/Latin floor.
const LocaleFontEntry* find_locale_font_entry(const std::string& locale);

// ---------------------------------------------------------------------------
// Runtime-registered locales (language packs).
// ---------------------------------------------------------------------------
//
// Screens bakes only the English floor; a locale becomes renderable by REGISTERING its
// pack's own manifest.json (ss_register_pack_manifest in locale_loader.h), which
// synthesizes a LocaleFontEntry. So a brand-new language works with NO recompile — its
// pack, discovered under lang-packs/ (desktop/Pi) or on the SD card (ESP32), is scanned
// and registered by the host. find_locale_font_entry() and supported_locales_json()
// consult ONLY these runtime entries, so the whole load path (ss_load_locale /
// ss_locale_pack_files) works from the manifests alone. locale policy (chain / rtl /
// shaping / role sizes) lives in the seedsigner-language-packs repo, not here.

// The canonical role/size preset for a chain. A runtime pack that does not carry
// its own role sizes inherits exactly what the compiled packs use for that chain
// (Primary = CJK legibility bump; Fallback = same-size as the baked floor), so
// the two sources agree.
std::vector<LocaleRoleSize> default_locale_roles(ChainRole chain);

// Add or replace (by locale code) a runtime locale entry. Returns false only when
// the entry is unusable (empty locale). Idempotent per code: re-registering the
// same code replaces the prior entry.
bool register_runtime_locale_entry(const LocaleFontEntry& entry);

// Drop all runtime-registered locale entries (e.g. before a fresh SD rescan).
void clear_runtime_locale_entries();

// Render px for a role under a locale entry at the given PX_MULTIPLIER (0 if the
// locale's font does not serve that role). Both the manifest (what px the host
// rasterizes each role at) and the registration-seam size guard use this, so the
// two always agree. For a same-size (Fallback) script pack it returns the baked
// OpenSans baseline's px for the role — including the large_button quirk (20 base
// at 240 height, 18 at 320/480) — so script glyphs match the Latin baseline.
int locale_role_render_px(const LocaleFontEntry& entry, TextRole role, int px_multiplier);

// Native (un-bumped) base px size for a text role, matching make_profile().
int text_role_native_base_size(TextRole role);

// Stable string id for a role (used as the register_font logical name and in the
// manifest / generated font filenames). Round-trips via text_role_from_name().
const char* text_role_name(TextRole role);
bool text_role_from_name(const std::string& name, TextRole& out);

// JSON manifest of all locales that need additional fonts, with the specific
// font files required for the ACTIVE display profile (call after set_display()).
// A locale absent from the manifest is covered by the baked floor. The host
// fetches this once at startup, reconciles it against the fonts present in its
// store (and against available .mo catalogs), and offers the satisfied locales.
//
// Shape:
//   {"profile":{"width":W,"height":H},
//    "locales":[
//      {"locale":"zh_Hans_CN","source_family":"NotoSansSC","chain":"primary",
//       "fonts":[{"role":"body","px":18,"file":"zh_Hans_CN.ttf"}, ...]}, ...]}
//       (one subset .ttf per locale serves all sizes; px is the render size per role)
std::string supported_locales_json();

#endif // LOCALE_FONTS_H
