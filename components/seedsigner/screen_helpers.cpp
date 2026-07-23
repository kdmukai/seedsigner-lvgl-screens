// screen_helpers.cpp — definitions for the cross-cutting render/layout helpers declared
// in screen_helpers.h (button-list readers, status-type tables, text-measure/line-spacing,
// hex-color + network-color parsing, btc_amount builder) plus the static-render mode flag.
// Shared by the per-screen TUs under screens/ and by screen_scaffold.cpp.

#include "seedsigner.h"
#include "screen_scaffold.h"  // scaffold entry-point declarations (defined in screen_scaffold.cpp)
#include "screen_helpers.h"   // declarations for the helpers defined in this file
#include "qr_core.h"          // shared QR encode/decode core + gutter close button (qr_display + transcribe)
#include "components.h"
#include "camera_preview_overlay.h"
#include "camera_entropy_overlay.h"
#include "keyboard_core.h"
#include "gui_constants.h"
#include "navigation.h"
#include "input_profile.h"
#include "font_registry.h"
#include "glyph_runs.h"
#include "locale_loader.h"   // ss_reap_retired() after the old screen is deleted
#include "locale_picker.h"   // endonym-image rows for settings_locale_picker_screen
#include "overlay_manager.h" // SS_OBJ_FLAG_NO_SCREENSAVER (per-screen saver policy)

#include "lvgl.h"

// Nayuki qrcodegen is BUNDLED inside the LVGL submodule (used by lv_qrcode). We call
// it directly (not the lv_qrcode widget) so we control ECC level (L), mode (numeric for
// SeedQR / byte for the rest), and a fixed quiet zone — see qr_display_screen below.
// Gated by LV_USE_QRCODE: qrcodegen.c only compiles when the flag is set.
#if LV_USE_QRCODE
#include "../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"
#endif

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <set>
#include <map>
#include <algorithm>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>   // psram_alloc: route PSBT-overview geometry off the internal heap
#endif

using json = nlohmann::json;



// Switch to a newly built LVGL screen and dispose of the old one.
//
// Rationale:
// - Every screen render path allocates a fresh root screen (`lv_obj_create(NULL)`).
// - If we do not delete the previous root, those widget trees remain allocated and
//   accumulate over repeated navigations/renders.
// - We only delete after `lv_scr_load(new_screen)` so LVGL has already switched the
//   active screen; this avoids deleting the currently active screen too early.
// - The `old_screen != new_screen` guard is a safety check for accidental reuse.
// Recursively force RTL text direction on every text LABEL in a finished screen
// tree (for RTL locales). This is the ONE global place direction is applied, so
// the screen/component builders stay direction-agnostic — no per-label or
// per-builder maintenance.
//
// Why labels only (not the screen root): LVGL's base_dir is a single inherited
// property that drives BOTH bidi text direction AND element layout (flex order,
// and the coordinate origin for lv_obj_set_pos / lv_obj_align). Setting it RTL at
// the root would mirror the layout too — the Scan tile, the nav buttons, the
// passphrase cursor all flip. Scoping it to lv_label objects gives correct RTL
// text while every container keeps its physical left-to-right arrangement.
//
// lv_textarea subtrees are skipped: the passphrase entry is always ASCII/LTR, so
// its box and cursor must stay left-to-right. The on-screen keyboard is a
// buttonmatrix (no child labels), so it is unaffected.
void apply_rtl_text_to_labels(lv_obj_t *obj) {
    if (lv_obj_check_type(obj, &lv_textarea_class)) return;  // user input stays LTR
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_obj_set_style_base_dir(obj, LV_BASE_DIR_RTL, 0);
    }
    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; ++i) {
        apply_rtl_text_to_labels(lv_obj_get_child(obj, i));
    }
}



// Build the standard "body" container used by screens beneath TopNav.
// Most screens share the same layout/styling shell (size, alignment, padding, background,
// border, and scrollbar baseline behavior). This function encapsulates that common
// boilerplate.
lv_obj_t* create_standard_body_content(lv_obj_t *screen, lv_obj_t *top_nav_obj, bool scrollable) {
    lv_obj_t* body_content = lv_obj_create(screen);

    // The body sits directly below the full top_nav (TOP_NAV_HEIGHT tall) and clips
    // at TOP_NAV_HEIGHT. The top_nav vertically centers its buttons, so the space
    // between a button's bottom edge and TOP_NAV_HEIGHT is a visual buffer OWNED BY
    // THE TOP_NAV: scrolling body content clips at TOP_NAV_HEIGHT and never renders
    // up against the back/power buttons — the buffer stays between the nav and the
    // moving content. (An earlier revision pushed this buffer down into the body so
    // a screen could overlap an element up into the nav, but that let scrolled
    // content collide with the nav buttons; the buffer belongs to the top_nav.)
    lv_obj_set_size(body_content, lv_obj_get_width(screen),
                    lv_obj_get_height(screen) - TOP_NAV_HEIGHT);
    lv_obj_align_to(body_content, top_nav_obj, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(body_content, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_pad_left(body_content, EDGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_right(body_content, EDGE_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_top(body_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(body_content, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_border_width(body_content, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(body_content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_content, scrollable ? LV_SCROLLBAR_MODE_AUTO : LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_right(body_content, 0, LV_PART_SCROLLBAR);
    return body_content;
}


void nav_mode_override_from_cfg(const json &cfg, bool &has_override, input_mode_t &mode_override) {
    has_override = false;
    mode_override = INPUT_MODE_TOUCH;

    if (!(cfg.contains("input") && cfg["input"].is_object() && cfg["input"].contains("mode") && cfg["input"]["mode"].is_string())) {
        return;
    }

    std::string mode = cfg["input"]["mode"].get<std::string>();
    if (mode == "touch") {
        has_override = true;
        mode_override = INPUT_MODE_TOUCH;
    } else if (mode == "hardware") {
        has_override = true;
        mode_override = INPUT_MODE_HARDWARE;
    }
}


size_t nav_initial_index_from_cfg(const json &cfg, size_t default_index) {
    if (cfg.contains("initial_selected_index") && cfg["initial_selected_index"].is_number_integer()) {
        int idx = cfg["initial_selected_index"].get<int>();
        if (idx >= 0) {
            return (size_t)idx;
        }
    }
    return default_index;
}


// Read a button_list array from cfg into a vector of label strings.
// Accepts either bare strings or `[label, ...]` arrays whose first element is a
// string label (Python's ButtonOption tuple shape). Returns true if the key was
// present and well-formed; false (with `out` cleared) if the key is missing.
// Throws on a malformed entry so screens fail fast on bad JSON.
bool read_button_list_labels(const json &cfg, std::vector<std::string> &out) {
    out.clear();
    if (!cfg.contains("button_list")) {
        return false;
    }
    if (!cfg["button_list"].is_array()) {
        throw std::runtime_error("button_list must be an array");
    }

    for (const auto &entry : cfg["button_list"]) {
        if (entry.is_string()) {
            out.push_back(entry.get<std::string>());
        } else if (entry.is_array() && !entry.empty() && entry[0].is_string()) {
            out.push_back(entry[0].get<std::string>());
        } else if (entry.is_object() && entry.contains("label") && entry["label"].is_string()) {
            // Object form (per-button icons) — the label is all this reader needs.
            out.push_back(entry["label"].get<std::string>());
        } else {
            throw std::runtime_error("button_list entries must be string, array, or object with a string label");
        }
    }
    return true;
}


// Parse a JSON color string ("#RRGGBB", "0xRRGGBB", or "RRGGBB") to a 0xRRGGBB int.
// Throws on a malformed value so bad cfg fails fast (same convention as the rest of
// the screen-config parser).
uint32_t parse_hex_color(const std::string &s) {
    std::string h = s;
    if (h.rfind("#", 0) == 0) {
        h = h.substr(1);
    } else if (h.rfind("0x", 0) == 0 || h.rfind("0X", 0) == 0) {
        h = h.substr(2);
    }
    if (h.size() != 6) {
        throw std::runtime_error("color must be a 6-digit hex string like \"#30D158\"");
    }
    for (char c : h) {
        if (!std::isxdigit((unsigned char)c)) {
            throw std::runtime_error("color must be a 6-digit hex string like \"#30D158\"");
        }
    }
    return (uint32_t)std::strtoul(h.c_str(), nullptr, 16);
}


// Richer button_list reader: parallels read_button_list_labels but also captures the
// per-item inline icon / right icon / icon color when an entry is an OBJECT. Accepts:
//   - "Label"                                                  → label only
//   - ["Label", ...]                                           → label at index 0
//   - {"label","icon"?,"icon_color"?,"right_icon"?}            → label + inline icons
// Returns true if "button_list" was present and well-formed; false (out cleared) if
// missing. Throws on a malformed entry so screens fail fast.
// button_item_cfg_t is defined in screen_helpers.h (shared with screens that move out).
bool read_button_list_items(const json &cfg, std::vector<button_item_cfg_t> &out) {
    out.clear();
    if (!cfg.contains("button_list")) {
        return false;
    }
    if (!cfg["button_list"].is_array()) {
        throw std::runtime_error("button_list must be an array");
    }

    for (const auto &entry : cfg["button_list"]) {
        button_item_cfg_t item;
        if (entry.is_string()) {
            item.label = entry.get<std::string>();
        } else if (entry.is_array() && !entry.empty() && entry[0].is_string()) {
            item.label = entry[0].get<std::string>();
        } else if (entry.is_object()) {
            if (!entry.contains("label") || !entry["label"].is_string()) {
                throw std::runtime_error("button_list object entry requires a string \"label\"");
            }
            item.label = entry["label"].get<std::string>();
            if (entry.contains("icon")) {
                if (!entry["icon"].is_string()) {
                    throw std::runtime_error("button_list \"icon\" must be a string");
                }
                item.icon = entry["icon"].get<std::string>();
            }
            if (entry.contains("right_icon")) {
                if (!entry["right_icon"].is_string()) {
                    throw std::runtime_error("button_list \"right_icon\" must be a string");
                }
                item.right_icon = entry["right_icon"].get<std::string>();
            }
            if (entry.contains("icon_color")) {
                if (!entry["icon_color"].is_string()) {
                    throw std::runtime_error("button_list \"icon_color\" must be a string");
                }
                item.icon_color = parse_hex_color(entry["icon_color"].get<std::string>());
            }
            if (entry.contains("label_color")) {
                if (!entry["label_color"].is_string()) {
                    throw std::runtime_error("button_list \"label_color\" must be a string");
                }
                item.label_color = parse_hex_color(entry["label_color"].get<std::string>());
            }
        } else {
            throw std::runtime_error("button_list entries must be a string, array, or object with a string \"label\"");
        }
        out.push_back(item);
    }
    return true;
}



// Forward decl: tight body line-spacing helper (defined after tight_line_space).
// Non-static so sibling screen TUs (e.g. seed_sign_message_confirm_message_screen.cpp)
// can reuse the exact PIL-matched inter-line advance for their own body labels.
void apply_body_tight_line_spacing(lv_obj_t *label);

// Create a standard wrapped body-text label in `parent`: WRAP, fixed `width`,
// centered, BODY_FONT in BODY_FONT_COLOR. Shared by the button_list_screen intro
// text and the status-screen body (which then layer on tight line spacing via
// apply_body_tight_line_spacing(), plus any inset width / centering). Caller owns
// any further styling.
lv_obj_t *make_body_text_label(lv_obj_t *parent, const char *text, int32_t width) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);
    return label;
}







// ---------------------------------------------------------------------------
// Status-type trio: the status-screen family's shared config layer.
//
// status_type_t / status_type_defaults_t are defined in screen_helpers.h.
// defaults_for_status_type() is the per-status lookup table — hero icon glyph,
// status color, reserved default title / button label, warning-edges default,
// and text-inset multiplier. parse_status_type() reads and validates
// cfg["status_type"] against the enumerated values. apply_status_type_defaults()
// injects the table's defaults into a screen cfg (and forces is_bottom_list)
// before the scaffold reads it. Consumed by large_icon_status_screen — see that
// file's banner for the screen contract; Python's Success / Warning /
// DireWarning / Error variants all funnel through this table.
// ---------------------------------------------------------------------------

status_type_defaults_t defaults_for_status_type(status_type_t st) {
    switch (st) {
        case status_type_t::SUCCESS:
            return { SeedSignerIconConstants::SUCCESS, SUCCESS_COLOR,
                     "Success!",   "OK",            false, 1 };
        case status_type_t::WARNING:
            return { SeedSignerIconConstants::WARNING, WARNING_COLOR,
                     "Caution",    "I understand",  true,  2 };
        case status_type_t::DIRE_WARNING:
            return { SeedSignerIconConstants::WARNING, DIRE_WARNING_COLOR,
                     "Caution",    "I understand",  true,  2 };
        case status_type_t::ERROR:
            return { SeedSignerIconConstants::ERROR,   ERROR_COLOR,
                     "Error",      "I understand",  true,  2 };
        case status_type_t::CUSTOM:
            // The icon glyph, color, and title all come from cfg (filled in by
            // large_icon_status_screen); these are only fallbacks for a bare cfg. No
            // forced title (default ""), a plain "OK" button, no warning border, and the
            // 1x text inset of a non-warning screen.
            return { SeedSignerIconConstants::INFO, INFO_COLOR,
                     "",           "OK",            false, 1 };
    }
    // Unreachable; silences -Wreturn-type on some compilers.
    return { SeedSignerIconConstants::SUCCESS, SUCCESS_COLOR, "", "", false, 1 };
}


status_type_t parse_status_type(const json &cfg) {
    if (!cfg.contains("status_type") || !cfg["status_type"].is_string()) {
        throw std::runtime_error("status_type is required and must be one of "
                                 "\"success\", \"warning\", \"dire_warning\", \"error\"");
    }
    std::string value = cfg["status_type"].get<std::string>();
    if (value == "success")      return status_type_t::SUCCESS;
    if (value == "warning")      return status_type_t::WARNING;
    if (value == "dire_warning") return status_type_t::DIRE_WARNING;
    if (value == "error")        return status_type_t::ERROR;
    // "custom": the caller supplies the icon glyph + color at call time (Python's
    // PSBTFinalizeScreen's SIGN icon, the microSD-notification screen, etc.), so this
    // one screen covers any large-icon prompt without a bespoke entry point.
    if (value == "custom")       return status_type_t::CUSTOM;
    throw std::runtime_error("status_type must be one of \"success\", "
                             "\"warning\", \"dire_warning\", \"error\", \"custom\"");
}


// Inject status-type defaults into cfg before the scaffold reads it.
//
// Mirrors the Python class hierarchy: `top_nav.title`, `button_list`, and
// `is_bottom_list` get sensible per-status defaults if the JSON omits them.
// `is_bottom_list` is forced to true for status screens, matching
// LargeIconStatusScreen.__post_init__.
//
// NOTE (rollout): the English content defaults below (title, button label)
// predate the content policy (docs/screen-conformance-spec.md) — user-visible
// text must come localized from the host, so when the status family goes
// through conformance these become require-and-throw and the structural parts
// rebuild on ensure_top_nav_structure. Unchanged until then.
void apply_status_type_defaults(json &cfg, const status_type_defaults_t &defaults) {
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) {
        cfg["top_nav"] = json::object();
    }
    if (!cfg["top_nav"].contains("title")) {
        cfg["top_nav"]["title"] = defaults.default_top_nav_title;
    }
    if (!cfg.contains("button_list")) {
        cfg["button_list"] = json::array({ defaults.default_button_label });
    }

    // LargeIconStatusScreen always pins the button to the bottom.
    cfg["is_bottom_list"] = true;
}


// ---------------------------------------------------------------------------
// top_nav chrome normalization (structural defaults + content validation).
//
// Content policy (docs/screen-conformance-spec.md): user-visible strings —
// including the top-nav title — arrive LOCALIZED from the host view layer via
// cfg; a screen never injects an English content default. Structural flags
// (never rendered as text) keep write-if-absent defaults — a host-provided
// value always wins. Forced (non-defaulted) overrides — e.g. unconditionally
// hiding the back button — stay explicit assignments at the call site AFTER
// the call, preserving each screen's guarded-vs-forced host-override contract
// instead of burying it in parameters.

void ensure_top_nav_structure(json &cfg,
                              bool default_show_back_button, bool default_show_power_button) {
    // Match the inline blocks this replaces: an absent OR non-object top_nav is
    // replaced with a fresh object (a non-object would throw in the scaffold).
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) {
        cfg["top_nav"] = json::object();
    }

    if (!cfg["top_nav"].contains("show_back_button")) {
        cfg["top_nav"]["show_back_button"] = default_show_back_button;
    }
    if (!cfg["top_nav"].contains("show_power_button")) {
        cfg["top_nav"]["show_power_button"] = default_show_power_button;
    }
}


// Missing localized content is a developer error in the host view layer —
// surface it as a screen-named throw, never patch over it with English text.
void require_top_nav_title(const json &cfg, const char *screen_name) {
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object() ||
        !cfg["top_nav"].contains("title") || !cfg["top_nav"]["title"].is_string()) {
        throw std::runtime_error(std::string(screen_name) +
                                 ": top_nav.title is required (localized content comes from the host)");
    }
}


// ---------------------------------------------------------------------------
// Tight, ink-based inter-line spacing.
//
// Walk `text` (UTF-8) and return the maximum ink ascent and descent — the tallest
// distance above and the deepest below the baseline — over every visible glyph in
// `font`. Whitespace and absent/inkless glyphs are skipped. Both tight_line_space
// and text_top_leading derive their metrics from this single pass (text_top_leading
// uses only the ascent). Either out-param may be null.
void measure_text_ink_extents(const lv_font_t *font, const char *text,
                                     int32_t *out_max_ascent, int32_t *out_max_descent) {
    int32_t max_ascent  = 0;   // ink above the baseline
    int32_t max_descent = 0;   // ink below the baseline

    // Minimal UTF-8 walk; ask the font engine for each glyph's ink box.
    for (const unsigned char *p = (const unsigned char *)text; *p; ) {
        uint32_t cp;
        if (*p < 0x80) {
            cp = *p; p += 1;
        } else if ((*p >> 5) == 0x6 && p[1]) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
        } else if ((*p >> 4) == 0xE && p[1] && p[2]) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3;
        } else if ((*p >> 3) == 0x1E && p[1] && p[2] && p[3]) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4;
        } else {
            p += 1; continue;
        }

        if (cp == '\n' || cp == '\r' || cp == ' ') {
            continue;
        }

        lv_font_glyph_dsc_t d;
        if (!lv_font_get_glyph_dsc(font, &d, cp, 0) || d.box_h == 0) {
            continue;   // absent or inkless (whitespace)
        }

        int32_t ascent  = (int32_t)d.ofs_y + (int32_t)d.box_h;  // top, above baseline
        int32_t descent = -(int32_t)d.ofs_y;                    // bottom, below baseline
        if (ascent  > max_ascent)  max_ascent  = ascent;
        if (descent > max_descent) max_descent = descent;
    }

    if (out_max_ascent)  *out_max_ascent  = max_ascent;
    if (out_max_descent) *out_max_descent = max_descent;
}


// LVGL's declared font line_height carries loose leading and varies wildly
// across scripts (Arabic/Farsi fonts reserve large vertical space for stacked
// marks), so using it as the line advance leaves multi-line body text far
// looser than the PIL/Python reference. Instead we derive the advance from the
// ACTUAL ink extents of the glyphs present in `text` -- the tallest ascender
// plus the deepest descender -- and add only a small visual `gap`. The result
// is the value to hand lv_obj_set_style_text_line_space() (the rendered advance
// is font line_height + this), so it is usually NEGATIVE (tightening).
//
// Worst-case safe for a run of text: the maximum ascender (which may land on a
// lower line) and the maximum descender (which may land on the line above it)
// are kept at least `gap` px apart. Complex cursive scripts with a cascading
// baseline (Urdu Nastaliq) are not well served by a single constant advance and
// render through their own (shaped) path, not this one.
int32_t tight_line_space(const lv_font_t *font, const char *text, int32_t gap) {
    if (!font || !text) {
        return 0;
    }

    int32_t max_ascent = 0, max_descent = 0;
    measure_text_ink_extents(font, text, &max_ascent, &max_descent);

    if (max_ascent + max_descent <= 0) {
        return 0;   // measured nothing; leave the label's default spacing alone
    }

    int32_t line_h  = (int32_t)lv_font_get_line_height(font);
    int32_t advance = max_ascent + max_descent + gap;
    int32_t space   = advance - line_h;

    // Safety floor on how far we tighten. Per-codepoint measurement is exact for
    // simple LTR scripts but can UNDER-count scripts whose on-screen glyphs differ
    // from their source codepoints (Arabic/Farsi presentation-form shaping):
    // there the measured ink is far too small and an unclamped advance would
    // collapse the lines on top of each other. Never remove more than a quarter
    // of the font's declared line_height — enough to strip loose leading on
    // well-measured fonts, but a guard against collapse on the rest.
    // NOTE: callers in large_icon_status_screen now pass lv_label_get_text() (the
    // AP-processed presentation forms actually drawn), so fa measures correctly and
    // no longer hits this floor — it is now insurance against future divergence,
    // not the active correction it once was for fa.
    int32_t min_space = -(line_h / 4);
    if (space < min_space) {
        space = min_space;
    }
    return space;
}


// Apply the status-screen body's tight, ink-based inter-line spacing to an already-
// created body-text label: zero top margin + a per-line advance derived from THIS
// label's real glyph ink (max ascender + max descender + a small profile-scaled
// gap) via tight_line_space(), rather than the font's loose declared line_height.
//
// Shared by the large_icon_status_screen body and the button_list_screen intro
// text so both render identically and match the PIL reference. Measures the label's
// STORED text so Arabic/Persian presentation forms (what's actually drawn) are what
// we measure. Without this the intro text inherits the screen-wide (loose)
// BODY_LINE_SPACING, whose taller block can tip a short prompt into a marginal
// overflow — wrongly tripping scroll-then-buttons so no button is highlighted on
// load.
void apply_body_tight_line_spacing(lv_obj_t *label) {
    if (!label) {
        return;
    }
    // Python places the body immediately after the headline (no extra gap) — so no
    // top margin; a margin made the headline->body gap visibly looser than Python.
    lv_obj_set_style_margin_top(label, 0, LV_PART_MAIN);

    int32_t line_gap = LIST_ITEM_PADDING / 2;
    lv_obj_set_style_text_line_space(
        label,
        tight_line_space(&BODY_FONT, lv_label_get_text(label), line_gap),
        LV_PART_MAIN);
}


// Empty vertical space between a label's box top and the VISIBLE top of its text
// — the font's ascent minus the text's real ink ascent. LVGL anchors text by the
// font's ascent (which includes leading above the caps); PIL/Python anchors the
// visible glyph top. Subtract this from a top margin so the visible text lands
// where Python places it, instead of the label's (taller) box.
int32_t text_top_leading(const lv_font_t *font, const char *text) {
    if (!font || !text) {
        return 0;
    }
    int32_t max_ascent = 0;
    measure_text_ink_extents(font, text, &max_ascent, nullptr);
    if (max_ascent <= 0) {
        return 0;
    }
    int32_t ascent  = (int32_t)lv_font_get_line_height(font) - (int32_t)font->base_line;
    int32_t leading = ascent - max_ascent;
    return leading > 0 ? leading : 0;
}


// Balanced wrap for a codepoint-rendered (subset/Latin) wrapped label: shrink its
// column to the SMALLEST width that still produces the same number of lines —
// floored at half the original width — so greedy wrapping fills the lines more
// evenly and a lone trailing word gets pulled up. Width-only: the line count, and
// therefore the label's height, is unchanged, so this composes with the vertical
// centering / reclaim done by the caller. The search is pure metric arithmetic
// over already-loaded glyph advances (lv_text_get_size: no rasterization, no
// re-shaping), run once at screen build — negligible cost. Shaped glyph-run
// locales (hi/th) wrap in the render layer (glyph_runs.cpp) and are balanced
// there instead; this path must not touch them (its codepoint measurement would
// not match their shaped line-breaking).
void balance_wrapped_label_column(lv_obj_t *label) {
    if (!label) return;
    const char *text = lv_label_get_text(label);
    if (!text || !text[0]) return;

    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    int32_t letter_space  = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    int32_t line_space    = lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
    int32_t w0            = lv_obj_get_width(label);
    if (!font || w0 < 2) return;

    // Reuse the shared line-count binary search (balanced_wrap_width, glyph_runs.h) —
    // the shaped path feeds it glyph-run advances; here the per-trial measure is
    // lv_text_get_size over the codepoint text. LVGL stacks N wrapped lines to height
    // N*(line_h + line_space) - line_space, so the visual line count is exactly
    // (sz.y + line_space) / (line_h + line_space) — the same quantity the old height
    // comparison used, expressed as the count the shared search expects.
    const int32_t line_h = (int32_t)lv_font_get_line_height(font);
    int best = balanced_wrap_width((int)w0, [&](int w, size_t *nlines, int *max_line_w) {
        lv_point_t sz;
        lv_text_get_size(&sz, text, font, letter_space, line_space, w, LV_TEXT_FLAG_NONE);
        *nlines     = (size_t)((sz.y + line_space) / (line_h + line_space));
        *max_line_w = (int)sz.x;
    });
    if (best < (int)w0) lv_obj_set_width(label, (int32_t)best);
}



// Map a network to its accent/highlight color: Bitcoin orange (mainnet), testnet green,
// regtest blue. The JSON contract uses the Python SettingsConstants network codes
// ("M"/"T"/"R"); the legacy long names are also accepted so pre-existing scenarios keep
// working. Shared by btc_amount_from_cfg (the coin icon) and the PSBT detail screens (the
// formatted-address head/tail highlight), so a value and its address carry one color.
uint32_t network_color(const std::string &net) {
    if (net == "T" || net == "testnet") return (uint32_t)TESTNET_COLOR;
    if (net == "R" || net == "regtest") return (uint32_t)REGTEST_COLOR;
    return (uint32_t)ACCENT_COLOR;   // "M" / "mainnet" / default
}


// Build a components.btc_amount from a cfg object: maps network -> icon color (an
// explicit icon_color hex overrides) and forwards the host-formatted display strings.
// Shared by every amount-showing screen (PSBT overview / detail / change).
lv_obj_t *btc_amount_from_cfg(lv_obj_t *parent, const json &j) {
    std::string primary   = j.value("primary", std::string(""));
    std::string secondary = j.value("secondary", std::string(""));
    std::string unit      = j.value("unit", std::string(""));

    btc_amount_opts_t o = {};
    o.primary       = primary.c_str();
    o.secondary     = secondary.empty() ? nullptr : secondary.c_str();
    o.unit          = unit.empty() ? nullptr : unit.c_str();
    o.primary_small = j.value("primary_small", false);

    o.icon_color = network_color(j.value("network", std::string("M")));
    if (j.contains("icon_color") && j["icon_color"].is_string()) {
        o.icon_color = parse_hex_color(j["icon_color"].get<std::string>());
    }
    return btc_amount(parent, &o);
}


// Static-render mode: when enabled, screens render without animations that would
// make a still capture non-deterministic — currently the text-entry cursor is
// shown without blinking. Off by default; the screenshot generator turns it on.
// Live use leaves it off so the cursor blinks normally. Read across screens via the
// seedsigner_lvgl_is_static_render() getter (declared in seedsigner.h).
static bool s_static_render = false;
extern "C" void seedsigner_lvgl_set_static_render(bool enabled) {
    s_static_render = enabled;
}
extern "C" bool seedsigner_lvgl_is_static_render(void) {
    return s_static_render;
}
