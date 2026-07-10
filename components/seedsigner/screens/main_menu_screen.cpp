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

void main_menu_screen(void *ctx_json)
{
    // The home menu's structure is fixed (a 2x2 grid of four icon buttons), but
    // its DISPLAY TEXT — the top-nav title and the four button labels — must
    // localize. So those come from the JSON context (translated upstream by the
    // scenario localizer / Python view layer); the four icons never translate.
    //
    // Defaults below reproduce the original English home menu, so the screen
    // still renders correctly when called with no context (ctx_json == NULL).
    json cfg = {
        {"top_nav", {{"title", "Home"}, {"show_back_button", false}, {"show_power_button", true}}},
    };

    // Merge any provided context over the defaults (RFC 7396 merge-patch): a
    // caller can override just the keys it cares about (e.g. only button_list).
    // The shared optional parse turns a missing context into an empty
    // (normalized) object, so the defaults above survive untouched.
    const char *json_str = (const char *)ctx_json;
    json incoming;
    parse_optional_screen_json_ctx(json_str, incoming);
    cfg.merge_patch(incoming);

    // Button labels come from cfg["button_list"] when it supplies exactly the
    // four the grid needs; otherwise fall back to the English defaults. (The
    // grid is a fixed 2x2, so a mismatched count means an ill-formed context —
    // defaulting keeps the screen legible rather than rendering blanks.)
    static const char *default_labels[] = {"Scan", "Seeds", "Tools", "Settings"};
    std::vector<std::string> labels(default_labels, default_labels + 4);
    {
        std::vector<std::string> from_cfg;
        if (read_button_list_labels(cfg, from_cfg) && from_cfg.size() == 4) {
            labels = std::move(from_cfg);
        }
    }

    // Drop button_list before building the scaffold: this screen lays out its own
    // 2x2 large-icon grid, so the scaffold must stay in its no-button_list mode.
    // Leaving the key in would make the scaffold ALSO stack a hidden text-button
    // list in the body, which bleeds through the gaps behind the grid.
    cfg.erase("button_list");

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false, &MAIN_MENU_TITLE_FONT);
    lv_obj_t *scr = screen.screen;
    lv_obj_t *body_content = screen.body;

    static const char *icons[] = {
        SeedSignerIconConstants::SCAN,
        SeedSignerIconConstants::SEEDS,
        SeedSignerIconConstants::TOOLS,
        SeedSignerIconConstants::SETTINGS,
    };

    const int32_t available_w = lv_obj_get_content_width(body_content);
    const int32_t screen_h = lv_obj_get_height(scr);

    // Match the Python LargeButtonScreen button sizing:
    //   button_height = int((canvas_height - top_nav.height - 2*COMPONENT_PADDING - EDGE_PADDING) / 2)
    int32_t button_h = (screen_h - TOP_NAV_HEIGHT - 2 * COMPONENT_PADDING - EDGE_PADDING) / 2;
    int32_t button_w = (available_w - COMPONENT_PADDING) / 2;

    // Cap the button width to the 320x240 profile's button proportions so wider-
    // than-4:3 displays don't stretch the 2x2 grid buttons too far. Keep the full
    // button HEIGHT (the grid still fills the screen vertically) but limit the
    // WIDTH to the reference aspect, then center the grid (below) so the body
    // pillar-boxes symmetrically. The top_nav spans the full width regardless, so
    // its power button stays pinned to the far right.
    //
    // Reference = the 320x240 buttons (the widest small/4:3 profile), computed from
    // that profile's geometry. 320x240 renders at PX_MULTIPLIER_100, so these are
    // the unscaled base constants (EDGE_PADDING=8, COMPONENT_PADDING=8,
    // top_nav_height=48):
    //   ref_h = (240 - 48 - 2*8 - 8) / 2          = 84
    //   ref_w = (320 - 2*8 [body pad] - 8) / 2    = 148
    // 240x240 (ref_w would be 108) and 320x240 (exactly 148) stay below the cap, so
    // both are left byte-identical; only 480/800 narrow + pillar-box.
    constexpr int32_t REF_BTN_W = 148;   // 320x240 button width
    constexpr int32_t REF_BTN_H = 84;    // 320x240 button height
    int32_t max_button_w = button_h * REF_BTN_W / REF_BTN_H;
    if (button_w > max_button_w) {
        button_w = max_button_w;
    }

    // Vertically center the 2x2 grid, matching the Python LargeButtonScreen:
    //   button_start_y = top_nav_h + (canvas_h - (top_nav_h + CP) - 2*button_h - CP) / 2
    // Computed relative to the body origin (which sits at top_nav bottom).
    int32_t below_nav = screen_h - TOP_NAV_HEIGHT;
    int32_t y_offset = (below_nav - COMPONENT_PADDING - 2 * button_h - COMPONENT_PADDING) / 2;

    // Horizontally center the (possibly width-capped) grid within the body so wide
    // displays pillar-box symmetrically; x_offset is 0 when the grid fills the width.
    int32_t grid_w = 2 * button_w + COMPONENT_PADDING;
    int32_t x_offset = (available_w - grid_w) / 2;
    if (x_offset < 0) x_offset = 0;

    lv_obj_t *buttons[4] = {NULL, NULL, NULL, NULL};
    for (uint32_t i = 0; i < 4; ++i) {
        lv_obj_t *btn = large_icon_button(body_content, icons[i], labels[i].c_str(), NULL);
        lv_obj_set_size(btn, button_w, button_h);
        buttons[i] = btn;
    }

    // first row
    lv_obj_set_pos(buttons[0], x_offset, y_offset);
    lv_obj_set_pos(buttons[1], x_offset + button_w + COMPONENT_PADDING, y_offset);

    // second row
    lv_obj_set_pos(buttons[2], x_offset, y_offset + button_h + COMPONENT_PADDING);
    lv_obj_set_pos(buttons[3], x_offset + button_w + COMPONENT_PADDING, y_offset + button_h + COMPONENT_PADDING);

    // Bind shared nav behavior using this screen's body focusables/layout.
    bind_screen_navigation(
        cfg,
        screen,
        buttons,
        4,
        NAV_BODY_GRID,
        0
    );

    load_screen_and_cleanup_previous(scr);
}
