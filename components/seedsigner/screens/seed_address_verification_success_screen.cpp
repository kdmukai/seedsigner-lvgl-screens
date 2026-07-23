// seed_address_verification_success_screen
//
// Python provenance: SeedAddressVerificationSuccessScreen (seed_screens.py, a
// LargeIconStatusScreen subclass).
//
// The confirmation shown after the Address Explorer's brute-force worker matches
// the unverified address to a derivation index. Structurally it is the SUCCESS
// variant of LargeIconStatusScreen (green check hero icon + green single-line
// headline) but with the base class's single wrapped-text body REPLACED by a
// three-part read-out that the generic large_icon_status_screen can't express —
// which is why this needs its own entry point:
//
//   1. FormattedAddress — the verified address in the shared fixed-width,
//      head/middle/tail-colored form, ABBREVIATED to one line (Python max_lines=1).
//   2. the address-type line — "receive address" / "change address" (Python picks
//      it from verified_index_is_change; the host passes it already localized).
//   3. the index line — "index N" (Python _("index {}").format(verified_index);
//      again pre-formatted + localized by the host).
//
// There is NO back button (Python show_back_button = False): the flow terminates
// at MainMenuView via the single OK button.
//
// Documented deviations from Python:
//   - The hero icon sits ~COMPONENT_PADDING/2 BELOW Python's anchor
//     (top_nav.height - COMPONENT_PADDING/2), the same trade large_icon_status_screen
//     makes: it keeps the top-nav's bottom buffer visible while a tall stack scrolls.
//   - The address block reuses the shared formatted_address() component AS-IS. That
//     component grays the bech32/base58 TYPE prefix and accents the NEXT 7 chars,
//     where Python accents the FIRST 7 (prefix included) — the accepted, already-merged
//     repo standard (see components.cpp). The accent color is mainnet ACCENT (orange),
//     matching Python: SeedAddressVerificationSuccessScreen builds FormattedAddress with
//     no font_accent_color override, so it always uses ACCENT_COLOR regardless of the
//     address's network (a Python behavior, faithfully reproduced — not network-tinted
//     like the live seed_address_verification_screen).
//   - An overflowing headline ellipsis-truncates (LV_LABEL_LONG_DOT) rather than
//     marquee-scrolling; "Address Verified" always fits, so the scroll branch
//     large_icon_status_screen adds for long localized headlines is omitted here.
//   - The address->type and type->index gaps are TIGHTENED from Python's
//     2*COMPONENT_PADDING and COMPONENT_PADDING to COMPONENT_PADDING and
//     COMPONENT_PADDING/2 (see the per-line notes): at Python's spacing the full
//     icon+headline+address+two-line stack overflows the band above the bottom-pinned
//     OK button on 240x240, forcing read-first scrolling before OK is reachable. The
//     tightened gaps let the stack fit with no scroll on every profile (verified
//     scroll_bottom == 0 at 240/320/480/800). The body stays scrollable as a backstop
//     (e.g. an outsized localized type line), but nothing scrolls in the normal case.
//
// Lifecycle: Tier 1 (stateless) — no statics, timers, or heap ctx; all state is
// widget-tree-owned.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title
//            (Python __post_init__: _("Success!")).
//   top_nav.show_back_button  forced false (Python __post_init__:
//            show_back_button = False); a host-supplied value is ignored.
//   top_nav.show_power_button (bool, default false)  Python BaseTopNavScreen default.
//   status_headline           (string, required)     the green single-line headline
//            under the icon (Python: _("Address Verified")).
//   address                   (string, required)     the verified address.
//   address_type_text         (string, required)     the already-localized address-type
//            line — "receive address" / "change address" (Python picks by
//            verified_index_is_change; the host composes + localizes it).
//   index_text                (string, required)     the already-localized index line —
//            "index N" (Python: _("index {}").format(verified_index)).
//   button_list               (array, required, non-empty)  the localized ack button
//            (Python: "OK").
//   is_bottom_list            forced true (Python LargeIconStatusScreen: is_bottom_list
//            = True); a host-supplied value is ignored.
//   initial_selected_index    (int, optional)        overrides the navigation layer's
//            NAV_INDEX_NONE default (read-first: nothing focused while the stack scrolls).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_address_verification_success_screen decl, screen_scaffold_t, text_top_leading
#include "components.h"       // formatted_address + formatted_address_opts_t, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // EDGE_PADDING, COMPONENT_PADDING, SUCCESS_COLOR, BODY_FONT, BODY_FONT_COLOR, KEYBOARD_FONT, ICON_PRIMARY_SCREEN_FONT__SEEDSIGNER, SeedSignerIconConstants
#include "navigation.h"       // NAV_INDEX_NONE
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, network_color

#include "lvgl.h"             // labels, per-object style setters, lv_font glyph metrics

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


void seed_address_verification_success_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: address is the datum this screen exists to confirm;
    // status_headline / address_type_text / index_text / button_list are all
    // user-visible CONTENT, which always arrives localized from the host view layer
    // (a string literal baked here would be English-only by construction). One throw
    // per field, before the scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("status_headline") || !cfg["status_headline"].is_string()) {
        throw std::runtime_error("seed_address_verification_success_screen: status_headline is required and must be a string");
    }
    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("seed_address_verification_success_screen: address is required and must be a string");
    }
    if (!cfg.contains("address_type_text") || !cfg["address_type_text"].is_string()) {
        throw std::runtime_error("seed_address_verification_success_screen: address_type_text is required and must be a string");
    }
    if (!cfg.contains("index_text") || !cfg["index_text"].is_string()) {
        throw std::runtime_error("seed_address_verification_success_screen: index_text is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("seed_address_verification_success_screen: button_list is required and must be a non-empty array");
    }
    std::string status_headline   = cfg["status_headline"].get<std::string>();
    std::string address           = cfg["address"].get<std::string>();
    std::string address_type_text = cfg["address_type_text"].get<std::string>();
    std::string index_text        = cfg["index_text"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). Python
    // SeedAddressVerificationSuccessScreen.__post_init__ unconditionally hides the
    // back button, so it is FORCED (not defaulted) below; show_power_button keeps the
    // Python BaseTopNavScreen default. The localized title is content and must come
    // from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/false,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_address_verification_success_screen");

    cfg["top_nav"]["show_back_button"] = false;   // forced — Python: show_back_button = False
    cfg["is_bottom_list"] = true;                 // forced — Python LargeIconStatusScreen: is_bottom_list = True

    // --- Scaffold ---

    // Scrollable so a tall stack (240x240) scrolls instead of colliding with the
    // bottom-pinned OK button; on taller profiles it simply fits (scroll_bottom == 0).
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/true);

    // --- Body ---

    // Top-anchored stack directly under the top-nav (the scaffold left upper_body a flex
    // column, main-axis START = top, cross-axis CENTER = horizontally centered). Zero the
    // flex row-gap: every inter-element gap is applied per-element below, because the
    // address block's descender-padded container height differs from Python's ink-based
    // spacing accounting (see the type-line margin note). The scaffold's bottom spacer keeps
    // the OK button pinned to the viewport bottom.
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);

    const int32_t display_width = lv_display_get_horizontal_resolution(NULL);

    // 1. Hero icon — the SUCCESS glyph (green check), sized from the active display
    //    profile's primary-screen icon font (Python ICON_PRIMARY_SCREEN_SIZE = 50). The
    //    upper_body flex centers it; strip label padding so the box matches the glyph
    //    line-height and it renders whole.
    lv_obj_t *icon = lv_label_create(screen.upper_body);
    lv_label_set_text(icon, SeedSignerIconConstants::SUCCESS);
    lv_obj_set_style_text_font(icon, &ICON_PRIMARY_SCREEN_FONT__SEEDSIGNER, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(SUCCESS_COLOR), LV_PART_MAIN);
    lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_margin_top(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // 2. Green single-line headline (Python status_headline TextArea, font_color =
    //    status_color = SUCCESS_COLOR, auto_line_break=False). Centered on the full
    //    width, ellipsis-capable (LV_LABEL_LONG_DOT). Python's gap is COMPONENT_PADDING/2
    //    between the icon's visible bottom and the headline's visible top; the label adds
    //    top leading above the caps that PIL doesn't, so subtract it (matches
    //    large_icon_status_screen).
    lv_obj_t *headline = lv_label_create(screen.upper_body);
    lv_label_set_text(headline, status_headline.c_str());
    lv_obj_set_style_text_font(headline, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(headline, lv_color_hex(SUCCESS_COLOR), LV_PART_MAIN);
    lv_obj_set_width(headline, display_width);
    lv_label_set_long_mode(headline, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(headline, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_margin_top(headline,
                                COMPONENT_PADDING / 2 - text_top_leading(&BODY_FONT, status_headline.c_str()),
                                LV_PART_MAIN);

    // 3. FormattedAddress — reuses the shared component AS-IS (its prefix-gray +
    //    next-7-accent styling is the repo standard). ABBREVIATED to one line
    //    (max_lines = 1): head highlight + as much middle as fits, then a gray "..."
    //    up to the highlighted last-7. Head/tail carry the NETWORK accent so
    //    testnet/regtest read distinctly, mirroring the progress screen. Per D-6 the
    //    HOST decides the network and passes cfg["network"]; the screen owns the
    //    palette via network_color() and never infers the network from the address.
    //    Absent network defaults to mainnet (unchanged look until the host supplies it).
    //    Width is the body's inner column (display minus the two edge paddings);
    //    Python's gap below the headline is COMPONENT_PADDING.
    formatted_address_opts_t address_opts = {};
    address_opts.address      = address.c_str();
    address_opts.width        = display_width - 2 * EDGE_PADDING;
    address_opts.max_lines    = 1;                                 // abbreviated head…"..."tail
    address_opts.accent_color = network_color(cfg.value("network", std::string("M")));  // head/tail = network accent
    address_opts.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;     // -> LABEL_FONT_COLOR (gray prefix + middle)
    lv_obj_t *address_block = formatted_address(screen.upper_body, &address_opts);
    lv_obj_set_style_margin_top(address_block, COMPONENT_PADDING, LV_PART_MAIN);

    // Address block metrics for the type-line gap correction below. The shared
    // formatted_address container reserves the fixed font's FULL line height (so the
    // tail's descenders aren't clipped), whereas Python's FormattedAddress.height for a
    // single line is the "1"-ink-box height (char_height) — taller here by the descender
    // reserve (line_height - char_height). Metrics are read from the SAME face and the
    // SAME way formatted_address measures them, so the correction stays right per profile.
    const lv_font_t *address_font = &KEYBOARD_FONT;   // formatted_address's fixed-width face
    int32_t address_line_height = (int32_t)lv_font_get_line_height(address_font);
    lv_font_glyph_dsc_t address_glyph_dsc;
    int32_t address_char_height = lv_font_get_glyph_dsc(address_font, &address_glyph_dsc, (uint32_t)'1', 0)
                                      ? (int32_t)address_glyph_dsc.box_h : address_line_height;

    // 4. Address-type line — "receive address" / "change address", white body font,
    //    centered (Python TextArea default: BODY_FONT_COLOR, is_text_centered=True).
    //    With pad_row = 0 the flex would drop it a full line_height below the address top;
    //    a margin_top of (char_height + COMPONENT_PADDING - line_height) pulls it up into
    //    the descender reserve so the on-screen gap is char_height + COMPONENT_PADDING
    //    (the same idiom seed_address_verification_screen uses).
    //    DEVIATION: Python places this line 2*COMPONENT_PADDING below the address, but at
    //    240x240 the full icon+headline+address+two-line stack then overflows the band above
    //    the bottom-pinned OK button by ~9 px, forcing read-first scrolling before OK is
    //    reachable. Tightened to a single COMPONENT_PADDING so the stack fits with no scroll
    //    on every profile (verified scroll_bottom == 0); the gap still reads as deliberate
    //    breathing room above the two-line read-out.
    lv_obj_t *type_label = lv_label_create(screen.upper_body);
    lv_label_set_text(type_label, address_type_text.c_str());
    lv_obj_set_style_pad_all(type_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(type_label, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(type_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_align(type_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(type_label, display_width - 2 * EDGE_PADDING);
    lv_label_set_long_mode(type_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_margin_top(type_label,
                                address_char_height + COMPONENT_PADDING - address_line_height,
                                LV_PART_MAIN);

    // 5. Index line — "index N", white body font, centered (Python TextArea default).
    //    Reclaim the index label's own top leading so the visible gap is measured from the
    //    caps, not the taller LVGL box (the type label's own bottom leading fills the rest).
    //    DEVIATION: Python's gap here is COMPONENT_PADDING; tightened to COMPONENT_PADDING/2
    //    as part of the no-scroll fit at 240x240 (see the type-line note). The two read-out
    //    lines still sit clearly apart.
    lv_obj_t *index_label = lv_label_create(screen.upper_body);
    lv_label_set_text(index_label, index_text.c_str());
    lv_obj_set_style_pad_all(index_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(index_label, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(index_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_align(index_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(index_label, display_width - 2 * EDGE_PADDING);
    lv_label_set_long_mode(index_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_margin_top(index_label,
                                COMPONENT_PADDING / 2 - text_top_leading(&BODY_FONT, index_text.c_str()),
                                LV_PART_MAIN);

    // --- Navigation + load ---

    // NAV_INDEX_NONE (read-first): when the stack fits, the single OK button is reached
    // with one joystick step; when it overflows the 240x240 viewport the user scrolls
    // through the read-out before OK becomes selectable (bind_screen_navigation
    // auto-detects the overflow). The host may still pre-focus via initial_selected_index.
    bind_screen_navigation(cfg, screen, NAV_INDEX_NONE);

    load_screen_and_cleanup_previous(screen.screen);
}
