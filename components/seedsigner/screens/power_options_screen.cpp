// power_options_screen
//
// Python provenance: PowerOptionsView (view.py) rendered by LargeButtonScreen
// (screen.py) — the LVGL port of the generic 2-or-4 large-button screen, reached in
// the app via PowerOptionsView (the "Reset / Power" menu: a large "Restart" button
// beside a large "Power off" button).
//
// It is the only remaining app screen that uses Python's LargeButtonScreen with a
// button count other than the home menu's four, so it needs its own entry point: the
// generic button_list_screen renders a vertical text list, not the big icon tiles
// LargeButtonScreen draws. The Python-parity grid math (button box, 4:3 width-cap,
// centering) is the SHARED large_button_grid() helper (components.cpp) that
// main_menu_screen also uses — this screen just supplies the host's labels + icons.
//
// Layout (Python LargeButtonScreen): a fixed 2-column grid of large icon tiles,
// horizontally + vertically centered under the top nav. TWO buttons render as one
// centered row (side by side); FOUR render as a 2x2 grid. The tiles are always the
// half-body-height box (a 2-button screen does NOT get full-height tiles). Pressing a
// tile returns its 0-based index; the top-nav back button returns
// SEEDSIGNER_RET_BACK_BUTTON.
//
// Documented deviation from Python: the button WIDTH is capped to the 320x240 (4:3)
// proportions so wider displays pillar-box the grid instead of stretching the tiles —
// the same cap main_menu uses (it lives in large_button_grid()); 240/320 are
// byte-identical.
//
// Lifecycle: Tier 1 (stateless) — no timers / heap ctx / cleanup callback; the icon +
// label strings are copied into the widget tree. Ends with load_screen_and_cleanup_previous.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title
//            (Python PowerOptionsView: _("Reset / Power")).
//   top_nav.show_back_button  (bool, default true)   Python passes show_back_button=True.
//   top_nav.show_power_button (bool, default false)  Python BaseTopNavScreen default.
//   button_list               (array of EXACTLY 2 or 4 items, required)  each item is a
//            localized label plus an icon glyph (Python ButtonOption(label, icon) — the
//            same flat shape the app already sends to button_list_screen). For Power
//            Options: "Restart" (RESTART glyph) + "Power off" (POWER glyph). An item with
//            no icon renders text-only (Python falls back to a plain Button). Erased before
//            the scaffold so it stays in no-button_list mode and this screen lays out the
//            bespoke large-button grid.
//   initial_selected_index    (int, default 0)       [read by nav] initially focused tile.
//   input.mode                (string, optional)     [read by nav] "touch" | "hardware".
//   input.keys.key1/key2/key3 (string, optional)     [read by nav] per-aux-key policy.
//   allow_screensaver         (bool, default true)   [read by scaffold] per-screen saver policy.

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // power_options_screen decl, screen_scaffold_t
#include "components.h"       // large_button_grid
#include "navigation.h"       // NAV_BODY_GRID
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, read_button_list_items, button_item_cfg_t

#include "lvgl.h"             // lv_obj_t

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string
#include <vector>             // std::vector (parsed button items)

using json = nlohmann::json;


void power_options_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // The button labels + icons are the user-visible CONTENT this screen exists to offer,
    // always arriving localized from the host view layer. Parse them before the scaffold
    // exists so no throw path can leak LVGL objects. Python LargeButtonScreen supports
    // ONLY 2 or 4 buttons (a fixed 2-column grid), so any other count is a host wiring
    // error and throws.
    std::vector<button_item_cfg_t> items;
    if (!read_button_list_items(cfg, items) || (items.size() != 2 && items.size() != 4)) {
        throw std::runtime_error("power_options_screen: button_list is required and must supply exactly 2 or 4 items (label + icon)");
    }

    // Structural top_nav flag defaults (never user-visible text): Python PowerOptionsView
    // passes show_back_button=True; show_power_button keeps the BaseTopNavScreen default
    // (false). The localized title is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "power_options_screen");

    // Drop button_list before building the scaffold: this screen lays out its own large-
    // button grid, so the scaffold must stay in its no-button_list mode (otherwise it also
    // stacks a hidden text-button list in the body that bleeds through behind the grid) —
    // the same erase main_menu_screen does.
    cfg.erase("button_list");

    // Parallel icon + label arrays for the grid helper. The `items` vector owns the
    // strings, so these c_str() pointers stay valid through large_button_grid() below.
    const size_t count = items.size();
    std::vector<const char *> icons(count), labels(count);
    for (size_t i = 0; i < count; ++i) {
        icons[i]  = items[i].icon.c_str();     // "" -> text-only tile (Python plain Button)
        labels[i] = items[i].label.c_str();
    }

    // --- Scaffold ---

    // Chrome only: the scaffold builds the top-nav (default title font — unlike
    // main_menu's 26px MainMenuScreen override, LargeButtonScreen keeps the
    // BaseTopNavScreen default) and an empty body; large_button_grid fills that body.
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Shared Python LargeButtonScreen grid: 2 tiles -> one centered row; 4 -> a 2x2 grid.
    lv_obj_t *buttons[4] = {NULL, NULL, NULL, NULL};
    large_button_grid(screen.body, screen.screen, icons.data(), labels.data(), count, buttons);

    // --- Navigation + load ---

    // Grid navigation over the tiles (NAV_BODY_GRID: grid_columns_for_count() gives 2 cols
    // for both 2 and 4, so two tiles step left/right in one row and four move as a 2x2).
    // Menu-style default index 0 — a power menu always has a tile focused (Restart); the
    // host may override via cfg initial_selected_index. On the FULL bind_screen_navigation
    // overload (custom item array + grid layout), like main_menu.
    bind_screen_navigation(cfg, screen, buttons, count, NAV_BODY_GRID, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
