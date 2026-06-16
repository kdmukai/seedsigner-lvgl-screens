#include "locale_loader.h"
#include "locale_fonts.h"    // supported_locales_json()
#include "font_registry.h"   // seedsigner_set_locale / register_font / clear
#include "glyph_runs.h"      // seedsigner_set_glyph_runs / clear

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using json = nlohmann::json;

// The loader OWNS the registered font byte buffers: tiny_ttf reads glyph outlines
// lazily, so the bytes must outlive the lv_font_t. Freed on unload / next load.
// A locale needs at most 2 unique font files (a two-weight Fallback pack), so a
// small reserve keeps these addresses stable across registration. (runs.bin
// bytes are copied by seedsigner_set_glyph_runs and are not retained here.)
static std::vector<std::vector<uint8_t>> g_owned;

void ss_unload_locale(void) {
    seedsigner_clear_registered_fonts();  // destroys fonts referencing g_owned (must run first)
    seedsigner_clear_glyph_runs();
    g_owned.clear();                       // now safe to free the buffers
    seedsigner_set_locale("");
}

// Locate the manifest entry for `locale` (the per-active-profile font plan). The
// manifest is the render layer's own supported_locales_json(), so the loader never
// duplicates the locale -> {font, role, px, file} policy.
static bool find_locale_entry(const std::string& locale, json& out_entry) {
    json manifest = json::parse(supported_locales_json(), nullptr, /*allow_exceptions=*/false);
    if (manifest.is_discarded() || !manifest.contains("locales")) return false;
    for (const auto& loc : manifest["locales"]) {
        if (loc.value("locale", std::string()) == locale) { out_entry = loc; return true; }
    }
    return false;
}

bool ss_load_locale(const char* locale_c, ss_pack_provider_t provider, void* user) {
    ss_unload_locale();
    const std::string locale = locale_c ? locale_c : "";
    seedsigner_set_locale(locale.c_str());
    if (locale.empty()) return true;

    json entry;
    if (!find_locale_entry(locale, entry)) return true;  // baked-floor locale: nothing to load
    if (!provider) {
        fprintf(stderr, "ss_load_locale: provider required for pack locale '%s'\n", locale.c_str());
        return false;
    }

    g_owned.reserve(4);  // keep .data() pointers stable across the registrations below

    // Fetch each UNIQUE file once (CJK/shaping share one .ttf across all roles;
    // a Fallback pack has a Regular + a SemiBold) and register every role from it.
    std::vector<std::string> files;       // file name -> index into g_owned
    std::vector<size_t>      owned_index;
    auto buffer_for = [&](const std::string& file, const uint8_t** b, size_t* l) -> bool {
        for (size_t i = 0; i < files.size(); ++i) {
            if (files[i] == file) { *b = g_owned[owned_index[i]].data(); *l = g_owned[owned_index[i]].size(); return true; }
        }
        const uint8_t* src = nullptr; size_t srclen = 0;
        if (!provider(locale.c_str(), file.c_str(), &src, &srclen, user) || !src || srclen == 0) {
            fprintf(stderr, "ss_load_locale: provider failed for %s/%s\n", locale.c_str(), file.c_str());
            return false;
        }
        g_owned.emplace_back(src, src + srclen);
        files.push_back(file);
        owned_index.push_back(g_owned.size() - 1);
        *b = g_owned.back().data();
        *l = g_owned.back().size();
        return true;
    };

    if (entry.contains("fonts")) {
        for (const auto& f : entry["fonts"]) {
            const std::string role = f.value("role", std::string());
            const std::string file = f.value("file", std::string());
            const int px = f.value("px", 0);
            const uint8_t* b = nullptr; size_t l = 0;
            if (!buffer_for(file, &b, &l)) { ss_unload_locale(); return false; }
            if (!seedsigner_register_font(role.c_str(), b, l, px)) {
                fprintf(stderr, "ss_load_locale: register failed for %s role '%s'\n", locale.c_str(), role.c_str());
                ss_unload_locale();
                return false;
            }
        }
    }

    // Complex-script locales ship a pre-shaped glyph-run table (the compact binary
    // runs.bin) next to the .ttf. Its bytes are copied by seedsigner_set_glyph_runs,
    // so they need not persist.
    if (entry.value("shaping", false)) {
        const uint8_t* b = nullptr; size_t l = 0;
        if (!provider(locale.c_str(), "runs.bin", &b, &l, user) || !b || l == 0) {
            fprintf(stderr, "ss_load_locale: provider failed for %s/runs.bin\n", locale.c_str());
            ss_unload_locale();
            return false;
        }
        if (!seedsigner_set_glyph_runs((const char*)b, l)) {
            fprintf(stderr, "ss_load_locale: bad runs.bin for %s\n", locale.c_str());
            ss_unload_locale();
            return false;
        }
    }
    return true;
}

const char* ss_locale_pack_files(const char* locale_c) {
    static std::string out;
    const std::string locale = locale_c ? locale_c : "";
    json files = json::array();
    json entry;
    if (!locale.empty() && find_locale_entry(locale, entry)) {
        std::vector<std::string> seen;
        if (entry.contains("fonts")) {
            for (const auto& f : entry["fonts"]) {
                const std::string file = f.value("file", std::string());
                if (std::find(seen.begin(), seen.end(), file) == seen.end()) {
                    seen.push_back(file);
                    files.push_back(file);
                }
            }
        }
        if (entry.value("shaping", false)) files.push_back("runs.bin");
    }
    out = files.dump();
    return out.c_str();
}
