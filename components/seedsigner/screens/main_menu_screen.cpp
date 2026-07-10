// main_menu_screen
//
// Python provenance: MainMenuScreen (screen.py), a LargeButtonScreen subclass —
// the grid geometry below ports LargeButtonScreen's 4-button layout math.
//
// The Home menu: a fixed 2x2 grid of four large icon buttons (Scan / Seeds /
// Tools / Settings) under a title bar. Pressing a tile returns its index through
// the standard navigation callback. The four icons never localize; the title and
// the four labels do (host-supplied via cfg).
//
// Bespoke-grid tier (spec §8): listed under the chrome-free / scaffold-bypass
// tier as the bespoke Python-parity grid. It KEEPS the scaffold's top-nav chrome
// (create_top_nav_screen_scaffold) but bypasses the scaffold's button_list
// layout, laying out its own absolute-positioned 2x2 grid (no flex, no lv_grid)
// to match Python's LargeButtonScreen pixel math.
//
// Documented deviations from Python:
//   - Button WIDTH is capped to the 320x240 (4:3) profile's proportions so wider-
//     than-4:3 displays pillar-box the grid instead of stretching the tiles;
//     Python renders full-width 2-across. 240x240 and 320x240 stay under the cap
//     and are byte-identical.
//   - button_list must carry EXACTLY four labels; any other count throws (the grid
//     is a fixed 2x2, so a mismatched count is a host wiring error).
//
// CONTENT POLICY (spec §5): main_menu's user-visible strings — the title and the
// four tile labels — are localized CONTENT and are REQUIRED from the host (an
// English literal baked here would ship untranslated). Only the structural top_nav
// flags (show_back_button / show_power_button) keep defaults. A NULL/empty ctx
// therefore no longer renders a built-in English menu; it throws on the missing
// title. (This narrows the earlier boot-tier exception, which kept English
// title/label defaults; on-device main_menu is always reached through
// MainMenuView, which supplies the localized strings.)
//
// Lifecycle: Tier 1 (stateless) — no timers / heap ctx / cleanup callback; the
// two function-local static arrays are const data with no teardown concern. Ends
// with load_screen_and_cleanup_previous.
//
// cfg — a present ctx is merged (RFC 7396 merge-patch) over the structural top_nav
// flag defaults, so a host overrides only the keys it supplies.
// parse_optional_screen_json_ctx still tolerates a NULL/empty ctx at the parse
// layer, but the required title/labels below then throw.
//   top_nav.title              (string, required)   localized screen title.
//   top_nav.show_back_button   (bool,   default false)    Python MainMenuScreen override.
//   top_nav.show_power_button  (bool,   default true)     Python MainMenuScreen override.
//   button_list                (array of EXACTLY 4 label strings, required)  the four
//            localized tile labels. Erased before the scaffold so it stays in
//            no-button_list mode and this screen lays out the bespoke 2x2 grid.
//   allow_screensaver          (bool, default true)  [read by scaffold]  per-screen saver
//            opt-out; false stamps SS_OBJ_FLAG_NO_SCREENSAVER on the root.
//   top_nav.icon / top_nav.icon_color  (string, optional)  [read by scaffold]  optional
//            contextual glyph beside the title; unused by the reference home menu.
//   input.mode                 (string "touch"|"hardware", optional)  [read by nav]
//            input-mode override.
//   input.keys.key1/key2/key3  (string "enter"|"noop"|"emit", default "enter")  [read by nav]
//            hardware aux-key policy.
//   initial_selected_index     (int, default 0)  [read by nav]  initially focused tile.

#include "screen_scaffold.h"  // parse_optional_screen_json_ctx, create_top_nav_screen_scaffold, bind_screen_navigation, load_screen_and_cleanup_previous, screen_scaffold_t
#include "seedsigner.h"       // main_menu_screen decl
#include "components.h"       // large_icon_button
#include "gui_constants.h"    // MAIN_MENU_TITLE_FONT, SeedSignerIconConstants, TOP_NAV_HEIGHT, COMPONENT_PADDING, EDGE_PADDING
#include "navigation.h"       // NAV_BODY_GRID (nav_body_layout_t)
#include "screen_helpers.h"   // read_button_list_labels, require_top_nav_title

#include "lvgl.h"             // lv_obj_get_content_width / lv_obj_get_height / lv_obj_set_size / lv_obj_set_pos

#include <nlohmann/json.hpp>  // json (defaults literal + merge_patch + cfg reads)

#include <stdexcept>          // std::runtime_error (required title/labels)
#include <string>             // std::string
#include <vector>             // std::vector (the label list)

using json = nlohmann::json;


void main_menu_screen(void *ctx_json) {
    // --- Config ---

    // Structural top_nav flag defaults (never user-visible text): the Python
    // MainMenuScreen overrides (title font 26 via MAIN_MENU_TITLE_FONT below,
    // show_back_button=False, show_power_button=True). The title itself is localized
    // CONTENT, required from the host (see the content-policy note in the banner).
    json cfg = {
        {"top_nav", {{"show_back_button", false}, {"show_power_button", true}}},
    };

    // Merge any provided context over the structural defaults (RFC 7396 merge-patch):
    // a caller overrides just the keys it supplies. The shared optional parse turns a
    // missing context into an empty (normalized) object; the required title/labels
    // below then throw for a NULL/empty ctx.
    const char *json_str = (const char *)ctx_json;
    json incoming;
    parse_optional_screen_json_ctx(json_str, incoming);
    cfg.merge_patch(incoming);

    // Require the localized title (host-supplied content) — a screen-named throw on a
    // missing/malformed title instead of an English fallback.
    require_top_nav_title(cfg, "main_menu_screen");

    // The four tile labels are localized content, required from the host: exactly four
    // strings (the grid is a fixed 2x2, so any other count is a host wiring error and
    // throws).
    //
    // read_button_list_labels lives in screen_helpers though this file is currently
    // its sole caller; that single-caller consolidation is slated for a rollout
    // decision (spec §12), not resolved here.
    std::vector<std::string> labels;
    if (!read_button_list_labels(cfg, labels) || labels.size() != 4) {
        throw std::runtime_error("main_menu_screen: button_list is required and must supply exactly four labels");
    }

    // Drop button_list before building the scaffold: this screen lays out its own
    // 2x2 large-icon grid, so the scaffold must stay in its no-button_list mode.
    // Leaving the key in would make the scaffold ALSO stack a hidden text-button
    // list in the body, which bleeds through the gaps behind the grid.
    cfg.erase("button_list");

    // --- Scaffold ---

    // Chrome only: the scaffold builds the top-nav (with the 26px MainMenuScreen
    // title font) and an empty body; this screen fills that body with its own grid.
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false, &MAIN_MENU_TITLE_FONT);

    // The four fixed home-menu glyphs, parallel to `labels` (Scan/Seeds/Tools/
    // Settings). Icons never localize, so they are a static table, not cfg-driven.
    static const char *main_menu_icons[] = {
        SeedSignerIconConstants::SCAN,
        SeedSignerIconConstants::SEEDS,
        SeedSignerIconConstants::TOOLS,
        SeedSignerIconConstants::SETTINGS,
    };

    // --- Geometry ---
    //
    // Fully manual, absolute-positioned 2x2 grid (Python LargeButtonScreen parity):
    // derive the button box size and the pillar-box centering offsets from the parent
    // dimensions here, THEN create and place the four buttons in the Body section
    // below. No flex, no lv_grid — the scaffold supplied only chrome + an empty body.
    const int32_t available_width = lv_obj_get_content_width(screen.body);
    const int32_t screen_height   = lv_obj_get_height(screen.screen);

    // Button-sizing pass — match Python LargeButtonScreen:
    //   button_height = int((canvas_height - top_nav.height - 2*COMPONENT_PADDING - EDGE_PADDING) / 2)
    //   button_width  = int((canvas_width  - 2*EDGE_PADDING [body pad] - COMPONENT_PADDING) / 2)
    // available_width is already the body's CONTENT width (canvas minus the body's
    // 2*EDGE_PADDING), so only the single inter-column COMPONENT_PADDING is subtracted.
    int32_t button_height = (screen_height - TOP_NAV_HEIGHT - 2 * COMPONENT_PADDING - EDGE_PADDING) / 2;
    int32_t button_width  = (available_width - COMPONENT_PADDING) / 2;

    // Width-cap pass (documented deviation from Python) — cap the button WIDTH to the
    // 320x240 profile's button proportions so wider-than-4:3 displays don't stretch the
    // grid. Keep the full button HEIGHT (the grid still fills the screen vertically) but
    // limit the WIDTH to the reference aspect, then center the grid (below) so the body
    // pillar-boxes symmetrically. The top_nav spans the full width regardless, so its
    // power button stays pinned to the far right.
    //
    // Reference = the 320x240 buttons (the widest small/4:3 profile), computed from that
    // profile's geometry. 320x240 renders at PX_MULTIPLIER_100, so these are the
    // unscaled base constants (EDGE_PADDING=8, COMPONENT_PADDING=8, top_nav_height=48):
    //   REFERENCE_BUTTON_HEIGHT = (240 - 48 - 2*8 - 8) / 2       = 84
    //   REFERENCE_BUTTON_WIDTH  = (320 - 2*8 [body pad] - 8) / 2 = 148
    // 240x240 (uncapped width would be 108) and 320x240 (exactly 148) stay below the
    // cap, so both are left byte-identical; only 480/800 narrow + pillar-box.
    constexpr int32_t REFERENCE_BUTTON_WIDTH  = 148;   // 320x240 button width
    constexpr int32_t REFERENCE_BUTTON_HEIGHT = 84;    // 320x240 button height
    int32_t max_button_width = button_height * REFERENCE_BUTTON_WIDTH / REFERENCE_BUTTON_HEIGHT;
    if (button_width > max_button_width) {
        button_width = max_button_width;
    }

    // Vertical-centering pass — center the 2x2 grid, matching Python LargeButtonScreen:
    //   button_start_y = top_nav_h + (canvas_h - (top_nav_h + CP) - 2*button_h - CP) / 2
    // Computed relative to the body origin (which sits at the top_nav bottom).
    int32_t below_top_nav = screen_height - TOP_NAV_HEIGHT;
    int32_t y_offset = (below_top_nav - COMPONENT_PADDING - 2 * button_height - COMPONENT_PADDING) / 2;

    // Horizontal-centering pass — center the (possibly width-capped) grid within the
    // body so wide displays pillar-box symmetrically; x_offset is 0 when the grid
    // already fills the width.
    int32_t grid_width = 2 * button_width + COMPONENT_PADDING;
    int32_t x_offset = (available_width - grid_width) / 2;
    if (x_offset < 0) x_offset = 0;

    // --- Body ---

    // Build the four large icon buttons into the body and size each to the derived
    // button box; the explicit positions are set immediately after.
    lv_obj_t *buttons[4] = {NULL, NULL, NULL, NULL};
    for (uint32_t i = 0; i < 4; ++i) {
        // align_to is NULL: these tiles are absolutely positioned (lv_obj_set_pos
        // below), not chain-aligned to a sibling.
        lv_obj_t *button = large_icon_button(screen.body, main_menu_icons[i], labels[i].c_str(), /*align_to=*/NULL);
        lv_obj_set_size(button, button_width, button_height);
        buttons[i] = button;
    }

    // First row: the two top tiles at the grid's top-left / top-right.
    lv_obj_set_pos(buttons[0], x_offset, y_offset);
    lv_obj_set_pos(buttons[1], x_offset + button_width + COMPONENT_PADDING, y_offset);

    // Second row: the two bottom tiles, one button-height + gutter below the first.
    lv_obj_set_pos(buttons[2], x_offset, y_offset + button_height + COMPONENT_PADDING);
    lv_obj_set_pos(buttons[3], x_offset + button_width + COMPONENT_PADDING, y_offset + button_height + COMPONENT_PADDING);

    // --- Navigation + load ---

    // Grid navigation over the four tiles (NAV_BODY_GRID: the joystick moves across
    // the 2x2). Menu-style default index 0 — the home menu always has a tile focused;
    // a host may override via cfg initial_selected_index. main_menu stays on the FULL
    // bind_screen_navigation overload (custom item array + grid layout), not the
    // scaffold-buttons convenience form.
    bind_screen_navigation(
        cfg,
        screen,
        buttons,
        4,
        NAV_BODY_GRID,
        /*default_initial_index=*/0
    );

    load_screen_and_cleanup_previous(screen.screen);
}
