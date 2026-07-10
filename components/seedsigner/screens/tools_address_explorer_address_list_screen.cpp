// tools_address_explorer_address_list_screen
//
// Python provenance: ToolsAddressExplorerAddressListScreen (tools_screens.py)
//
// One page of the Address Explorer tool: a left-aligned button list of derived
// addresses — one "{index}:{truncated address}" row per address in the FIXED-WIDTH
// emphasis font — plus a trailing localized "Next N" paging action with a right
// chevron (Python CHEVRON_RIGHT). The focused row swaps to its full space-formatted
// address and marquee-scrolls it (focus-reveal), reverting when focus moves on; the
// pressed button index returns through the standard navigation callback.
//
// Two things make this screen need its own entry point rather than the generic
// button_list_screen:
//
//   1. FIXED-WIDTH font. Every button renders in the fixed-width emphasis font at
//      button_font_size + 4 (Python FIXED_WIDTH_EMPHASIS_FONT_NAME =
//      "Inconsolata-SemiBold"), == the profile's 22px-base CANDIDATE_FONT. The
//      generic button_list_screen hard-codes the proportional BUTTON_FONT, so the
//      monospace columns (and the char-width-derived truncation below) can't be
//      reproduced by host-formatted labels alone.
//
//   2. Per-button truncation sized from the MONOSPACE cell width and the canvas
//      width. Because the truncation depends on the C font's metrics at the ACTIVE
//      resolution, it is computed here (not host-side) so every display profile
//      truncates to its own width.
//
// Documented deviations from Python (both flagged in the conformance bug ledger;
// conformance leaves them unchanged):
//   - Truncation SHAPE: the WIDTH math matches Python's __post_init__ cell
//     budgeting, but the emitted characters differ — Python's at-rest label is a
//     symmetric "{index}:{first-half}...{last-half}" split and its reveal label is
//     the plain "{index}:{address}"; this port renders a prefix-aware priority
//     truncation and a space-formatted reveal (the formatted_address run rhythm).
//   - is_bottom_list = false vs Python's True — equivalent only while the page
//     overflows the viewport; see the rationale at the assignment below.
//
// Lifecycle: Tier 1 (stateless) — no statics or timers; the focus-reveal
// full-address strings are deep-copied into the shared reveal registry
// (components.cpp) and erased on the label's LV_EVENT_DELETE.
//
// cfg:
//   top_nav.title             (string, required)    localized page title (the host
//            passes "Receive Addrs" / "Change Addrs").
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   addresses                 (string[], required)  full derived addresses for this
//            page (host-derived from the xpub / wallet descriptor).
//   start_index               (int, default 0)      index of addresses[0] (Python
//            start_index = 0); prefixes each row label as "{index}:".
//   next_label                (string, required)    localized paging-button label
//            (Python: _("Next {}").format(len(addresses))).
//   initial_selected_index    (int, default 0)      [read by the navigation layer]
//            which row is focused (and full-address-revealed) on load (Python
//            selected_button).
//   input.mode                (string, optional)    [read by the navigation layer]
//            "touch" | "hardware" input-mode override.
//   input.keys.key1/key2/key3 (string, optional)    [read by the navigation layer]
//            per-aux-key policy "enter" | "noop" | "emit".
//   allow_screensaver         (bool, default true)  [read by the parse/scaffold
//            layer] normalized by parse_screen_json_ctx; false stamps the
//            screensaver opt-out flag on the screen root.
//   button_list / is_bottom_list / is_button_text_centered are COMPOSED/FORCED by
//            this screen (host-supplied values are overwritten); the scaffold's
//            other generic button-list options (button_style, checked_buttons,
//            intro text) pass through to it untouched and are not part of this
//            screen's contract.

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // tools_address_explorer_address_list_screen decl, screen_scaffold_t fields
#include "components.h"       // fa_prefix_len, button_text_label, label_set_focus_reveal
#include "gui_constants.h"    // EDGE_PADDING, COMPONENT_PADDING, CANDIDATE_FONT, SeedSignerIconConstants
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_text_get_size, label long-mode, per-object style setters

#include <nlohmann/json.hpp>  // json (cfg reads + button-list composition)

#include <algorithm>          // std::max, std::min (truncation budgeting)
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string, std::to_string
#include <vector>             // std::vector

using json = nlohmann::json;


namespace {

// Both helpers below encode formatted_address's head/middle/tail "run rhythm";
// they are slated for extraction next to formatted_address (components.cpp) at the
// rollout consolidation decision and stay file-local until then.

// Full space-formatted address for the focus-reveal (marquee) label: the whole
// address with the formatted_address run rhythm — "<prefix> <first-7> <middle>
// <last-7>". Short addresses that can't be split are returned as-is.
// NOTE: deliberately diverges from Python, whose reveal label is the plain
// "{index}:{address}" — flagged in the conformance bug ledger for adjudication.
std::string tools_address_explorer_address_list_format_full(const std::string &address,
                                                            int prefix_length) {
    const int run_length = 7;
    int address_length = (int)address.size();
    if (address_length <= prefix_length + 2 * run_length) return address;

    std::string head = (prefix_length > 0)
        ? (address.substr(0, prefix_length) + " " + address.substr(prefix_length, run_length))
        : address.substr(0, run_length);
    std::string middle = address.substr(prefix_length + run_length,
                                        address_length - run_length - (prefix_length + run_length));
    std::string tail = address.substr(address_length - run_length, run_length);
    return head + " " + middle + " " + tail;
}

// At-rest truncation of one address to `available_cells` monospace cells,
// formatted_address-style, with this priority as width shrinks: (1) prefix + " " +
// first-7 (the verifiable head), (2) last-7, (3) middle chars. So a 240 row shows
// "<prefix> <first-7>..." (ellipsis at the end); wider rows add "...<last-7>"; the
// widest fill the middle around the ellipsis
// ("<prefix> <first-7> <midHead>...<midTail> <last-7>").
// NOTE: the WIDTH math matches Python's __post_init__ cell budgeting, but the
// emitted characters deliberately differ from Python's symmetric
// "{first-half}...{last-half}" split — flagged in the conformance bug ledger for
// adjudication.
std::string tools_address_explorer_address_list_abbreviate(const std::string &address,
                                                           int prefix_length,
                                                           int available_cells) {
    const int run_length = 7;
    const std::string ellipsis = "...";
    int address_length = (int)address.size();
    if (address_length <= prefix_length + 2 * run_length) {
        return address.substr(0, std::max(0, available_cells));   // too short to split
    }

    std::string first_run  = address.substr(prefix_length, run_length);
    std::string last_run   = address.substr(address_length - run_length, run_length);
    std::string middle_run = address.substr(prefix_length + run_length,
                                            address_length - run_length - (prefix_length + run_length));
    std::string base = (prefix_length > 0)
        ? (address.substr(0, prefix_length) + " " + first_run)
        : first_run;                                               // prefix + first-7

    if (available_cells <= (int)base.size()) {
        return base.substr(0, std::max(0, available_cells));       // degenerate: keep the head
    }
    int remaining = available_cells - (int)base.size();

    // Secondary: append the tail "<ellipsis><sp><last-7>". If there isn't room, show
    // only the ellipsis at the end (first-7 priority) — or nothing if not even the
    // ellipsis fits.
    if (remaining < (int)ellipsis.size() + 1 + run_length) {
        return (remaining >= (int)ellipsis.size()) ? base + ellipsis : base;
    }

    // Tertiary: spend any leftover on middle chars, split around the ellipsis (costs
    // one extra leading separator before the middle run).
    int extra         = remaining - ((int)ellipsis.size() + 1 + run_length);
    int middle_budget = (extra > 1) ? std::min(extra - 1, (int)middle_run.size()) : 0;
    if (middle_budget <= 0) {
        return base + ellipsis + " " + last_run;                   // "<prefix> <first-7>... <last-7>"
    }
    int middle_head_length = (middle_budget + 1) / 2;
    int middle_tail_length = middle_budget - middle_head_length;
    std::string middle_head = middle_run.substr(0, middle_head_length);
    std::string middle_tail = (middle_tail_length > 0)
        ? middle_run.substr(middle_run.size() - middle_tail_length, middle_tail_length)
        : std::string();
    return base + " " + middle_head + ellipsis + middle_tail + " " + last_run;
}

}  // namespace


void tools_address_explorer_address_list_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: addresses is the page content this screen exists to show;
    // next_label is user-visible CONTENT, which always arrives localized from the
    // host view layer (a string literal baked here would be English-only by
    // construction — Python: _("Next {}").format(len(addresses))). One throw per
    // field, before the scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("addresses") || !cfg["addresses"].is_array()) {
        throw std::runtime_error("tools_address_explorer_address_list_screen: addresses is required and must be an array of strings");
    }
    if (!cfg.contains("next_label") || !cfg["next_label"].is_string()) {
        throw std::runtime_error("tools_address_explorer_address_list_screen: next_label is required and must be a string");
    }

    // Full addresses for this page (host derives them from the xpub / wallet
    // descriptor); every element must be a string.
    std::vector<std::string> addresses;
    for (const auto &address_entry : cfg["addresses"]) {
        if (!address_entry.is_string()) {
            throw std::runtime_error("tools_address_explorer_address_list_screen: addresses is required and must be an array of strings");
        }
        addresses.push_back(address_entry.get<std::string>());
    }

    // Structural default (a number, never rendered as text) — Python: start_index = 0.
    int start_index = cfg.value("start_index", 0);

    // Which row is focused on load is decided by bind_screen_navigation from
    // cfg["initial_selected_index"] (default 0, Python selected_button); the reveal
    // below follows whatever row it highlights.

    // "Next N" paging label (host-localized; validated above).
    std::string next_label = cfg["next_label"].get<std::string>();

    // --- Row width math ---

    // The fixed-width emphasis font at button_font_size + 4 == the profile CANDIDATE_FONT
    // (Inconsolata SemiBold, 22px base). Measure one monospace cell (advance of "X"),
    // matching Python's Fonts.get_font(...).getbbox("X") width.
    // Deliberately NOT migrated to the shared monospace_char_width() ('0'-run average):
    // the single-'X' advance equals the ten-'0' average only as a property of this
    // font, not provably in code (extraction ledger #3 keeps this variant as-is).
    const lv_font_t *fixed_font = &CANDIDATE_FONT;
    lv_point_t x_size = {0, 0};
    lv_text_get_size(&x_size, "X", fixed_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t char_width = x_size.x > 0 ? x_size.x : 1;

    // Reserve room for the widest "{index}:" prefix (index_digits digits + the colon), so
    // every row truncates its ADDRESS portion to the same width.
    int last_index = start_index + (int)addresses.size() - 1;
    if (last_index < 0) last_index = 0;
    int index_digits = (int)std::to_string(last_index).size();

    int32_t canvas_width = lv_display_get_horizontal_resolution(NULL);
    int32_t available_width = canvas_width
                            - 2 * EDGE_PADDING
                            - 2 * COMPONENT_PADDING
                            - (index_digits + 1) * char_width;
    int available_cells = (int)(available_width / char_width);   // address cells after "{index}:"
    if (available_cells < 1) available_cells = 1;

    // --- Build the button list ---

    // Each row's at-rest label is width-adaptive (prefix + first-7 priority, then
    // last-7, then middle — see tools_address_explorer_address_list_abbreviate);
    // full_labels[i] holds the full space-formatted address revealed +
    // marquee-scrolled when the row is focused (see the Body pass below).
    json button_list = json::array();
    std::vector<std::string> full_labels(addresses.size());
    for (size_t i = 0; i < addresses.size(); ++i) {
        int current_index = start_index + (int)i;
        const std::string &address = addresses[i];
        int prefix_length = fa_prefix_len(address.c_str());

        std::string index_prefix = std::to_string(current_index) + ":";
        button_list.push_back(index_prefix
            + tools_address_explorer_address_list_abbreviate(address, prefix_length, available_cells));
        full_labels[i] = index_prefix
            + tools_address_explorer_address_list_format_full(address, prefix_length);
    }

    // Trailing paging button: label + right chevron (Python CHEVRON_RIGHT).
    {
        json next_button = json::object();
        next_button["label"] = next_label;
        next_button["right_icon"] = SeedSignerIconConstants::CHEVRON_RIGHT;
        button_list.push_back(next_button);
    }

    // top_nav chrome: structural flags write-if-absent (Python ButtonListScreen
    // defaults show_back_button=True, show_power_button=False); the localized title
    // is content and must come from the host ("Receive Addrs" / "Change Addrs").
    // Kept here at the pre-scaffold composition point rather than immediately after
    // parse so the row-width measurement above keeps its position (conformance rule:
    // validations do not move across LVGL calls).
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "tools_address_explorer_address_list_screen");

    cfg["button_list"] = button_list;

    // Forced, not defaulted — a host-supplied value is overwritten:
    //   is_button_text_centered = false  (Python: is_button_text_centered = False —
    //       a left-aligned address column).
    //   is_bottom_list = false DELIBERATELY DIVERGES from Python's True: a full
    //       address page (10 rows + the "Next N" action) overflows the viewport, and
    //       Python forces an overflowing list to TOP-align at top_nav.height
    //       (screen.py: button_list_y clamped to top_nav.height, has_scroll_arrows =
    //       True). Mode 2 (is_bottom_list = false) is the scaffold path that
    //       top-aligns the buttons flush under the nav with item-focus scrolling —
    //       exactly Python's forced-overflow layout. (The bottom-pinned scaffold path
    //       would instead leave an empty upper_body + flex spacer that nudge the
    //       first row down.) Nothing enforces the always-overflows assumption: a
    //       short final page would top-align here where Python bottom-pins it —
    //       flagged in the conformance bug ledger, left unchanged.
    cfg["is_bottom_list"] = false;
    cfg["is_button_text_centered"] = false;

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // Re-font every button + arm each address row's focus-reveal.
    // The scaffold builds labels in the proportional BUTTON_FONT; the Python screen
    // renders the whole list (addresses AND the "Next N" action) in monospace. While
    // re-fonting, register each ADDRESS row's full space-formatted address as its
    // focus-reveal text (label_set_focus_reveal): at rest the row shows its truncated
    // head...tail (the label the scaffold built); when the row gains focus the nav
    // layer swaps in the full address and marquee-scrolls it, then reverts on defocus
    // — so the reveal follows focus down EVERY row, not just the first. Start each
    // row CLIPPED so the first focus transition (CLIP -> SCROLL_CIRCULAR) actually
    // fires the *paced* 40 px/sec marquee promotion (label_set_line_autoscroll)
    // instead of being short-circuited into LVGL's unpaced default scroll (the
    // "scrolls way too fast" case). The trailing "Next N" action has no full form and
    // is left untouched.
    for (size_t i = 0; i < screen.button_list_count; ++i) {
        lv_obj_t *label = button_text_label(screen.button_list[i]);
        if (!label) continue;
        lv_obj_set_style_text_font(label, fixed_font, LV_PART_MAIN);
        if (i < addresses.size()) {
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
            label_set_focus_reveal(label, full_labels[i].c_str());
        }
    }

    // --- Navigation + load ---

    // Menu-style default index: an address list always has a selection, so row 0
    // starts focused (the host may override via cfg initial_selected_index).
    // bind_screen_navigation applies the initial focus through update_visual_focus,
    // which fires button_set_label_marquee on each row — so the focused row's full
    // address is revealed + paced-marquee'd right here on load, and then follows the
    // joystick as the user moves between rows. In touch mode (no persistent
    // selection) nothing is focused, so every row stays truncated — matching
    // Python's "reveal only while selected".
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
