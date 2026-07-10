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

// ---------------------------------------------------------------------------
// seed_finalize_screen
// ---------------------------------------------------------------------------
//
// The "Finalize Seed" step shown immediately after a seed is loaded (the LVGL port
// of Python's SeedFinalizeScreen, seed_screens.py). Structurally it is a
// bottom-pinned ButtonListScreen (Done / BIP-39 Passphrase, no back button); its
// one special element is a centered fingerprint readout — the master fingerprint
// hex beneath a small "fingerprint" label, beside a large blue fingerprint icon.
//
// The readout ports Python's IconTextLine(is_text_centered=True,
// icon_name=FINGERPRINT): a horizontally-centered group of
//     [ fingerprint icon ] [gap] [ label (top) / value (bottom) ]
// with the label + value left-aligned to a shared x and the icon vertically
// centered against that two-line block. Python centers the whole group vertically
// between the top-nav and the first button; here the scaffold's flex body does that
// (see the upper_body grow below).
//
// cfg:
//   top_nav: { title (default "Finalize Seed"), show_power_button }. show_back_button
//            is forced false (Python SeedFinalizeScreen.show_back_button = False).
//   fingerprint (string, required): the master-fingerprint hex to display.
//   fingerprint_label (string, optional): the small label above the value; defaults
//            to "fingerprint" (translated upstream by the View / scenario localizer).
//   button_list (array): the action buttons (e.g. ["Done", "BIP-39 Passphrase"]).
//            Defaults to ["Done"] when absent.
void seed_finalize_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // fingerprint (required); the small label defaults to the (English) Python string.
    if (!cfg.contains("fingerprint") || !cfg["fingerprint"].is_string()) {
        throw std::runtime_error("seed_finalize_screen requires a \"fingerprint\" string");
    }
    std::string fingerprint       = cfg["fingerprint"].get<std::string>();
    std::string fingerprint_label = cfg.value("fingerprint_label", std::string("fingerprint"));

    // Force the SeedFinalizeScreen shape onto the scaffold cfg: a titled,
    // back-button-less, bottom-pinned button list. The View supplies the localized
    // title + button_list; default both so a bare cfg still renders.
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Finalize Seed";
    cfg["top_nav"]["show_back_button"] = false;    // Python: show_back_button = False
    cfg["is_bottom_list"] = true;                  // Python: is_bottom_list = True
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Done" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // Python centers the fingerprint IconTextLine in the gap between the top-nav and
    // the first button. The scaffold's bottom-list body is a flex column
    // [upper_body][spacer grow=1][buttons]; make upper_body itself the grower and
    // center its child on the main (vertical) axis, then zero the scaffold spacer so
    // upper_body claims the whole gap. Result: the readout sits vertically centered
    // above the buttons.
    lv_obj_set_flex_grow(screen.upper_body, 1);
    lv_obj_set_flex_align(screen.upper_body, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (screen.button_list_spacer) lv_obj_set_flex_grow(screen.button_list_spacer, 0);

    // Fingerprint readout via the shared IconTextLine component — the SAME widget the
    // xpub-details / review-passphrase screens use, so labeled-value spacing (gap, leading
    // reclaim) is identical across every screen. Large blue fingerprint glyph, gray label
    // over the Latin body+2 hex; upper_body's center alignment centers the whole block.
    icon_text_line_opts_t fp = {};
    fp.icon_glyph   = SeedSignerIconConstants::FINGERPRINT;
    fp.icon_font    = &ICON_LARGE_BUTTON_FONT__SEEDSIGNER;   // Python icon_size = ICON_FONT_SIZE+12 (~36)
    fp.icon_color   = INFO_COLOR;
    fp.label_text   = fingerprint_label.c_str();
    fp.value_text   = fingerprint.c_str();
    fp.value_font   = seedsigner_latin_font(19);             // Python value = body+2, always Latin
    fp.label_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    fp.value_color  = SEEDSIGNER_ICON_COLOR_DEFAULT;
    fp.is_text_centered = true;
    icon_text_line(screen.upper_body, &fp);

    bind_screen_navigation(
        cfg,
        screen,
        0   // default the first action button (Done) selected, like button_list_screen
    );

    load_screen_and_cleanup_previous(screen.screen);
}
