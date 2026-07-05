#include "locale_fonts.h"
#include "gui_constants.h"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Match make_profile()'s scaling (base px -> profile px).
static int px_scale(int base, int multiplier) {
    return static_cast<int>(base * multiplier / 100.0);
}

// ---------------------------------------------------------------------------
// Native (un-bumped) base sizes — must mirror make_profile()/fonts_for_multiplier().
// ---------------------------------------------------------------------------
int text_role_native_base_size(TextRole role) {
    switch (role) {
        case TextRole::Body:          return 17;  // opensans_regular_17
        case TextRole::Button:        return 18;  // opensans_semibold_18
        case TextRole::LargeButton:   return 20;  // opensans_semibold_20
        case TextRole::TopNavTitle:   return 20;  // opensans_semibold_20
        case TextRole::MainMenuTitle: return 26;  // opensans_semibold_26
    }
    return 17;
}

const char* text_role_name(TextRole role) {
    switch (role) {
        case TextRole::Body:          return "body";
        case TextRole::Button:        return "button";
        case TextRole::LargeButton:   return "large_button";
        case TextRole::TopNavTitle:   return "top_nav_title";
        case TextRole::MainMenuTitle: return "main_menu_title";
    }
    return "body";
}

bool text_role_from_name(const std::string& name, TextRole& out) {
    if (name == "body")            { out = TextRole::Body;          return true; }
    if (name == "button")          { out = TextRole::Button;        return true; }
    if (name == "large_button")    { out = TextRole::LargeButton;   return true; }
    if (name == "top_nav_title")   { out = TextRole::TopNavTitle;   return true; }
    if (name == "main_menu_title") { out = TextRole::MainMenuTitle; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// The canonical table.
//
// CJK (Primary): legibility-bumped base sizes follow the production Python
// per-locale size tables (gui/components.py): body 17->18, button 18->20,
// title 20->23. LargeButton (native 20) tracks the title bump to 23;
// MainMenuTitle (26) is already large enough and is left un-bumped.
//
// Phase 1 ships the three CJK locales. Same-size scripts (Greek/Cyrillic/
// Vietnamese, ChainRole::Fallback over OpenSans) and shaping-complex scripts
// (Arabic/Persian/Thai/Hindi, Phase 2) are added here as their subsets land.
// ---------------------------------------------------------------------------
static std::vector<LocaleRoleSize> cjk_primary_roles() {
    return {
        {TextRole::Body,          18},
        {TextRole::Button,        20},
        {TextRole::LargeButton,   23},
        {TextRole::TopNavTitle,   23},
        {TextRole::MainMenuTitle, 26},
    };
}

// Same-size script packs (Greek/Cyrillic/Vietnamese): OpenSans subsets chained as
// a Fallback under the baked Western baseline at the NATIVE role sizes (so the
// script glyphs match the Latin baseline). Sizes mirror make_profile()/the
// baseline install; the large_button quirk (18 vs 20 at 320/480) is applied in
// locale_role_render_px(), keyed off the active multiplier.
static std::vector<LocaleRoleSize> opensans_fallback_roles() {
    return {
        {TextRole::Body,          17},
        {TextRole::Button,        18},
        {TextRole::LargeButton,   20},
        {TextRole::TopNavTitle,   20},
        {TextRole::MainMenuTitle, 26},
    };
}

const std::vector<LocaleFontEntry>& locale_font_table() {
    static const std::vector<LocaleFontEntry> table = {
        // CJK: corpus-subset Noto, becomes the Primary (bumped sizes).
        {"zh_Hans_CN", "NotoSansSC", ChainRole::Primary, cjk_primary_roles()},
        {"ja",         "NotoSansJP", ChainRole::Primary, cjk_primary_roles()},
        {"ko",         "NotoSansKR", ChainRole::Primary, cjk_primary_roles()},
        // Arabic/Persian (Phase 2): corpus-subset NotoSansAR as the Primary, at
        // the same legibility-bumped sizes as CJK. The subset holds the
        // presentation FORMS the renderer emits (LV_USE_ARABIC_PERSIAN_CHARS),
        // not base letters — the offline builder runs the corpus through the real
        // LVGL shaper (tools/i18n/shaper) to learn them. rtl=true drives layout
        // mirroring; LVGL (LV_USE_BIDI) handles the bidi reordering. The baked
        // OpenSans baseline stays as fallback so embedded Latin/digits render LTR.
        {"fa", "NotoSansAR", ChainRole::Primary, cjk_primary_roles(), "", /*rtl=*/true},
        // Complex-script (Phase 2): rendered from OFFLINE HarfBuzz glyph runs, not
        // by codepoint. corpus-subset Noto as the Primary at the CJK legibility
        // bump; the pack ships runs.bin next to <locale>.ttf. The subset keeps its
        // GSUB/GPOS/GDEF layout closure (the offline shaper needs it), unlike the
        // CJK packs which drop layout. shaping=true + the ISO-15924 script tag tell
        // the offline builder how to shape; the screen layer draws the runs.
        //   hi  Devanagari   reorder + conjuncts (glyphs with no codepoint)
        //   th  Thai         GPOS mark stacking + SARA-AM decomposition
        //   ur  Nastaliq     the extreme GPOS case (diagonal baseline cascade), rtl
        {"hi", "NotoSansDevanagari", ChainRole::Primary, cjk_primary_roles(), "", /*rtl=*/false, /*shaping=*/true, "Deva"},
        {"th", "NotoSansTH",         ChainRole::Primary, cjk_primary_roles(), "", /*rtl=*/false, /*shaping=*/true, "Thai"},
        {"ur", "NotoNastaliqUrdu",   ChainRole::Primary, cjk_primary_roles(), "", /*rtl=*/true,  /*shaping=*/true, "Arab"},
        // Script packs: block-range OpenSans subsets, same-size Fallback over the
        // baked Western baseline. One pack per script covers its language family
        // (e.g. ru's Cyrillic block also serves uk/bg) with no corpus coupling.
        {"el", "OpenSans", ChainRole::Fallback, opensans_fallback_roles(), "U+0370-03FF"},              // Greek
        {"ru", "OpenSans", ChainRole::Fallback, opensans_fallback_roles(), "U+0400-04FF"},              // Cyrillic
        // Vietnamese: the horn vowels Ơơ/Ưư live in Latin Extended-B (above the baked
        // Western floor's Latin-1+Ext-A); without them common words ("được") tofu. Plus
        // combining marks + the precomposed tone-marked vowels in Latin Ext Additional.
        {"vi", "OpenSans", ChainRole::Fallback, opensans_fallback_roles(), "U+01A0-01A1,U+01AF-01B0,U+0300-036F,U+1E00-1EFF"},  // Vietnamese
    };
    return table;
}

// ---------------------------------------------------------------------------
// Runtime-registered locales (SD-card packs). See locale_fonts.h.
// ---------------------------------------------------------------------------
static std::vector<LocaleFontEntry>& runtime_entries() {
    static std::vector<LocaleFontEntry> entries;
    return entries;
}

std::vector<LocaleRoleSize> default_locale_roles(ChainRole chain) {
    // 1:1 with the compiled table: every Primary entry uses the CJK bump, every
    // Fallback entry the same-size baseline preset.
    return chain == ChainRole::Primary ? cjk_primary_roles()
                                       : opensans_fallback_roles();
}

bool register_runtime_locale_entry(const LocaleFontEntry& entry) {
    if (entry.locale.empty()) return false;
    std::vector<LocaleFontEntry>& entries = runtime_entries();
    for (LocaleFontEntry& e : entries) {
        if (e.locale == entry.locale) { e = entry; return true; }  // replace by code
    }
    entries.push_back(entry);
    return true;
}

void clear_runtime_locale_entries() {
    runtime_entries().clear();
}

const LocaleFontEntry* find_locale_font_entry(const std::string& locale) {
    // Runtime (SD) entries win over the compiled default for the same code.
    for (const LocaleFontEntry& e : runtime_entries()) {
        if (e.locale == locale) return &e;
    }
    for (const LocaleFontEntry& e : locale_font_table()) {
        if (e.locale == locale) return &e;
    }
    return nullptr;
}

int locale_role_render_px(const LocaleFontEntry& entry, TextRole role, int mult) {
    int base = -1;
    for (const LocaleRoleSize& r : entry.roles) {
        if (r.role == role) { base = r.base_size; break; }
    }
    if (base < 0) return 0;  // locale's font does not serve this role

    // Same-size script packs must match the compiled-in OpenSans baseline EXACTLY,
    // including its large_button quirk: 20 px base at the 240-height profile
    // (PX_MULTIPLIER_100) but 18 px at 320/480. (Primary/CJK packs set their own
    // bumped sizes, so they take the table value unchanged.)
    if (entry.chain_role == ChainRole::Fallback &&
        role == TextRole::LargeButton && mult != PX_MULTIPLIER_100) {
        base = 18;
    }
    return px_scale(base, mult);
}

// ---------------------------------------------------------------------------
// Manifest for the active profile.
// ---------------------------------------------------------------------------
std::string supported_locales_json() {
    const DisplayProfile& profile = active_profile();
    const int mult = profile.px_multiplier;

    json out;
    out["profile"] = { {"width", profile.width}, {"height", profile.height} };
    out["locales"] = json::array();

    // Emit runtime (SD) entries first; a compiled entry with the same code is then
    // skipped (the runtime pack overrides it). This is what lets ss_load_locale /
    // ss_locale_pack_files serve a not-compiled-in locale — they read this JSON.
    std::vector<const LocaleFontEntry*> entries;
    for (const LocaleFontEntry& e : runtime_entries()) entries.push_back(&e);
    for (const LocaleFontEntry& e : locale_font_table()) {
        bool overridden = false;
        for (const LocaleFontEntry& r : runtime_entries()) {
            if (r.locale == e.locale) { overridden = true; break; }
        }
        if (!overridden) entries.push_back(&e);
    }

    for (const LocaleFontEntry* ep : entries) {
        const LocaleFontEntry& e = *ep;
        json locale_obj;
        locale_obj["locale"] = e.locale;
        locale_obj["source_family"] = e.source_family;
        locale_obj["chain"] = (e.chain_role == ChainRole::Primary) ? "primary" : "fallback";
        // Script packs carry the block range the offline subsetter slices (no .po).
        if (!e.unicode_range.empty()) {
            locale_obj["unicode_range"] = e.unicode_range;
        }
        // RTL locales: the host/screen layer mirrors layout for these. Emitted
        // only when true (LTR is the default), mirroring the unicode_range style.
        if (e.rtl) {
            locale_obj["rtl"] = true;
        }
        // Complex-script locales: the offline builder shapes these into glyph runs
        // (vs corpus/block subsetting). Emit the shaper script tag so the builder
        // never duplicates the locale->script policy the render layer owns.
        if (e.shaping) {
            locale_obj["shaping"] = true;
            locale_obj["script"] = e.script;
        }

        json fonts = json::array();
        const bool fallback = (e.chain_role == ChainRole::Fallback);
        for (const LocaleRoleSize& r : e.roles) {
            int px = locale_role_render_px(e, r.role, mult);
            // Weight: an OpenSans script pack matches the baseline — body=Regular,
            // the other four roles=SemiBold. The Noto Primary packs ship a single
            // Regular weight that serves every role.
            const char* weight = (fallback && r.role != TextRole::Body) ? "semibold" : "regular";
            // One subset .ttf per (locale, weight) serves every size (tiny_ttf
            // creates a font at any px from the same data). A single-weight Noto
            // pack stays "<locale>.ttf"; a two-weight script pack splits the file.
            std::string file = fallback ? (e.locale + "_" + weight + ".ttf")
                                        : (e.locale + ".ttf");
            fonts.push_back({
                {"role", text_role_name(r.role)},
                {"px", px},
                {"file", file},
                {"weight", weight},
            });
        }
        locale_obj["fonts"] = fonts;
        out["locales"].push_back(locale_obj);
    }

    return out.dump();
}
