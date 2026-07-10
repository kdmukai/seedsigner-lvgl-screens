// large_icon_status_screen
//
// Python provenance: LargeIconStatusScreen (screen.py). The Python subclass
// hierarchy — WarningScreen / DireWarningScreen / ErrorScreen layered via
// WarningEdgesMixin — collapses into this single function selected by
// cfg.status_type; a fifth "custom" status renders any caller-supplied hero
// icon + color (PSBTFinalize's SIGN prompt, the microSD notification, ...)
// so one screen covers every large-icon prompt without a bespoke entry point.
//
// A status/acknowledge screen: hero icon + status-colored single-line headline
// + wrapped body text above a bottom-pinned ack button; the pressed button
// index returns through the standard navigation callback. Warning-class
// variants pulse a colored border overlay around the whole screen.
//
// Layout (create_top_nav_screen_scaffold with cfg.button_list +
// is_bottom_list forced true):
//
//   ┌────────────────────────────────┐
//   │ TopNav (title; optional back)  │
//   ├────────────────────────────────┤
//   │  [hero icon, status_color]     │
//   │  [headline, status_color]      │
//   │  [body text, body color]       │
//   │  ┄ ┄ ┄ flex spacer ┄ ┄ ┄ ┄ ┄  │
//   │  [ OK / I understand ]         │
//   └────────────────────────────────┘
//
// This function adds only the icon, headline, and body text into
// screen.upper_body; the scaffold owns the spacer and the button. After the
// widgets exist, four sequential measure-correct passes (named under
// --- Geometry --- below) bake shaped glyph runs early, reclaim a small
// overflow into the top-nav's bottom buffer, balance the body's wrapped
// lines, and vertically center the body in its free band.
//
// Documented deviations from Python:
//   - The hero icon sits ~COMPONENT_PADDING/2 BELOW Python's anchor
//     (top_nav.height - COMPONENT_PADDING/2): the trade keeps the top-nav
//     buffer visible while a tall body scrolls beneath it.
//   - An overflowing LTR headline start-justifies and auto-scrolls (marquee)
//     instead of Python's ellipsis truncation; a fitting headline (and any
//     RTL headline) keeps the centered, ellipsis-capable rendering.
//   - A body too tall for the viewport scrolls (with read-first navigation)
//     rather than clipping inside Python's fixed-height TextArea.
//
// Content-policy note: apply_status_type_defaults injects English content
// defaults (the per-status top_nav.title and single ack-button label). These
// status-type content defaults predate the content policy
// (docs/screen-conformance-spec.md §5) and are shared by the whole status
// family, so they are NOT converted here; they are slated for
// require-and-throw at status-family rollout.
//
// Lifecycle: Tier 1 (stateless) — no statics, timers, or heap ctx; all state is
// widget-tree-owned.
//
// cfg:
//   status_type      (string, required)    "success" | "warning" | "dire_warning" |
//            "error" | "custom" — selects the hero icon, status color, warning-edges
//            default, text inset (1x EDGE_PADDING for success/custom, 2x for the
//            warning class), and the reserved default title / button label.
//   icon             (string, "custom" only, default INFO icon)  hero icon glyph —
//            a raw icon-font codepoint string, the button/top-nav icon convention.
//   icon_color       (string, "custom" only, default INFO_COLOR)  "#rrggbb" color
//            for the hero icon and headline.
//   status_headline  (string, optional)    single-line status-colored headline under
//            the icon (Python: auto_line_break=False); absent/empty = no headline.
//   text             (string, optional)    wrapped body text; absent/empty = no body.
//   warning_edges    (bool, default per status_type: true for warning/dire_warning/
//            error, false for success/custom)  pulsing border overlay; throws if
//            present and non-boolean.
//   top_nav.title    (string, default per status_type)  RESERVED English content
//            default — see the content-policy note above.
//   button_list      (array, default per status_type)   the ack button(s); defaults
//            to the single per-status label — RESERVED English content default.
//   is_bottom_list   forced true (Python: LargeIconStatusScreen.__post_init__);
//            a host-supplied value is ignored.
//   top_nav.show_back_button / top_nav.show_power_button pass through untouched to
//            the scaffold's own defaults (back=true, power=false).
//   initial_selected_index    (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / add_warning_edges_overlay / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // large_icon_status_screen decl, screen_scaffold_t fields, text_top_leading
#include "components.h"       // label_subset_text_width, label_set_line_autoscroll (headline fit-or-marquee)
#include "screen_helpers.h"   // status-type trio, parse_hex_color, make_body_text_label, apply_body_tight_line_spacing, apply_rtl_text_to_labels, balance_wrapped_label_column
#include "gui_constants.h"    // EDGE_PADDING, COMPONENT_PADDING, TOP_NAV_HEIGHT, TOP_NAV_BUTTON_SIZE, BODY_FONT, ICON_PRIMARY_SCREEN_FONT__SEEDSIGNER, LINE_SCROLL_BEGIN_HOLD_MS
#include "navigation.h"       // NAV_INDEX_NONE
#include "font_registry.h"    // seedsigner_locale_is_rtl, seedsigner_locale_uses_glyph_runs
#include "glyph_runs.h"       // apply_glyph_runs_to_labels, seedsigner_label_run_drawn_height

#include "lvgl.h"             // labels, per-object style setters, layout/coords queries

#include <nlohmann/json.hpp>  // json (cfg reads; the status-type trio writes defaults into it)

#include <cstddef>            // size_t (button re-width loop over screen.button_list_count)
#include <stdexcept>          // std::runtime_error (warning_edges validation)
#include <string>             // std::string

using json = nlohmann::json;


void large_icon_status_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Status-family dispatch: status_type selects the per-status defaults table
    // (icon glyph, status color, warning-edges default, text inset, reserved
    // title/button-label content defaults), which apply_status_type_defaults
    // then injects into cfg before the scaffold reads it.
    status_type_t status = parse_status_type(cfg);
    status_type_defaults_t defaults = defaults_for_status_type(status);
    apply_status_type_defaults(cfg, defaults);

    // For a "custom" status screen the hero icon glyph + color are caller-supplied
    // (raw glyph string + hex color, the same convention as button/top-nav icons), so
    // one screen renders any large-icon prompt: PSBTFinalize's SIGN icon, the microSD
    // notification, etc. custom_icon backs defaults.icon (a const char*) for the render.
    std::string custom_icon;
    if (status == status_type_t::CUSTOM) {
        if (cfg.contains("icon") && cfg["icon"].is_string()) {
            custom_icon   = cfg["icon"].get<std::string>();
            defaults.icon = custom_icon.c_str();
        }
        if (cfg.contains("icon_color") && cfg["icon_color"].is_string()) {
            defaults.color = (int)parse_hex_color(cfg["icon_color"].get<std::string>());
        }
    }

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/true);

    // Status screens use the full screen width for icon / headline / body text
    // (Python's LargeIconStatusScreen positions these on the canvas without a
    // margin). Zero out the body's horizontal padding so upper_body spans
    // the full screen width; re-apply the matching margin to the scaffold's
    // bottom-list buttons so they keep their EDGE_PADDING side gutters.
    lv_obj_set_style_pad_left(screen.body, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(screen.body, 0, LV_PART_MAIN);
    for (size_t i = 0; i < screen.button_list_count; ++i) {
        lv_obj_set_width(screen.button_list[i],
                         lv_obj_get_width(screen.body) - 2 * EDGE_PADDING);
    }

    // Most status screens fit the viewport and never scroll. But a tall body
    // (long warning text on a small display) can push the bottom button off-screen,
    // so KEEP the body scrollable (the scaffold already set scroll_dir=VER +
    // SCROLLBAR_MODE_AUTO): touch gets native drag-to-scroll, and bind_screen_
    // navigation auto-detects overflow to opt the joystick nav into scroll-then-
    // button stepping. When content fits, scroll_bottom == 0 — the scrollbar stays
    // hidden and the flow is the plain one.

    // --- Body ---

    // Zero upper_body's flex row-gap so the icon->headline spacing is ONLY the
    // headline's COMPONENT_PADDING/2 top margin (matching Python's
    // next_y = icon_bottom + COMPONENT_PADDING/2); any inherited row-gap inflates it.
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);

    // 1. Hero icon — colored, centered, sized from the active display profile.
    //    upper_body's flex layout (column, cross-axis center) handles centering. The
    //    icon needs no margin: the body's default pad_top is 0, so the icon renders at
    //    the first available body pixel — just below the top_nav's bottom buffer — and
    //    the glyph fits its label box exactly (box_h == line_height), so it renders
    //    whole (no clip). This sits ~COMPONENT_PADDING/2 below Python's anchor
    //    (top_nav.height - COMPONENT_PADDING/2); the trade keeps the top_nav buffer
    //    visible while the body scrolls beneath it.
    lv_obj_t *icon = lv_label_create(screen.upper_body);
    lv_label_set_text(icon, defaults.icon);
    lv_obj_set_style_text_font(icon, &ICON_PRIMARY_SCREEN_FONT__SEEDSIGNER, LV_PART_MAIN);
    lv_obj_set_style_text_color(icon, lv_color_hex(defaults.color), LV_PART_MAIN);
    lv_obj_set_style_margin_top(icon, 0, LV_PART_MAIN);

    // Strip default label padding so the box matches the font's line_height
    // (cap-height; the icon font has base_line=0), so the icon's bottom edge
    // anchors the headline spacing exactly where Python places it.
    lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // upper_body now spans the full screen width (body padding zeroed above).
    const int32_t upper_body_content_width = lv_obj_get_width(screen.screen);

    // Horizontal inset for the readable text column: the body text, and an
    // overflowing headline when it scrolls, both sit inside this gutter so they
    // share one left/right edge and clear the pulsing warning border (warning
    // variants double it; success uses a single EDGE_PADDING). The hero icon and
    // a centered (fitting) headline still span the full width, matching Python.
    const int32_t text_inset = defaults.text_edge_padding_multiplier * EDGE_PADDING;

    // 2. Optional headline — colored to match status. Single-line (never wraps),
    //    matching Python's auto_line_break=False. A headline that FITS centers on the
    //    full width and is byte-identical to before (LONG_DOT). A headline that
    //    OVERFLOWS start-justifies (LTR=left; shaped RTL via the glyph-run draw) and
    //    auto-scrolls within the text column to reveal the tail — a NEW capability
    //    (Python truncates with an ellipsis; we scroll instead). Overflow is rare
    //    (the fit test is against the full width) but long localized strings hit it.
    if (cfg.contains("status_headline") && cfg["status_headline"].is_string()) {
        std::string headline = cfg["status_headline"].get<std::string>();
        if (!headline.empty()) {
            lv_obj_t *headline_label = lv_label_create(screen.upper_body);
            lv_label_set_text(headline_label, headline.c_str());
            lv_obj_set_style_text_color(headline_label, lv_color_hex(defaults.color), LV_PART_MAIN);
            lv_obj_set_style_text_font(headline_label, &BODY_FONT, LV_PART_MAIN);

            // Measure the rendered width the same way the top_nav title fit-test
            // does: against the label's STORED (presentation-form) text, so the
            // overflow test matches the painted glyphs (see label_subset_text_width).
            // Single-line headline; shaped hi/th still mis-measure here (codepoint
            // width) — a known low-impact gap.
            if (label_subset_text_width(headline_label, &BODY_FONT) > upper_body_content_width &&
                !seedsigner_locale_is_rtl()) {
                // LTR overflow: clamp the label to the text column (so it scrolls
                // within the same gutters as the body, never up to the screen edge
                // or under the warning border), then start-justify (LEFT) +
                // continuous marquee with the initial/per-wrap hold + true 40 px/sec
                // (shared with the top_nav title). The upper_body flex centers the
                // narrower label, yielding equal `text_inset` gutters. Shaped hi/th
                // ride the offset-aware glyph-run draw. RTL (ur) is gated out here
                // to match glyph_run_draw_cb (the offset, scroll start-justify, and
                // content-box clip are all LTR-only for now); an overflowing ur
                // headline keeps the centered LONG_DOT branch below, so ur stays
                // byte-identical until its RTL scroll support lands with the broader
                // ur RTL work.
                int32_t scroll_width = upper_body_content_width - 2 * text_inset;
                if (scroll_width < 16) {
                    scroll_width = 16;
                }
                lv_obj_set_width(headline_label, scroll_width);
                lv_obj_set_style_text_align(headline_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
                label_set_line_autoscroll(headline_label, LINE_SCROLL_BEGIN_HOLD_MS, LINE_SCROLL_BEGIN_HOLD_MS);
            } else {
                // Fits (any locale) or RTL overflow: centered on the full width +
                // ellipsis-capable, exactly as before (byte-identical).
                lv_obj_set_width(headline_label, upper_body_content_width);
                lv_label_set_long_mode(headline_label, LV_LABEL_LONG_DOT);
                lv_obj_set_style_text_align(headline_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            }
            // Python's gap is COMPONENT_PADDING/2 between the icon's visible bottom
            // and the headline's VISIBLE top. The label adds top leading above the
            // caps that PIL does not, so subtract it — otherwise the gap reads ~2x
            // too big and cascades down, jamming the bottom button against the edge.
            // Measure the label's STORED text, not the logical `headline`: with
            // LV_USE_ARABIC_PERSIAN_CHARS, lv_label_set_text rewrote Arabic/Persian
            // into presentation forms (the glyphs actually drawn, and the only ones
            // present in the subset font). Measuring the logical codepoints
            // under-counts the ink → an over-large leading → the fa headline pulls
            // UP into the hero icon. en is unaffected (the rewrite is a no-op there,
            // so the stored text == the logical text).
            int32_t headline_top_leading = text_top_leading(&BODY_FONT, lv_label_get_text(headline_label));
            lv_obj_set_style_margin_top(headline_label, COMPONENT_PADDING / 2 - headline_top_leading, LV_PART_MAIN);
        }
    }

    // 3. Body text — wraps inside the upper_body width minus the
    //    status-type-appropriate edge inset. Warning-class screens use
    //    2 * EDGE_PADDING so text never sits under the pulsing border.
    //    Hoisted to function scope so the fits-case vertical centering below can
    //    reference it after the full layout settles.
    lv_obj_t *body_label = nullptr;
    if (cfg.contains("text") && cfg["text"].is_string()) {
        std::string text = cfg["text"].get<std::string>();
        if (!text.empty()) {
            // Inset the body text by `text_inset` on each side (computed above,
            // shared with the headline's scroll column) so it never sits under the
            // pulsing warning border.
            int32_t text_width = upper_body_content_width - 2 * text_inset;
            if (text_width < 16) {
                text_width = 16;
            }

            body_label = make_body_text_label(screen.upper_body, text.c_str(), text_width);

            // Zero top margin (Python places the body immediately after the
            // headline) + tight, ink-based inter-line spacing matching the PIL
            // reference — both applied by apply_body_tight_line_spacing(), shared
            // with the button_list_screen intro text so the two render identically.
            apply_body_tight_line_spacing(body_label);

            // A custom large-icon screen with no headline (PSBTFinalize's "Click to
            // approve this transaction" under the SIGN icon) needs the text spaced off
            // the icon; Python uses icon_bottom + 2*COMPONENT_PADDING. Restore that gap
            // that apply_body_tight_line_spacing() zeroed for the headline-adjacent case.
            bool has_headline = cfg.contains("status_headline") &&
                                cfg["status_headline"].is_string() &&
                                !cfg["status_headline"].get<std::string>().empty();
            if (status == status_type_t::CUSTOM && !has_headline) {
                lv_obj_set_style_margin_top(body_label, 2 * COMPONENT_PADDING, LV_PART_MAIN);
            }
        }
    }

    // 4. Optional pulsing border. Default per status_type (true for warning /
    //    dire_warning / error), explicitly overridable from JSON.
    bool warning_edges = defaults.warning_edges_default;
    if (cfg.contains("warning_edges")) {
        if (!cfg["warning_edges"].is_boolean()) {
            throw std::runtime_error("large_icon_status_screen: warning_edges must be a boolean");
        }
        warning_edges = cfg["warning_edges"].get<bool>();
    }
    if (warning_edges) {
        add_warning_edges_overlay(screen.screen, defaults.color);
    }

    // --- Geometry ---
    //
    // Four sequential measure-correct passes. Each re-runs lv_obj_update_layout
    // because the pass before it changed heights or margins, and the order is
    // load-bearing: the glyph-run bake must precede every scroll_bottom read (so
    // shaped bodies report their true painted height), the reclaim must precede
    // the fits gate, the balanced wrap changes only the column WIDTH (so the
    // centering math after it is unaffected), and the vertical centering
    // consumes the final settled coordinates.

    // Glyph-run pre-bake pass — bake shaped runs early + grow the body label to
    // the run's true drawn height.
    //
    // For shaped (glyph-run) locales the body's mask is drawn TALLER than the
    // lv_label widget box — the run lays out at the font's full line_height,
    // while the codepoint box is sized at the tighter tight_line_space advance. Bake
    // the runs NOW (normally deferred to the post-load pass) and GROW the body label
    // to the run's true drawn height, so the codepoint box no longer under-reports
    // the painted extent. Every lv_obj_get_scroll_bottom() decision below — reclaim,
    // the fits gate, the centering, and bind_screen_navigation's scroll auto-detect —
    // then measures the real height on ONE code path, so a tall shaped body
    // reclaims/scrolls instead of clipping at the bottom with no scroll path. Mirror
    // the post-load order (RTL flip, then attach); the post-load pass re-runs both as
    // no-ops (RTL idempotent; attach skips already-attached labels). No-op for
    // en/subset (apply_glyph_runs_to_labels self-gates on the active locale; run
    // height returns -1 so no min-height is set).
    //
    // NOTE: this correction is specific to the status body, which sets a TIGHT
    // line_space (tight_line_space, below) so its codepoint box is SHORTER than the
    // run. Plain body labels (make_body_text_label, e.g. button_list_screen's intro
    // text) keep the screen's generous default BODY_LINE_SPACING, so their codepoint
    // box already meets/exceeds the run height and needs no such correction.
    if (seedsigner_locale_uses_glyph_runs()) {
        lv_obj_update_layout(screen.body);
        if (seedsigner_locale_is_rtl()) {
            apply_rtl_text_to_labels(screen.screen);
        }
        apply_glyph_runs_to_labels(screen.screen);
        if (body_label) {
            // run height is content-relative (painted from the content top), so size
            // the box to the drawn height PLUS the label's vertical padding to keep
            // the content area >= the painted run regardless of any theme padding.
            int32_t run_drawn_height = seedsigner_label_run_drawn_height(body_label);
            if (run_drawn_height >= 0) {
                int32_t vertical_padding = lv_obj_get_style_pad_top(body_label, LV_PART_MAIN) +
                                           lv_obj_get_style_pad_bottom(body_label, LV_PART_MAIN);
                lv_obj_set_style_min_height(body_label, run_drawn_height + vertical_padding, LV_PART_MAIN);
            }
        }
    }

    // Reclaim-into-top-nav-buffer pass — reclaim-only-as-needed: if the content
    // overflows by no more than the top_nav's bottom buffer, pull the whole body up
    // by exactly that overflow so it FITS without scrolling — the icon/headline/text
    // rise a few px into the buffer region while the bottom button and its padding
    // stay put. The body keeps its bottom edge, so only the top is reclaimed. Because
    // we pull up by the exact overflow, the result fits (scroll_bottom -> 0) and
    // never scrolls, so scrolled content can never collide with the nav. A larger
    // overflow (more than the buffer can hide) is left alone: the icon stays at the
    // default position and the screen scrolls cleanly under the full buffer
    // (bind_screen_navigation then enables scroll-then-buttons). The two cases are
    // mutually exclusive.
    lv_obj_update_layout(screen.body);
    int32_t overflow = lv_obj_get_scroll_bottom(screen.body);
    int32_t top_nav_bottom_buffer = (TOP_NAV_HEIGHT - TOP_NAV_BUTTON_SIZE) / 2;
    if (overflow > 0 && overflow <= top_nav_bottom_buffer) {
        lv_obj_set_height(screen.body, lv_obj_get_height(screen.body) + overflow);
        lv_obj_align_to(screen.body, screen.top_nav, LV_ALIGN_OUT_BOTTOM_MID, 0, -overflow);
    }

    // Balanced-wrap pass — when the body fits, even out the body text's lines by
    // narrowing its column (keeping the line count). Only the codepoint-rendered
    // (subset/Latin) locales are handled here; shaped glyph-run locales (hi/th)
    // wrap in the render layer and are balanced there. Done before the vertical
    // centering below; it changes only the column width, not the height, so the
    // centering math is unaffected.
    //
    // KNOWN ASYMMETRY (intentional): this subset/Latin balancing is SCOPED to this
    // screen (called only here), while the shaped balancing in glyph_runs.cpp is
    // GLOBAL (every wrapped shaped label, app-wide). So on a NON-status screen with
    // wrapped body text (e.g. a button_list with intro text), shaped locales get
    // balanced but subset/Latin ones do not. The status screen — the original
    // motivation — is covered for all locales. If full cross-locale parity on other
    // screens is ever needed, promote this to a global post-load pass (walk the
    // tree, balance LONG_WRAP non-shaped labels) mirroring apply_glyph_runs_to_labels.
    lv_obj_update_layout(screen.body);
    bool body_fits = lv_obj_get_scroll_bottom(screen.body) == 0;
    if (body_fits && body_label && !seedsigner_locale_uses_glyph_runs()) {
        balance_wrapped_label_column(body_label);
    }

    // Center-body-between-headline-and-button pass — vertically center the body
    // text in the gap between the headline and the bottom button when the content
    // FITS with slack to spare. By default the body text sits directly under the
    // headline (gap 0) and the whole flex-grow spacer sits BELOW it, hard against
    // the button — so the text reads as top-biased. Moving the text down by HALF
    // the below-gap puts equal space above and below it; the spacer (which the
    // shift comes out of) keeps the button pinned, so the button and its bottom
    // padding never move. Skipped when the screen scrolls or only just fits
    // (spacer smaller than the shift): the gate keeps it a no-op there, so
    // reclaimed/overflowing screens are unaffected.
    if (body_label && screen.button_list_count > 0 && screen.button_list_spacer) {
        lv_obj_update_layout(screen.body);
        lv_area_t text_area, button_area;
        lv_obj_get_coords(body_label, &text_area);
        lv_obj_get_coords(screen.button_list[0], &button_area);

        // text bottom -> button top. For a shaped (glyph-run) body the run paints
        // a different height than the codepoint label box, so use the run's true
        // drawn bottom (content top + drawn block height); -1 => no run (en/subset
        // locales), in which case the label box bottom is already correct.
        int32_t text_bottom = text_area.y2;
        int32_t run_drawn_height = seedsigner_label_run_drawn_height(body_label);
        if (run_drawn_height >= 0) {
            lv_area_t body_content_area;
            lv_obj_get_content_coords(body_label, &body_content_area);
            text_bottom = body_content_area.y1 + run_drawn_height;
        }
        int32_t below_gap = button_area.y1 - text_bottom;
        int32_t spacer_height = lv_obj_get_height(screen.button_list_spacer);

        // Shift the body down by half the below-gap to balance the space above and
        // below it. The shift comes out of the flex-grow spacer, so the button stays
        // pinned — but we can never take more than the spacer holds, or the button
        // would move. On TIGHT screens (e.g. a 3-line shaped body at 240x240) the
        // ideal half-gap exceeds the small spacer; CLAMP to the spacer for a partial
        // centering rather than skipping it entirely, which would leave the body
        // hugging the headline (the visible hi/th symptom this clamp prevents).
        int32_t shift = below_gap / 2;
        if (shift > spacer_height) {
            shift = spacer_height;
        }
        if (shift > 0) {
            lv_obj_set_style_margin_top(body_label, shift, LV_PART_MAIN);
        }
    }

    // --- Navigation + load ---

    // bind_screen_navigation auto-detects any remaining body overflow (a long
    // warning that even a full reclaim can't fit) and enables scroll-then-buttons
    // joystick navigation. After a successful reclaim the content fits, so nothing
    // scrolls and the flow is the plain top-nav<->button one.
    bind_screen_navigation(
        cfg,
        screen,
        // NAV_INDEX_NONE: the single OK/ack button is active when the screen FITS, but
        // when a long warning overflows the user must scroll through it before the
        // button becomes selectable (read-first). It is NOT pre-focused under overflow.
        NAV_INDEX_NONE
    );

    load_screen_and_cleanup_previous(screen.screen);
}
