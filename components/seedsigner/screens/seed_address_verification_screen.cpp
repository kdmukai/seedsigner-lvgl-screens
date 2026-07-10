// seed_address_verification_screen
//
// Python provenance: SeedAddressVerificationScreen (seed_screens.py)
//
// The live "Verify Address" screen shown while the host's brute-force worker
// scans derivation indexes for a match. Structurally a bottom-pinned
// ButtonListScreen (Python is_bottom_list = True) with a read-only stack pinned
// directly under the top nav — Python TOP-anchors it (FormattedAddress.screen_y
// = top_nav.height, i.e. immediately below the nav; NOT vertically centered
// like seed_finalize / psbt_address_details):
//
//   1. FormattedAddress — the unverified address in the shared, fixed-width,
//      head/middle/tail colored form.
//   2. TextArea — the "<sig_type> - <script_type>" line (+ " (<network>)" when
//      not mainnet), small and gray (Python font_size=LABEL_FONT_SIZE,
//      font_color=LABEL_FONT_COLOR, is_text_centered=True), a COMPONENT_PADDING
//      gap below the address block.
//   3. the live "Checking address N" progress line (Python ProgressThread),
//      updated in place by the host through the extern "C" push API below.
//
// There is NO back button (Python show_back_button = False): this is a live
// verification screen the user leaves via Skip 10 / Cancel, not by backing out.
//
// Documented deviations from Python:
//   - The address block reuses the shared formatted_address() component AS-IS.
//     That component intentionally diverges from Python's FormattedAddress — it
//     grays the bech32/base58 TYPE prefix (e.g. "bc1q") and accents the NEXT 7
//     chars, whereas Python accents the FIRST 7 (prefix included). This is the
//     accepted, already-merged repo standard (see components.cpp), so the head
//     coloring differs from the reference PNG in exactly that documented way.
//   - The address is fixed to the ABBREVIATED TWO-LINE form (max_lines = 2)
//     where Python uses max_lines = 1: the bounded height leaves consistent room
//     for the type/network line, the live progress line, and the bottom-pinned
//     buttons' edge padding (see the body comment at step 1).
//   - Python renders the progress line via a ProgressThread that doesn't run in
//     the static screenshot generator, so the reference PNG omits it; rendering
//     it here is intentional — the live readout is the whole point of the screen.
//
// Lifecycle (stateful, Tier 2): one module-static live-push pointer cleared by
// an identity-guarded LV_EVENT_DELETE callback on the screen root (the
// qr_display g_qr_ctx pattern). No heap ctx, no timers — the pushed-to label is
// widget-tree-owned.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title
//            (Python: _("Verify Address")).
//   top_nav.show_back_button  (bool, forced false)   Python show_back_button =
//            False; a host-supplied value is ignored.
//   top_nav.show_power_button (bool, default false)  Python BaseTopNavScreen default.
//   address                   (string, required)     the unverified address being
//            scanned.
//   type_network              (string, required)     the pre-formatted
//            "<sig_type> - <script_type>[ (<network>)]" line (the host/View
//            composes it from the localized sig-type, script-type, and network
//            display names).
//   network                   (string, default "mainnet")  selects the address
//            head/tail accent color; this screen recognizes the long names
//            "mainnet"/"testnet"/"regtest" only (see the reserved-mapping note
//            in the body).
//   progress_text             (string, default "")   initial progress line; the
//            empty default renders blank until the first host push — a live-push
//            contract, not missing content (the host worker owns the text).
//   button_list               (array, required, non-empty)  the localized action
//            buttons (Python: "Skip 10", "Cancel").
//   is_bottom_list            (forced true)          Python: is_bottom_list = True.
//   initial_selected_index    (int, optional)        override the default focused
//            button index (navigation layer; default 0 = the first action button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_address_verification_screen + seed_address_verification_set_progress decls, screen_scaffold_t, text_top_leading
#include "components.h"       // formatted_address + formatted_address_opts_t, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // COMPONENT_PADDING, EDGE_PADDING, ACCENT_COLOR, TESTNET_COLOR, REGTEST_COLOR, BODY_FONT, BODY_FONT_COLOR, LABEL_FONT_COLOR, KEYBOARD_FONT, seedsigner_latin_font
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_label + per-object style setters, lv_font glyph metrics

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string

using json = nlohmann::json;


// ---------------------------------------------------------------------------
// Live progress push (host-driven, mirrors qr_display_set_frame)
// ---------------------------------------------------------------------------

namespace {

// The single active seed_address_verification_screen's progress label. The host
// owns the background brute-force worker + the match logic (Python
// BruteForceAddressVerificationThread + ThreadsafeCounters); this screen owns no
// worker. The host pushes an already-localized "Checking address N" line through
// seed_address_verification_set_progress() on the UI thread — the same live-push
// shape qr_display_set_frame uses (g_qr_ctx). Cleared on the screen's
// LV_EVENT_DELETE with an identity guard so a freshly-built screen isn't nulled
// by the previous one's teardown.
//
// No heap ctx / allocation idiom: the state is this bare pointer to a
// widget-tree-owned label, so teardown is just the identity-guarded null below.
lv_obj_t *g_seed_address_verification_progress = nullptr;

// LV_EVENT_DELETE teardown on the screen root: forget the push target iff it
// still belongs to the screen being deleted (identity guard — a re-rendered
// screen registers its own label BEFORE the old screen is torn down).
void seed_address_verification_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    lv_obj_t *progress_label = (lv_obj_t *)lv_event_get_user_data(e);
    if (g_seed_address_verification_progress == progress_label) g_seed_address_verification_progress = nullptr;
}

}  // namespace


// Update the in-place "checking…" progress line on a live seed_address_verification_screen.
// Host pushes already-localized text (the library holds no strings — same rule as
// qr_display's brighter/darker_text). Safe no-op when no such screen is active (same
// contract as qr_display_set_frame): the host may push after the screen has closed.
extern "C" void seed_address_verification_set_progress(const char *progress_text) {
    if (!g_seed_address_verification_progress || !progress_text) return;
    lv_label_set_text(g_seed_address_verification_progress, progress_text);
}


void seed_address_verification_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: address + type_network are the host-derived verification
    // data this screen exists to show; button_list is user-visible CONTENT, which
    // always arrives localized from the host view layer (a string literal baked
    // here would be English-only by construction). One throw per field, before the
    // scaffold exists, so no throw path can leak LVGL objects.
    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("seed_address_verification_screen: address is required and must be a string");
    }
    if (!cfg.contains("type_network") || !cfg["type_network"].is_string()) {
        throw std::runtime_error("seed_address_verification_screen: type_network is required and must be a string");
    }
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("seed_address_verification_screen: button_list is required and must be a non-empty array");
    }
    std::string address      = cfg["address"].get<std::string>();
    std::string type_network = cfg["type_network"].get<std::string>();

    // Structural defaults (write-if-absent, never user-visible text). The localized
    // title itself is content and must come from the host. Python
    // SeedAddressVerificationScreen.__post_init__ unconditionally hides the back
    // button, so it is FORCED (not defaulted) below; show_power_button keeps the
    // Python BaseTopNavScreen default.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/false,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_address_verification_screen");

    cfg["top_nav"]["show_back_button"] = false;   // forced, not defaulted — Python: show_back_button = False
    cfg["is_bottom_list"] = true;                 // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false, /*title_font=*/nullptr);

    // --- Body ---

    // Top-anchored stack (Python FormattedAddress.screen_y = top_nav.height): leave
    // upper_body at its default LV_SIZE_CONTENT top position — the scaffold body sits
    // directly under the top nav with zero top padding, so its first child's top edge
    // lands at top_nav.height, exactly like Python. The scaffold already made
    // upper_body a flex column (main-axis START = top, cross-axis CENTER = horizontal),
    // so the children stack under the nav, horizontally centered. Keep the bottom
    // spacer growing so the action buttons stay pinned to the viewport bottom.
    //
    // Inter-child gap is applied per-element below (not as a uniform pad_row), because
    // Python's address-to-text spacing is measured from the address's TEXT height, not
    // the descender-padded container height — see the type-line margin note below.
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);

    // Network accent color for the address head/tail highlight. Python colors the
    // verifiable chars by network (components.py get_address_..): mainnet -> ACCENT
    // (orange), testnet -> TESTNET_COLOR (green), regtest -> REGTEST_COLOR (cyan). The
    // host passes cfg["network"]; default mainnet. (The prefix + middle stay gray.)
    // NOTE: this inline mapping recognizes the LONG network names only, unlike the
    // shared network_color()/resolve_address_network() helpers ("M"/"T"/"R" codes +
    // address-prefix inference) — slated for shared-helper migration once that
    // behavior divergence is adjudicated.
    std::string network = cfg.value("network", std::string("mainnet"));
    uint32_t net_color = ACCENT_COLOR;
    if      (network == "testnet") net_color = (uint32_t)TESTNET_COLOR;
    else if (network == "regtest") net_color = (uint32_t)REGTEST_COLOR;

    // 1. FormattedAddress — reuses the shared formatted_address AS-IS (its
    //    bech32/base58 prefix-gray + next-7-accent styling is the accepted repo
    //    standard; the host derives the address). Width is the body's inner column
    //    (full display minus the scaffold's edge padding). Fixed to the ABBREVIATED
    //    TWO-LINE form (max_lines = 2): the head highlight + as much middle as fits,
    //    then a gray "..." skipping the rest up to the preserved highlighted last-7.
    //    A bounded 2-line height leaves consistent room for the type/network line
    //    (step 2), the live progress line (step 3), and the bottom-pinned buttons'
    //    edge padding — a full multi-line address crowded the progress line out and
    //    pushed Cancel to the screen edge.
    const int32_t display_width = lv_display_get_horizontal_resolution(NULL);

    formatted_address_opts_t address_opts = {};
    address_opts.address      = address.c_str();
    address_opts.width        = display_width - 2 * EDGE_PADDING;
    address_opts.max_lines    = 2;                                 // abbreviated two-line head…"..."tail
    address_opts.accent_color = net_color;                         // network-specific head/tail highlight
    address_opts.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;     // -> LABEL_FONT_COLOR (gray)
    formatted_address(screen.upper_body, &address_opts);

    // 2. Type/network line — "<sig_type> - <script_type>[ (<network>)]", small and
    //    gray, horizontally centered (Python TextArea font_size = LABEL_FONT_SIZE,
    //    font_color = LABEL_FONT_COLOR, is_text_centered = True). Python's
    //    LABEL_FONT_SIZE = BODY_FONT_MIN_SIZE = 15 in the default OpenSans-Regular
    //    body face; the profile's role fonts don't include that size, so rasterize
    //    it on demand via seedsigner_latin_font() (UNSCALED base px, profile
    //    multiplier applied inside) — the same helper btc_amount / the fingerprint
    //    value use for body-relative sizes. The upper_body's cross-axis CENTER
    //    centers the whole label.
    lv_obj_t *type_label = lv_label_create(screen.upper_body);
    lv_label_set_text(type_label, type_network.c_str());
    lv_obj_set_style_pad_all(type_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(type_label, seedsigner_latin_font(15), LV_PART_MAIN);  // LABEL_FONT_SIZE base
    lv_obj_set_style_text_color(type_label, lv_color_hex(LABEL_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_align(type_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    // Wrap a long type/network string (e.g. "… (Testnet)") instead of clipping it at the
    // screen edge — Python renders this as a TextArea (auto_line_break). Fixing the width
    // lets a long line wrap+center; a short one (the mainnet default) still fits one line.
    lv_obj_set_width(type_label, display_width - 2 * EDGE_PADDING);
    lv_label_set_long_mode(type_label, LV_LABEL_LONG_WRAP);
    // When it DOES wrap (e.g. testnet/regtest, whose network suffix drops to a second line),
    // LVGL's default advance leaves a loose gap above that line. Tighten it (~20% off the
    // declared line_height) so the second line sits snug — enough to strip the loose leading
    // while still clearing the first line's descenders + the parens' descenders below.
    {
        int32_t type_line_height = (int32_t)lv_font_get_line_height(seedsigner_latin_font(15));
        lv_obj_set_style_text_line_space(type_label, -(type_line_height / 5), LV_PART_MAIN);
    }

    // Place the type line exactly where Python does: COMPONENT_PADDING below the address
    // block, where Python's FormattedAddress.height (single line) == the fixed-width
    // font's "Q"-ink-box height (char_height), NOT the full line height. The shared
    // formatted_address container instead reserves the full line_h so the tail's
    // descenders ("q86f5ek") aren't clipped — taller than Python's accounting by the
    // descender reserve (line_h - char_height). With pad_row = 0 the flex would drop the
    // type line line_h below the address top; a margin_top of (char_height +
    // COMPONENT_PADDING - line_h) pulls it up into that reserve so the on-screen gap is
    // Python's char_height + COMPONENT_PADDING. Metrics are read from the SAME face and
    // the SAME way formatted_address measures them, so this stays correct across profiles.
    const lv_font_t *address_font = &KEYBOARD_FONT;   // formatted_address's fixed-width face
    int32_t address_line_height = (int32_t)lv_font_get_line_height(address_font);
    lv_font_glyph_dsc_t address_glyph_dsc;
    int32_t address_char_height = lv_font_get_glyph_dsc(address_font, &address_glyph_dsc, (uint32_t)'1', 0)
                                      ? (int32_t)address_glyph_dsc.box_h : address_line_height;
    lv_obj_set_style_margin_top(type_label,
                                address_char_height + COMPONENT_PADDING - address_line_height,
                                LV_PART_MAIN);

    // 3. Live "Checking address N" progress line (Python ProgressThread). Body font,
    //    centered, below the type/network line. The host pushes updates via
    //    seed_address_verification_set_progress() while its background worker scans
    //    derivation indexes; the initial text comes from cfg["progress_text"] so the
    //    web-runner demo shows a representative line and a freshly-built live screen
    //    isn't blank ("" renders nothing until the first host push). NOTE: the static
    //    Python reference PNG omits this line (its ProgressThread doesn't run in the
    //    screenshot generator), so rendering it is an intentional divergence — the
    //    live readout is the whole point of this screen. Wrapped so a host may push
    //    index + candidate address.
    std::string progress_text = cfg.value("progress_text", std::string(""));
    lv_obj_t *progress_label = lv_label_create(screen.upper_body);
    lv_label_set_text(progress_label, progress_text.c_str());
    lv_obj_set_style_pad_all(progress_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(progress_label, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(progress_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_align(progress_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(progress_label, display_width - 2 * EDGE_PADDING);
    lv_label_set_long_mode(progress_label, LV_LABEL_LONG_WRAP);
    // Sit the progress line snug under the type/network line: reclaim its own top leading and
    // use a half-padding gap. A full COMPONENT_PADDING left too big a gap below a wrapped
    // "(Testnet)" line, and the extra height nudged the bottom-pinned Cancel button into a
    // tiny scroll — this reclaims that space so both buttons keep their bottom edge padding.
    lv_obj_set_style_margin_top(
        progress_label,
        COMPONENT_PADDING / 2 - text_top_leading(&BODY_FONT, progress_text.c_str()),
        LV_PART_MAIN);

    // Register as the live push target for seed_address_verification_set_progress(); clear
    // on the screen's delete (identity-guarded so a later screen isn't nulled).
    g_seed_address_verification_progress = progress_label;
    lv_obj_add_event_cb(screen.screen, seed_address_verification_cleanup_cb, LV_EVENT_DELETE, progress_label);

    // --- Navigation + load ---

    // Menu-style default index: an action list always has a selection, so the first
    // button (Skip 10) starts focused, like button_list_screen (the host may
    // override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
