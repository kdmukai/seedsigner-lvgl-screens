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

// Render a screen whose body is a vertical list of buttons, optionally preceded
// by an intro-text block (`cfg["text"]`).
//
// The scaffold builds the buttons from `cfg["button_list"]`; when `cfg["text"]`
// is present it also gives us a separate `upper_body` (above the buttons) for the
// text. This function validates the required key, renders any intro text, wires
// navigation, and loads the screen. When intro text overflows the viewport,
// bind_screen_navigation auto-enables scroll-then-buttons joystick navigation:
// the text scrolls into view before the first button takes focus.
void button_list_screen(void *ctx_json)
{
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array()) {
        throw std::runtime_error("button_list is required and must be an array");
    }

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

    // Optional intro text above the buttons. The scaffold gave us a separate
    // upper_body (Mode 4) whenever cfg["text"] is a non-empty string; render the
    // text into it. Wraps to the upper_body content width and uses the standard
    // body font/color, with the SAME tight, ink-based line spacing as the
    // large_icon_status_screen body (not the screen-wide loose default) — otherwise
    // the taller block can tip a short prompt into a marginal overflow that wrongly
    // trips scroll-then-buttons, leaving no button highlighted on load.
    if (cfg.contains("text") && cfg["text"].is_string()) {
        std::string text = cfg["text"].get<std::string>();
        if (!text.empty() && screen.upper_body && screen.upper_body != screen.body) {
            lv_obj_t *intro_label = make_body_text_label(
                screen.upper_body, text.c_str(),
                lv_obj_get_content_width(screen.upper_body));
            apply_body_tight_line_spacing(intro_label);
        }
    }

    bind_screen_navigation(
        cfg,
        screen,
        // Default to button index 0 selected when nothing is passed in (an explicit
        // initial_selected_index still overrides). A concrete index keeps a button
        // active even when intro text makes the list overflow — a menu/list always
        // has a selection. (cf. status screens, which pass NAV_INDEX_NONE.)
        0
    );

    load_screen_and_cleanup_previous(screen.screen);
}
