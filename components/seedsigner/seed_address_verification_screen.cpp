#include "screen_scaffold.h" // parse/scaffold/nav/load helpers (defined in seedsigner.cpp)
#include "seedsigner.h"      // screen_scaffold_t, this screen's decl (extern "C")
#include "components.h"      // formatted_address()
#include "gui_constants.h"   // colors, COMPONENT_PADDING, EDGE_PADDING, BODY_FONT, LABEL_FONT_COLOR
#include "navigation.h"      // nav_body_layout_t, NAV_BODY_VERTICAL

#include "lvgl.h"

#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// SeedAddressVerificationScreen (Python seed_screens.py:1415)
//
// The live "Verify Address" screen shown while the brute-force thread scans
// derivation indexes for a match. Structurally a bottom-pinned ButtonListScreen
// (Python is_bottom_list = True) with TWO read-only components pinned directly
// under the top nav — Python TOP-anchors the stack (FormattedAddress.screen_y =
// top_nav.height, i.e. immediately below the nav; NOT vertically centered like
// SeedFinalize / PSBTAddressDetails):
//
//   [1] FormattedAddress — the unverified address in the shared, fixed-width,
//       head/middle/tail colored form, single-line compact (Python max_lines=1).
//   [2] TextArea — the "<sig_type> - <script_type>" line (+ " (<network>)" when
//       not mainnet), small and gray (Python font_size=LABEL_FONT_SIZE,
//       font_color=LABEL_FONT_COLOR, is_text_centered=True). Python places it a
//       COMPONENT_PADDING gap below the address block.
//
// There is NO back button (Python show_back_button = False): this is a live
// verification screen the user leaves via Skip 10 / Cancel, not by backing out.
//
// The address block reuses the shared formatted_address() component AS-IS. That
// component intentionally diverges from Python's FormattedAddress — it grays the
// bech32/base58 TYPE prefix (e.g. "bc1q") and accents the NEXT 7 chars, whereas
// Python accents the FIRST 7 (prefix included). This is the accepted, already-
// merged repo standard (see components.cpp), so the head coloring differs from the
// reference PNG in exactly that documented way; everything else matches.
//
// Python renders the live "Checking address N" progress line below the type/network
// text via a ProgressThread; that thread doesn't run in the static screenshot
// generator (and isn't part of the screen's fixed layout), so it is intentionally
// not reproduced here — matching the reference capture.
//
// cfg:
//   top_nav.title            — screen title (default "Verify Address"). show_back_button
//                              is forced false (Python show_back_button = False).
//   address (str, req.)      — the unverified address being scanned.
//   type_network (str, req.) — the pre-formatted "<sig_type> - <script_type>[ (<network>)]"
//                              line (the host/View composes it from the localized
//                              sig-type, script-type, and network display names).
//   button_list (array)      — action buttons (default ["Skip 10", "Cancel"]).
// ---------------------------------------------------------------------------
// --- Live progress push (host-driven, mirrors qr_display_set_frame) -----------------
// The single active seed_address_verification_screen's progress label. The host owns the
// background brute-force worker + the match logic (Python BruteForceAddressVerificationThread
// + ThreadsafeCounters); this screen owns no worker. The host pushes an already-localized
// "Checking address N" line through seed_address_verification_set_progress() on the UI
// thread — the same live-push shape qr_display_set_frame uses (g_qr_ctx). Cleared on the
// screen's LV_EVENT_DELETE with an identity guard so a freshly-built screen isn't nulled by
// the previous one's teardown.
static lv_obj_t *g_addr_verify_progress = nullptr;

static void addr_verify_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    lv_obj_t *progress_label = (lv_obj_t *)lv_event_get_user_data(e);
    if (g_addr_verify_progress == progress_label) g_addr_verify_progress = nullptr;
}

// Update the in-place "checking…" progress line on a live seed_address_verification_screen.
// Host pushes already-localized text (the library holds no strings — same rule as
// qr_display's brighter/darker_text). Safe no-op when no such screen is active (same
// contract as qr_display_set_frame): the host may push after the screen has closed.
extern "C" void seed_address_verification_set_progress(const char *progress_text) {
    if (!g_addr_verify_progress || !progress_text) return;
    lv_label_set_text(g_addr_verify_progress, progress_text);
}

void seed_address_verification_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required data (host-derived): the unverified address + the pre-formatted
    // sig-type/script-type/network line.
    if (!cfg.contains("address") || !cfg["address"].is_string()) {
        throw std::runtime_error("seed_address_verification_screen requires an \"address\" string");
    }
    if (!cfg.contains("type_network") || !cfg["type_network"].is_string()) {
        throw std::runtime_error("seed_address_verification_screen requires a \"type_network\" string");
    }
    std::string address      = cfg["address"].get<std::string>();
    std::string type_network = cfg["type_network"].get<std::string>();

    // Force the SeedAddressVerificationScreen shape onto the scaffold cfg: a titled,
    // bottom-pinned, back-button-less button list (Python is_bottom_list = True,
    // show_back_button = False). The View supplies the localized title + button
    // labels; default all so a bare cfg still renders.
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Verify Address";
    cfg["top_nav"]["show_back_button"] = false;                    // Python: show_back_button = False
    cfg["is_bottom_list"] = true;                                  // Python: is_bottom_list = True
    if (!cfg.contains("button_list")) cfg["button_list"] = json::array({ "Skip 10", "Cancel" });

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false, nullptr);

    // Top-anchored stack (Python FormattedAddress.screen_y = top_nav.height): leave
    // upper_body at its default LV_SIZE_CONTENT top position — the scaffold body sits
    // directly under the top nav with zero top padding, so its first child's top edge
    // lands at top_nav.height, exactly like Python. The scaffold already made
    // upper_body a flex column (main-axis START = top, cross-axis CENTER = horizontal),
    // so the two children stack under the nav, horizontally centered. Keep the bottom
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
    std::string network = cfg.value("network", std::string("mainnet"));
    uint32_t net_color = ACCENT_COLOR;
    if      (network == "testnet") net_color = (uint32_t)TESTNET_COLOR;
    else if (network == "regtest") net_color = (uint32_t)REGTEST_COLOR;

    // [1] FormattedAddress — reuses the shared formatted_address AS-IS (its bech32/base58
    // prefix-gray + next-7-accent styling is the accepted repo standard; the host derives
    // the address). Width is the body's inner column (full display minus the scaffold's edge
    // padding). Fixed to the ABBREVIATED TWO-LINE form (max_lines = 2): the head highlight +
    // as much middle as fits, then a gray "..." skipping the rest up to the preserved
    // highlighted last-7. A bounded 2-line height leaves consistent room for the type/network
    // line [2], the live progress line [3], and the bottom-pinned buttons' edge padding — a
    // full multi-line address crowded the progress line out and pushed Cancel to the screen
    // edge.
    const int32_t W = lv_display_get_horizontal_resolution(NULL);

    formatted_address_opts_t fo = {};
    fo.address      = address.c_str();
    fo.width        = W - 2 * EDGE_PADDING;
    fo.max_lines    = 2;                                       // abbreviated two-line head…"..."tail
    fo.accent_color = net_color;                              // network-specific head/tail highlight
    fo.base_color   = SEEDSIGNER_ICON_COLOR_DEFAULT;          // -> LABEL_FONT_COLOR (gray)
    formatted_address(screen.upper_body, &fo);

    // [2] Type/network line — "<sig_type> - <script_type>[ (<network>)]", small and
    // gray, horizontally centered (Python TextArea font_size = LABEL_FONT_SIZE,
    // font_color = LABEL_FONT_COLOR, is_text_centered = True). Python's LABEL_FONT_SIZE
    // = BODY_FONT_MIN_SIZE = 15 in the default OpenSans-Regular body face; the profile's
    // role fonts don't include that size, so rasterize it on demand via
    // seedsigner_latin_font() (UNSCALED base px, profile multiplier applied inside) —
    // the same helper btc_amount / the fingerprint value use for body-relative sizes.
    // LV_SIZE_CONTENT keeps it on one line (it fits the reference width); the
    // upper_body's cross-axis CENTER centers the whole label.
    lv_obj_t *type_label = lv_label_create(screen.upper_body);
    lv_label_set_text(type_label, type_network.c_str());
    lv_obj_set_style_pad_all(type_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(type_label, seedsigner_latin_font(15), LV_PART_MAIN);  // LABEL_FONT_SIZE base
    lv_obj_set_style_text_color(type_label, lv_color_hex(LABEL_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_align(type_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    // Wrap a long type/network string (e.g. "… (Testnet)") instead of clipping it at the
    // screen edge — Python renders this as a TextArea (auto_line_break). Fixing the width
    // lets a long line wrap+center; a short one (the mainnet default) still fits one line.
    lv_obj_set_width(type_label, W - 2 * EDGE_PADDING);
    lv_label_set_long_mode(type_label, LV_LABEL_LONG_WRAP);
    // When it DOES wrap (e.g. testnet/regtest, whose network suffix drops to a second line),
    // LVGL's default advance leaves a loose gap above that line. Tighten it (~20% off the
    // declared line_height) so the second line sits snug — enough to strip the loose leading
    // while still clearing the first line's descenders + the parens' descenders below.
    {
        int32_t tlh = (int32_t)lv_font_get_line_height(seedsigner_latin_font(15));
        lv_obj_set_style_text_line_space(type_label, -(tlh / 5), LV_PART_MAIN);
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
    const lv_font_t *addr_font = &KEYBOARD_FONT;   // formatted_address's fixed-width face
    int32_t addr_line_h = (int32_t)lv_font_get_line_height(addr_font);
    lv_font_glyph_dsc_t addr_gd;
    int32_t addr_char_h = lv_font_get_glyph_dsc(addr_font, &addr_gd, (uint32_t)'1', 0)
                              ? (int32_t)addr_gd.box_h : addr_line_h;
    lv_obj_set_style_margin_top(type_label,
                                addr_char_h + COMPONENT_PADDING - addr_line_h,
                                LV_PART_MAIN);

    // [3] Live "Checking address N" progress line (Python ProgressThread). Body font,
    // centered, a COMPONENT_PADDING gap below the type/network line. The host pushes
    // updates via seed_address_verification_set_progress() while its background worker
    // scans derivation indexes; the initial text comes from cfg["progress_text"] so the
    // web-runner demo shows a representative line and a freshly-built live screen isn't
    // blank ("" renders nothing until the first host push). NOTE: the static Python
    // reference PNG omits this line (its ProgressThread doesn't run in the screenshot
    // generator), so rendering it is an intentional divergence — the live readout is the
    // whole point of this screen. Wrapped so a host may push index + candidate address.
    std::string progress_text = cfg.value("progress_text", std::string(""));
    lv_obj_t *progress_label = lv_label_create(screen.upper_body);
    lv_label_set_text(progress_label, progress_text.c_str());
    lv_obj_set_style_pad_all(progress_label, 0, LV_PART_MAIN);
    lv_obj_set_style_text_font(progress_label, &BODY_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_color(progress_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_align(progress_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(progress_label, W - 2 * EDGE_PADDING);
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
    g_addr_verify_progress = progress_label;
    lv_obj_add_event_cb(screen.screen, addr_verify_cleanup_cb, LV_EVENT_DELETE, progress_label);

    bind_screen_navigation(
        cfg,
        screen,
        screen.button_list_count > 0 ? screen.button_list : NULL,
        screen.button_list_count,
        NAV_BODY_VERTICAL,
        0   // default the first action button (Skip 10) selected, like button_list_screen
    );

    load_screen_and_cleanup_previous(screen.screen);
}
