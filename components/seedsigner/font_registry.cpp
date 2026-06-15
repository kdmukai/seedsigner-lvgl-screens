#include "font_registry.h"
#include "gui_constants.h"
#include "locale_fonts.h"

#include "lvgl.h"

#include <cstdio>
#include <string>
#include <vector>

// The locale selected via seedsigner_set_locale(). Empty => baked floor only.
static std::string g_current_locale;

// One installed registration, tracked so seedsigner_clear_registered_fonts()
// can restore the profile exactly — even if the active profile changed since.
struct Registration {
    DisplayProfile* profile;                  // profile the field was repointed on
    const lv_font_t* DisplayProfile::* field; // which text-font pointer was changed
    const lv_font_t* original;                // compiled-in font to restore
    lv_font_t* script;                        // binfont created from the buffer (destroy)
    lv_font_t* heap_copy;                      // fallback-mode primary copy (delete), or null
};

static std::vector<Registration> g_registrations;

// Map a text role to its DisplayProfile font-pointer member.
static const lv_font_t* DisplayProfile::* role_field(TextRole role) {
    switch (role) {
        case TextRole::Body:          return &DisplayProfile::body_font;
        case TextRole::Button:        return &DisplayProfile::button_font;
        case TextRole::LargeButton:   return &DisplayProfile::large_button_font;
        case TextRole::TopNavTitle:   return &DisplayProfile::top_nav_title_font;
        case TextRole::MainMenuTitle: return &DisplayProfile::main_menu_title_font;
    }
    return &DisplayProfile::body_font;
}

void seedsigner_set_locale(const char* locale) {
    g_current_locale = locale ? locale : "";
}

bool seedsigner_locale_is_rtl() {
    const LocaleFontEntry* entry = find_locale_font_entry(g_current_locale);
    return entry && entry->rtl;
}

bool seedsigner_register_font(const char* logical_name, const uint8_t* buf, size_t len, int font_px_size) {
    if (!logical_name || !buf || len == 0) {
        fprintf(stderr, "seedsigner_register_font: invalid arguments\n");
        return false;
    }

    TextRole role;
    if (!text_role_from_name(logical_name, role)) {
        fprintf(stderr, "seedsigner_register_font: unknown role '%s'\n", logical_name);
        return false;
    }

    const LocaleFontEntry* entry = find_locale_font_entry(g_current_locale);
    if (!entry) {
        fprintf(stderr, "seedsigner_register_font: locale '%s' has no font table entry\n",
                g_current_locale.c_str());
        return false;
    }

    DisplayProfile& profile = active_profile_mutable();

    // Validate the supplied size against what the table expects for this role at
    // the active profile — catches host/manifest drift early. Same source of
    // truth as the manifest (locale_role_render_px), so they always agree.
    int expected_px = locale_role_render_px(*entry, role, profile.px_multiplier);
    if (expected_px <= 0) {
        fprintf(stderr, "seedsigner_register_font: locale '%s' does not use role '%s'\n",
                g_current_locale.c_str(), logical_name);
        return false;
    }
    if (font_px_size != expected_px) {
        fprintf(stderr, "seedsigner_register_font: size mismatch for '%s' (%s @ %dx%d): "
                "got %d, expected %d\n",
                logical_name, g_current_locale.c_str(), profile.width, profile.height,
                font_px_size, expected_px);
        return false;
    }

#if LV_USE_TINY_TTF
    // One subset .ttf per locale serves every size; create a font at this role's px.
    // NOTE: tiny_ttf keeps a reference to `buf` (lazy glyph reads), so the caller
    // must keep the buffer alive until seedsigner_clear_registered_fonts().
    // KERNING_NONE: our scripts don't need pair kerning.
    // Cache size = SEEDSIGNER_TTF_CACHE_SIZE (gui_constants.h): enabled by default
    // for redraw/scroll speed. The cache retains rasterized bitmaps, so every
    // target must back LVGL with adequate RAM/PSRAM (Pi Zero: LV_STDLIB_CLIB;
    // ESP32-S3: bitmaps in PSRAM); against a too-small fixed pool it OOMs and
    // LVGL's default assert handler spins. See docs/knowledge/tiny-ttf-cache-spin-root-cause.md.
    lv_font_t* script = lv_tiny_ttf_create_data_ex(buf, len, font_px_size,
                                                   LV_FONT_KERNING_NONE,
                                                   SEEDSIGNER_TTF_CACHE_SIZE);
#else
    fprintf(stderr, "seedsigner_register_font: built without LV_USE_TINY_TTF\n");
    lv_font_t* script = nullptr;
#endif
    if (!script) {
        fprintf(stderr, "seedsigner_register_font: failed to load font for '%s'\n", logical_name);
        return false;
    }

    auto field = role_field(role);
    const lv_font_t* original = profile.*field;
    lv_font_t* heap_copy = nullptr;

    if (entry->chain_role == ChainRole::Primary) {
        // Script font becomes the primary; baked OpenSans is its fallback so
        // embedded ASCII still renders at the English size. Line metrics come
        // from the (taller) script primary — no adjustment needed.
        script->fallback = original;
        profile.*field = script;
    } else {
        // Same-size script: chain it as a fallback under a heap copy of the
        // (const) compiled-in primary. The copy shares the original's read-only
        // glyph data; only its fallback pointer differs.
        heap_copy = new lv_font_t(*original);
        heap_copy->fallback = script;
        profile.*field = heap_copy;
    }

    g_registrations.push_back({&profile, field, original, script, heap_copy});
    return true;
}

void seedsigner_clear_registered_fonts() {
    for (Registration& r : g_registrations) {
        (r.profile)->*(r.field) = r.original;  // restore compiled-in font
        if (r.heap_copy) delete r.heap_copy;
#if LV_USE_TINY_TTF
        if (r.script) lv_tiny_ttf_destroy(r.script);
#endif
    }
    g_registrations.clear();
    g_current_locale.clear();
}
