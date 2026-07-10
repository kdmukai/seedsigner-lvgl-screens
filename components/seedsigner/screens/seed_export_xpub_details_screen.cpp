#include "seedsigner.h"
#include "screen_scaffold.h"
#include "screen_helpers.h"
#include "components.h"
#include "camera_preview_overlay.h"
#include "camera_entropy_overlay.h"
#include "keyboard_core.h"
#include "gui_constants.h"
#include "navigation.h"
#include "input_profile.h"
#include "font_registry.h"
#include "glyph_runs.h"
#include "locale_loader.h"
#include "locale_picker.h"
#include "overlay_manager.h"

#include "lvgl.h"

#if LV_USE_QRCODE
#include "../../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"
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
#include <esp_heap_caps.h>
#endif

using json = nlohmann::json;

// SeedExportXpubDetailsScreen: the xpub-export summary — fingerprint, derivation path,
// and the (truncated) xpub, each an IconTextLine, stacked top-anchored under the nav.
// WarningEdgesMixin frames it in pulsing YELLOW (WARNING_COLOR): an xpub leaks viewable
// transaction history — a privacy caution, not a key-material dire warning.
//
// cfg:
//   top_nav.title             — default "Xpub Details".
//   fingerprint (str, req.)   — BIP-32 master fingerprint (hex).
//   derivation_path (str)     — default "m/84'/0'/0'".
//   xpub (str, req.)          — the extended pubkey; truncated here to one line.
//   fingerprint_label / derivation_label / xpub_label (str) — host-localized field labels.
//   button_list (array)       — default ["Export xpub"].
void seed_export_xpub_details_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    if (!cfg.contains("fingerprint") || !cfg["fingerprint"].is_string()) {
        throw std::runtime_error("seed_export_xpub_details_screen requires a \"fingerprint\" string");
    }
    if (!cfg.contains("xpub") || !cfg["xpub"].is_string()) {
        throw std::runtime_error("seed_export_xpub_details_screen requires an \"xpub\" string");
    }
    std::string fingerprint       = cfg["fingerprint"].get<std::string>();
    std::string derivation_path   = cfg.value("derivation_path", std::string("m/84'/0'/0'"));
    std::string xpub              = cfg["xpub"].get<std::string>();
    std::string fingerprint_label = cfg.value("fingerprint_label", std::string("Fingerprint"));
    std::string derivation_label  = cfg.value("derivation_label",  std::string("Derivation"));
    std::string xpub_label        = cfg.value("xpub_label",        std::string("Xpub"));

    // Bottom-pinned button-list shape (Python is_bottom_list = True).
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Xpub Details";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Export xpub" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // Top-anchored, LEFT-aligned column of the three lines (Python: screen_x =
    // COMPONENT_PADDING, first line COMPONENT_PADDING below the nav). Grow upper_body to
    // claim the whole gap above the button and collapse the scaffold spacer, so the block
    // sits in a container sized to fit rather than a shrink-wrapped one that scrolls its top
    // under the nav. icon_text_line reclaims the LVGL line-height leading so three lines pack
    // at PIL density; the inter-line gap is COMPONENT_PADDING (Python's 1.5x would leave the
    // xpub line hugging the button on the 240 body).
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_left(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    lv_obj_set_style_pad_row(screen.upper_body, COMPONENT_PADDING, LV_PART_MAIN);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // Fingerprint line — seedsigner FINGERPRINT glyph, body-font value.
    icon_text_line_opts_t fp = {};
    fp.icon_glyph   = SeedSignerIconConstants::FINGERPRINT;
    fp.icon_color   = INFO_COLOR;
    fp.label_text   = fingerprint_label.c_str();
    fp.value_text   = fingerprint.c_str();
    fp.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> LABEL_FONT_COLOR (gray)
    fp.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BODY_FONT_COLOR
    fp.icon_width   = ICON_FONT_SIZE;                  // shared icon column -> aligned text
    icon_text_line(screen.upper_body, &fp);

    // Derivation line — seedsigner DERIVATION glyph, body-font value.
    icon_text_line_opts_t dv = {};
    dv.icon_glyph   = SeedSignerIconConstants::DERIVATION;
    dv.icon_color   = INFO_COLOR;
    dv.label_text   = derivation_label.c_str();
    dv.value_text   = derivation_path.c_str();
    dv.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    dv.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    dv.icon_width   = ICON_FONT_SIZE;
    icon_text_line(screen.upper_body, &dv);

    // Xpub line. Python's icon is FontAwesomeIconConstants.X == the ASCII letter "X" drawn
    // in the bold FontAwesome font; render it as a bold monospace "X". The value is the
    // xpub in the fixed-width font (Python FIXED_WIDTH at body+2; the baked 22 px monospace
    // here), truncated to fill one line. Truncation is measured HERE so it tracks each
    // display profile's char width — Python's num_chars math.
    const lv_font_t *xpub_font = &CANDIDATE_FONT;   // Inconsolata SemiBold, 22 px @240
    lv_point_t sz10;
    lv_text_get_size(&sz10, "0000000000", xpub_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t char_w = sz10.x / 10;
    if (char_w < 1) char_w = 1;
    const int32_t W = lv_display_get_horizontal_resolution(NULL);
    int num_chars = (int)((W - ICON_FONT_SIZE - 2 * COMPONENT_PADDING) / char_w) - 3;  // -3 = "..."
    if (num_chars < 1) num_chars = 1;
    if (num_chars > (int)xpub.size()) num_chars = (int)xpub.size();
    std::string xpub_display = xpub.substr(0, (size_t)num_chars) + "...";

    icon_text_line_opts_t xp = {};
    xp.icon_glyph   = "X";                 // Python FontAwesomeIconConstants.X = U+0058
    xp.icon_font    = &KEYBOARD_FONT;      // bold 24 px monospace X
    xp.icon_color   = INFO_COLOR;
    xp.label_text   = xpub_label.c_str();
    xp.value_text   = xpub_display.c_str();
    xp.value_font   = xpub_font;
    xp.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    xp.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    xp.icon_width   = ICON_FONT_SIZE;      // same column as fingerprint/derivation -> "X" centers, text aligns
    icon_text_line(screen.upper_body, &xp);

    // WarningEdgesMixin — pulsing yellow border.
    add_warning_edges_overlay(screen.screen, WARNING_COLOR);

    bind_screen_navigation(cfg, screen, 0);
    load_screen_and_cleanup_previous(screen.screen);
}
