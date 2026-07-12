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
// layout, delegating the fixed 2x2 grid to the shared large_button_grid() helper
// (components.cpp) — the Python LargeButtonScreen pixel math (button box, 4:3
// width-cap, grid centering), now shared with power_options_screen (the 2-button
// case). This file supplies only the four fixed icons + the localized labels.
//
// Documented deviations from Python:
//   - Button WIDTH is capped to the 320x240 (4:3) profile's proportions so wider-
//     than-4:3 displays pillar-box the grid instead of stretching the tiles;
//     Python renders full-width 2-across. 240x240 and 320x240 stay under the cap
//     and are byte-identical. (The cap lives in large_button_grid().)
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

    // --- Body ---

    // Lay out the fixed 2x2 large-icon grid via the shared large_button_grid helper (the
    // Python LargeButtonScreen geometry — button box, 4:3 width-cap, and grid centering —
    // now lives there, shared with power_options_screen). The scaffold supplied only the
    // top-nav chrome + an empty body; the helper fills it with the four absolutely-positioned
    // tiles. `labels` is a std::vector<std::string>, so pass a parallel array of c_str()s.
    const char *label_ptrs[4] = {
        labels[0].c_str(), labels[1].c_str(), labels[2].c_str(), labels[3].c_str(),
    };
    lv_obj_t *buttons[4] = {NULL, NULL, NULL, NULL};
    large_button_grid(screen.body, screen.screen, main_menu_icons, label_ptrs, 4, buttons);

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
