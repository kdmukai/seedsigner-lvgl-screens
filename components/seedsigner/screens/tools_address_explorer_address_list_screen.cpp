#include "screen_scaffold.h"   // parse/scaffold/nav/load helpers (defined in screen_scaffold.cpp)
#include "components.h"        // button_text_label()
#include "gui_constants.h"     // colors, scaled layout macros, CANDIDATE_FONT, SeedSignerIconConstants
#include "navigation.h"        // NAV_BODY_VERTICAL

#include "lvgl.h"

#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ToolsAddressExplorerAddressListScreen (Python tools_screens.py:501)
//
// A ButtonListScreen (is_bottom_list = True, is_button_text_centered = False) that
// lists a page of derived addresses, one per button, with a trailing "Next N" action
// (right chevron) that pages forward. Two things make this screen special enough to
// need its own entry point rather than the generic button_list_screen:
//
//  1. FIXED-WIDTH font. Every button renders in the fixed-width emphasis font at
//     button_font_size + 4 (Python FIXED_WIDTH_EMPHASIS_FONT_NAME = "Inconsolata-
//     SemiBold"), == the profile's 22px-base CANDIDATE_FONT. The generic
//     button_list_screen hard-codes the proportional BUTTON_FONT, so the monospace
//     columns (and the char-width-derived truncation below) can't be reproduced by
//     host-formatted labels alone.
//
//  2. Per-button head...tail truncation, sized from the MONOSPACE cell width and the
//     canvas width, exactly like Python's __post_init__. Because the truncation
//     depends on the C font's metrics at the ACTIVE resolution, it is computed here
//     (not host-side) so every profile (240/320/480/800) truncates to its own width.
//     Each address row shows "{index}:{head}...{tail}"; the initially-selected row is
//     revealed to its full "{index}:{address}" (Python ButtonOption.active_button_label,
//     a ScrollableTextLine that start-justifies and marquee-scrolls the full address
//     when the row is selected).
//
// cfg:
//   top_nav.title           — default "Receive Addrs" (host passes "Change Addrs" too).
//   addresses (str[])       — full derived addresses for this page (required, host-derived).
//   start_index (int)       — index of addresses[0] (default 0); prefixes each row label.
//   initial_selected_index  — which row is focused/revealed on load (default 0; Python
//                             selected_button). Shared with bind_screen_navigation.
//   next_label (str)        — the paging button label (host-localized; default "Next {N}").
// ---------------------------------------------------------------------------
// Full space-formatted address for the focus-reveal (marquee) label: the whole address
// with the formatted_address run rhythm — "<prefix> <first-7> <middle> <last-7>". Short
// addresses that can't be split are returned as-is.
static std::string ae_format_full(const std::string &addr, int plen) {
    const int n = 7;
    int len = (int)addr.size();
    if (len <= plen + 2 * n) return addr;
    std::string head = (plen > 0) ? (addr.substr(0, plen) + " " + addr.substr(plen, n))
                                  : addr.substr(0, n);
    std::string middle = addr.substr(plen + n, len - n - (plen + n));
    std::string tail   = addr.substr(len - n, n);
    return head + " " + middle + " " + tail;
}

// At-rest truncation of one address to `avail` monospace cells, formatted_address-style,
// with this priority as width shrinks: (1) prefix + " " + first-7 (the verifiable head),
// (2) last-7, (3) middle chars. So a 240 row shows "<prefix> <first-7>..." (ellipsis at the
// end); wider rows add "...<last-7>"; the widest fill the middle around the ellipsis
// ("<prefix> <first-7> <midHead>...<midTail> <last-7>").
static std::string ae_abbreviate(const std::string &addr, int plen, int avail) {
    const int n = 7;
    const std::string E = "...";
    int len = (int)addr.size();
    if (len <= plen + 2 * n) return addr.substr(0, std::max(0, avail));  // too short to split

    std::string F    = addr.substr(plen, n);
    std::string L    = addr.substr(len - n, n);
    std::string Mid  = addr.substr(plen + n, len - n - (plen + n));
    std::string base = (plen > 0) ? (addr.substr(0, plen) + " " + F) : F;   // prefix + first-7

    if (avail <= (int)base.size()) {
        return base.substr(0, std::max(0, avail));         // degenerate: keep the head
    }
    int rem = avail - (int)base.size();

    // Secondary: append the tail "<E><sp><L>". If there isn't room, show only the ellipsis
    // at the end (first-7 priority) — or nothing if not even the ellipsis fits.
    if (rem < (int)E.size() + 1 + n) {
        return (rem >= (int)E.size()) ? base + E : base;
    }
    // Tertiary: spend any leftover on middle chars, split around the ellipsis (costs one
    // extra leading separator before the middle run).
    int extra      = rem - ((int)E.size() + 1 + n);
    int mid_budget = (extra > 1) ? std::min(extra - 1, (int)Mid.size()) : 0;
    if (mid_budget <= 0) {
        return base + E + " " + L;                         // "<prefix> <first-7>... <last-7>"
    }
    int mh = (mid_budget + 1) / 2;
    int mt = mid_budget - mh;
    std::string Mh = Mid.substr(0, mh);
    std::string Mt = (mt > 0) ? Mid.substr(Mid.size() - mt, mt) : std::string();
    return base + " " + Mh + E + Mt + " " + L;
}

void tools_address_explorer_address_list_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // --- Data ------------------------------------------------------------------
    // Full addresses for this page (host derives them from the xpub / wallet descriptor).
    std::vector<std::string> addresses;
    if (cfg.contains("addresses") && cfg["addresses"].is_array()) {
        for (const auto &a : cfg["addresses"]) {
            if (a.is_string()) addresses.push_back(a.get<std::string>());
        }
    }

    int start_index = cfg.value("start_index", 0);

    // Which row is focused on load is decided by bind_screen_navigation from
    // cfg["initial_selected_index"] (default 0, Python selected_button); the reveal
    // below follows whatever row it highlights.

    // "Next N" paging label (host-localized; default mirrors Python _("Next {}").format(len)).
    std::string next_label = cfg.value(
        "next_label", std::string("Next ") + std::to_string(addresses.size()));

    // --- Row width math -------------------------------------------------------------
    // The fixed-width emphasis font at button_font_size + 4 == the profile CANDIDATE_FONT
    // (Inconsolata SemiBold, 22px base). Measure one monospace cell (advance of "X"),
    // matching Python's Fonts.get_font(...).getbbox("X") width.
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
    int avail_cells = (int)(available_width / char_width);   // address cells after "{index}:"
    if (avail_cells < 1) avail_cells = 1;

    // --- Build the button list (adaptive row labels + the "Next N" action) -----------
    // Each row's at-rest label is width-adaptive (prefix + first-7 priority, then last-7,
    // then middle — see ae_abbreviate); full_labels[i] holds the full space-formatted
    // address revealed + marquee-scrolled when the row is focused (see below).
    json button_list = json::array();
    std::vector<std::string> full_labels(addresses.size());
    for (size_t i = 0; i < addresses.size(); ++i) {
        int cur_index = start_index + (int)i;
        const std::string &addr = addresses[i];
        int plen = fa_prefix_len(addr.c_str());

        std::string idx = std::to_string(cur_index) + ":";
        button_list.push_back(idx + ae_abbreviate(addr, plen, avail_cells));
        full_labels[i] = idx + ae_format_full(addr, plen);
    }

    // Trailing paging button: label + right chevron (Python CHEVRON_RIGHT).
    {
        json next_btn = json::object();
        next_btn["label"] = next_label;
        next_btn["right_icon"] = SeedSignerIconConstants::CHEVRON_RIGHT;
        button_list.push_back(next_btn);
    }

    // --- Scaffold: a left-aligned button list (Python is_button_text_centered = False) --
    // Python's ButtonListScreen is is_bottom_list = True, but a full address page (10
    // rows + the "Next N" action) ALWAYS overflows the viewport, and Python forces an
    // overflowing list to TOP-align at top_nav.height (screen.py: button_list_y clamped
    // to top_nav.height, has_scroll_arrows = True). Mode 2 (is_bottom_list = False) is the
    // scaffold path that top-aligns the buttons flush under the nav with item-focus
    // scrolling — exactly Python's forced-overflow layout. (The bottom-pinned scaffold
    // path would instead leave an empty upper_body + flex spacer that nudge the first
    // row down; it never applies here because the page always overflows.)
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Receive Addrs";
    if (!cfg["top_nav"].contains("show_back_button")) cfg["top_nav"]["show_back_button"] = true;
    cfg["button_list"] = button_list;
    cfg["is_bottom_list"] = false;
    cfg["is_button_text_centered"] = false;

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // --- Re-font every button + arm each address row's focus-reveal ------------------
    // The scaffold builds labels in the proportional BUTTON_FONT; the Python screen
    // renders the whole list (addresses AND the "Next N" action) in monospace. While
    // re-fonting, register each ADDRESS row's full "{index}:{address}" as its focus-reveal
    // text (label_set_focus_reveal): at rest the row shows its truncated head...tail (the
    // label the scaffold built); when the row gains focus the nav layer swaps in the full
    // address and marquee-scrolls it, then reverts on defocus — so the reveal follows
    // focus down EVERY row, not just the first. Start each row CLIPPED so the first focus
    // transition (CLIP -> SCROLL_CIRCULAR) actually fires the *paced* 40 px/sec marquee
    // promotion (label_set_line_autoscroll) instead of being short-circuited into LVGL's
    // unpaced default scroll (the "scrolls way too fast" case). The trailing "Next N"
    // action has no full form and is left untouched.
    for (size_t i = 0; i < screen.button_list_count; ++i) {
        lv_obj_t *label = button_text_label(screen.button_list[i]);
        if (!label) continue;
        lv_obj_set_style_text_font(label, fixed_font, LV_PART_MAIN);
        if (i < addresses.size()) {
            lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
            label_set_focus_reveal(label, full_labels[i].c_str());
        }
    }

    // bind_screen_navigation applies the initial focus (row 0 by default) through
    // update_visual_focus, which fires button_set_label_marquee on each row — so the
    // focused row's full address is revealed + paced-marquee'd right here on load, and
    // then follows the joystick as the user moves between rows. In touch mode (no
    // persistent selection) nothing is focused, so every row stays truncated — matching
    // Python's "reveal only while selected".
    bind_screen_navigation(cfg, screen, 0);

    load_screen_and_cleanup_previous(screen.screen);
}
