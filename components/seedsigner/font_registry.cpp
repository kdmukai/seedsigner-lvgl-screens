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
    const uint8_t* buf;                        // caller-owned subset .ttf bytes (NOT freed here)
    size_t len;                                // byte count of buf
    int px;                                    // pixel size `script` was created at
};

static std::vector<Registration> g_registrations;

// Fonts detached from the active profile but NOT yet destroyed. A locale switch
// restores the profile and registers the new fonts WHILE the previous locale's
// screen is still the active LVGL screen — and that screen's labels hold raw
// pointers to the previous script fonts. Destroying those fonts immediately makes
// every still-live label a dangling pointer: the moment LVGL redraws the old
// screen (e.g. the ESP32 render task ticking between load_locale and the screen
// rebuild, two separate host calls), it draws a label through a freed lv_font_t
// and faults. So clear() RETIRES the fonts here instead of freeing them; the
// screen layer calls seedsigner_reap_retired_fonts() only AFTER it deletes the old
// screen (see load_screen_and_cleanup_previous), when nothing references them.
static std::vector<Registration> g_retired;

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

bool seedsigner_locale_uses_glyph_runs() {
    const LocaleFontEntry* entry = find_locale_font_entry(g_current_locale);
    return entry && entry->shaping;
}

// Look up a registered script font by pointer. For a shaping locale every role
// installs the SAME subset .ttf at a different px, so multiple registrations
// share buf/len and differ only in `script`/`px`; matching the resolved label
// font pointer selects the right px. A shaping locale is always Primary, so the
// installed field IS `script` (see seedsigner_register_font); no heap_copy case.
static const Registration* find_registration_by_font(const lv_font_t* font) {
    if (!font) return nullptr;
    for (const Registration& r : g_registrations) {
        if (r.script == font) return &r;
    }
    return nullptr;
}

int seedsigner_registered_font_px(const struct _lv_font_t* font) {
    const Registration* r = find_registration_by_font(font);
    return r ? r->px : 0;
}

const uint8_t* seedsigner_registered_font_bytes(const struct _lv_font_t* font, size_t* len_out) {
    const Registration* r = find_registration_by_font(font);
    if (!r) {
        if (len_out) *len_out = 0;
        return nullptr;
    }
    if (len_out) *len_out = r->len;
    return r->buf;
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

    g_registrations.push_back({&profile, field, original, script, heap_copy, buf, len, font_px_size});
    return true;
}

void seedsigner_clear_registered_fonts() {
    // Detach each registration from its profile (restore the compiled-in font) but
    // DEFER destroying the fonts: the previous locale's screen is typically still
    // active and its labels still point at `script`/`heap_copy`. Move them to the
    // retired list; seedsigner_reap_retired_fonts() frees them once that screen is
    // gone. Buffers (owned by the loader) must likewise outlive the retired fonts.
    for (Registration& r : g_registrations) {
        (r.profile)->*(r.field) = r.original;  // restore compiled-in font
        g_retired.push_back(r);                 // free later, after the old screen dies
    }
    g_registrations.clear();
    g_current_locale.clear();
}

void seedsigner_reap_retired_fonts() {
    for (Registration& r : g_retired) {
        if (r.heap_copy) delete r.heap_copy;
#if LV_USE_TINY_TTF
        if (r.script) lv_tiny_ttf_destroy(r.script);
#endif
    }
    g_retired.clear();
}
