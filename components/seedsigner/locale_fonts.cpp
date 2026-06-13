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

const std::vector<LocaleFontEntry>& locale_font_table() {
    static const std::vector<LocaleFontEntry> table = {
        {"zh_Hans_CN", "NotoSansSC", ChainRole::Primary, cjk_primary_roles()},
        {"ja",         "NotoSansJP", ChainRole::Primary, cjk_primary_roles()},
        {"ko",         "NotoSansKR", ChainRole::Primary, cjk_primary_roles()},
    };
    return table;
}

const LocaleFontEntry* find_locale_font_entry(const std::string& locale) {
    for (const LocaleFontEntry& e : locale_font_table()) {
        if (e.locale == locale) return &e;
    }
    return nullptr;
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

    for (const LocaleFontEntry& e : locale_font_table()) {
        json locale_obj;
        locale_obj["locale"] = e.locale;
        locale_obj["source_family"] = e.source_family;
        locale_obj["chain"] = (e.chain_role == ChainRole::Primary) ? "primary" : "fallback";

        json fonts = json::array();
        // One subset .ttf per locale serves every size (tiny_ttf creates a font
        // at any px from the same data); each role lists the px to create it at.
        std::string file = e.locale + ".ttf";
        for (const LocaleRoleSize& r : e.roles) {
            int px = px_scale(r.base_size, mult);
            fonts.push_back({
                {"role", text_role_name(r.role)},
                {"px", px},
                {"file", file},
            });
        }
        locale_obj["fonts"] = fonts;
        out["locales"].push_back(locale_obj);
    }

    return out.dump();
}
