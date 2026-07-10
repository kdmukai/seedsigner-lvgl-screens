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

void large_icon_status_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

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

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, true);

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

    // Zero upper_body's flex row-gap so the icon->headline spacing is ONLY the
    // headline's COMPONENT_PADDING/2 top margin (matching Python's
    // next_y = icon_bottom + COMPONENT_PADDING/2); any inherited row-gap inflates it.
    lv_obj_set_style_pad_row(screen.upper_body, 0, LV_PART_MAIN);

    // Hero icon — colored, centered, sized from the active display profile.
    // upper_body's flex layout (column, cross-axis center) handles centering. The
    // icon needs no margin: the body's default pad_top is 0, so the icon renders at
    // the first available body pixel — just below the top_nav's bottom buffer — and
    // the glyph fits its label box exactly (box_h == line_height), so it renders
    // whole (no clip). This sits ~COMPONENT_PADDING/2 below Python's anchor
    // (top_nav.height - COMPONENT_PADDING/2); the trade keeps the top_nav buffer
    // visible while the body scrolls beneath it.
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

    // Optional headline — colored to match status. Single-line (never wraps),
    // matching Python's auto_line_break=False. A headline that FITS centers on the
    // full width and is byte-identical to before (LONG_DOT). A headline that
    // OVERFLOWS start-justifies (LTR=left; shaped RTL via the glyph-run draw) and
    // auto-scrolls within the text column to reveal the tail — a NEW capability
    // (Python truncates with an ellipsis; we scroll instead). Overflow is rare
    // (the fit test is against the full width) but long localized strings hit it.
    if (cfg.contains("status_headline") && cfg["status_headline"].is_string()) {
        std::string headline = cfg["status_headline"].get<std::string>();
        if (!headline.empty()) {
            lv_obj_t *headline_label = lv_label_create(screen.upper_body);
            lv_label_set_text(headline_label, headline.c_str());
            lv_obj_set_style_text_color(headline_label, lv_color_hex(defaults.color), LV_PART_MAIN);
            lv_obj_set_style_text_font(headline_label, &BODY_FONT, LV_PART_MAIN);

            // Measure the rendered width like top_nav()/A11 do: the label's STORED
            // (presentation-form) text, so the overflow test matches the painted
            // glyphs (see label_subset_text_width). Single-line headline; shaped
            // hi/th still mis-measure here (codepoint width) — a known low-impact gap.
            if (label_subset_text_width(headline_label, &BODY_FONT) > upper_body_content_width &&
                !seedsigner_locale_is_rtl()) {
                // LTR overflow: clamp the label to the text column (so it scrolls
                // within the same gutters as the body, never up to the screen edge
                // or under the warning border), then start-justify (LEFT) +
                // continuous marquee with the initial/per-wrap hold + true 40 px/sec
                // (shared with the top_nav title). The upper_body flex centers the
                // narrower label, yielding equal `text_inset` gutters. Shaped hi/th
                // ride Task 0's offset-aware glyph-run draw. RTL (ur) is gated out
                // here to match Task 0 / glyph_run_draw_cb (the offset, scroll
                // start-justify, and content-box clip are all LTR-only for now); an
                // overflowing ur headline keeps the legacy LONG_DOT path below, so ur
                // stays byte-identical and its RTL scroll lands with the ur RTL track.
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
            // Python's gap is CP/2 between the icon's visible bottom and the
            // headline's VISIBLE top. The label adds top leading above the caps
            // that PIL does not, so subtract it — otherwise the gap reads ~2x too
            // big and cascades down, jamming the bottom button against the edge.
            // Measure the label's STORED text, not the logical `headline`: with
            // LV_USE_ARABIC_PERSIAN_CHARS, lv_label_set_text rewrote Arabic/Persian
            // into presentation forms (the glyphs actually drawn, and the only ones
            // present in the subset font). Measuring the logical codepoints
            // under-counts the ink → an over-large leading → the fa headline pulls
            // UP into the hero icon (the A11 collision). en is unaffected (AP is a
            // no-op there, so the stored text == the logical text).
            int32_t hl_lead = text_top_leading(&BODY_FONT, lv_label_get_text(headline_label));
            lv_obj_set_style_margin_top(headline_label, COMPONENT_PADDING / 2 - hl_lead, LV_PART_MAIN);
        }
    }

    // Body text — wraps inside the upper_body width minus the
    // status-type-appropriate edge inset. Warning-class screens use
    // 2 * EDGE_PADDING so text never sits under the pulsing border.
    // Hoisted to function scope so the fits-case vertical centering below can
    // reference it after the full layout settles.
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

    // Optional pulsing border. Default per status_type (true for warning /
    // dire_warning / error), explicitly overridable from JSON.
    bool warning_edges = defaults.warning_edges_default;
    if (cfg.contains("warning_edges")) {
        if (!cfg["warning_edges"].is_boolean()) {
            throw std::runtime_error("warning_edges must be a boolean");
        }
        warning_edges = cfg["warning_edges"].get<bool>();
    }
    if (warning_edges) {
        add_warning_edges_overlay(screen.screen, defaults.color);
    }

    // A13/Item1: for shaped (glyph-run) locales the body's mask is drawn TALLER than
    // the lv_label widget box — the run lays out at the font's full line_height,
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
    // NOTE: this fix is specific to the status body, which sets a TIGHT line_space
    // (tight_line_space, below) so its codepoint box is SHORTER than the run. Plain
    // body labels (make_body_text_label, e.g. button_list_screen's intro text) keep
    // the screen's generous default BODY_LINE_SPACING, so their codepoint box already
    // meets/exceeds the run height and needs no such fix.
    if (seedsigner_locale_uses_glyph_runs()) {
        lv_obj_update_layout(screen.body);
        if (seedsigner_locale_is_rtl()) {
            apply_rtl_text_to_labels(screen.screen);
        }
        apply_glyph_runs_to_labels(screen.screen);
        if (body_label) {
            // run height is content-relative (painted from the content top), so size
            // the box to run_h PLUS the label's vertical padding to keep the content
            // area >= the painted run regardless of any theme padding.
            int32_t run_h = seedsigner_label_run_drawn_height(body_label);
            if (run_h >= 0) {
                int32_t pad_v = lv_obj_get_style_pad_top(body_label, LV_PART_MAIN) +
                                lv_obj_get_style_pad_bottom(body_label, LV_PART_MAIN);
                lv_obj_set_style_min_height(body_label, run_h + pad_v, LV_PART_MAIN);
            }
        }
    }

    // Reclaim-only-as-needed: if the content overflows by no more than the top_nav's
    // bottom buffer (tn_gap), pull the whole body up by exactly that overflow so it
    // FITS without scrolling — the icon/headline/text rise a few px into the buffer
    // region while the bottom button and its padding stay put. The body keeps its
    // bottom edge, so only the top is reclaimed. Because we pull up by the exact
    // overflow, the result fits (scroll_bottom -> 0) and never scrolls, so scrolled
    // content can never collide with the nav. A larger overflow (> tn_gap can't be
    // hidden in the buffer) is left alone: the icon stays at the default position and
    // the screen scrolls cleanly under the full buffer (bind_screen_navigation then
    // enables scroll-then-buttons). The two cases are mutually exclusive.
    lv_obj_update_layout(screen.body);
    int32_t overflow = lv_obj_get_scroll_bottom(screen.body);
    int32_t tn_gap = (TOP_NAV_HEIGHT - TOP_NAV_BUTTON_SIZE) / 2;
    if (overflow > 0 && overflow <= tn_gap) {
        lv_obj_set_height(screen.body, lv_obj_get_height(screen.body) + overflow);
        lv_obj_align_to(screen.body, screen.top_nav, LV_ALIGN_OUT_BOTTOM_MID, 0, -overflow);
    }

    // Balanced wrap: when the body fits, even out the body text's lines by
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

    // Vertically center the body text in the gap between the headline and the
    // bottom button when the content FITS with slack to spare. By default the body
    // text sits directly under the headline (gap 0) and the whole flex-grow spacer
    // sits BELOW it, hard against the button — so the text reads as top-biased.
    // Moving the text down by HALF the below-gap puts equal space above and below
    // it; the spacer (which the shift comes out of) keeps the button pinned, so the
    // button and its bottom padding never move. Skipped when the screen scrolls or
    // only just fits (spacer smaller than the shift): the gate keeps it a no-op
    // there, so reclaimed/overflowing screens are unaffected.
    if (body_label && screen.button_list_count > 0 && screen.button_list_spacer) {
        lv_obj_update_layout(screen.body);
        lv_area_t text_area, button_area;
        lv_obj_get_coords(body_label, &text_area);
        lv_obj_get_coords(screen.button_list[0], &button_area);

        // text bottom -> button top. For a shaped (glyph-run) body the run paints
        // a different height than the codepoint label box, so use the run's true
        // drawn bottom (content top + drawn block height); -1 => no run (en/subset
        // locales), in which case the label box bottom is already correct. (A13)
        int32_t text_bottom = text_area.y2;
        int32_t run_h = seedsigner_label_run_drawn_height(body_label);
        if (run_h >= 0) {
            lv_area_t cc;
            lv_obj_get_content_coords(body_label, &cc);
            text_bottom = cc.y1 + run_h;
        }
        int32_t below_gap = button_area.y1 - text_bottom;
        int32_t spacer_height = lv_obj_get_height(screen.button_list_spacer);

        // Shift the body down by half the below-gap to balance the space above and
        // below it. The shift comes out of the flex-grow spacer, so the button stays
        // pinned — but we can never take more than the spacer holds, or the button
        // would move. On TIGHT screens (e.g. a 3-line shaped body at 240x240) the
        // ideal half-gap exceeds the small spacer; CLAMP to the spacer for a partial
        // centering rather than skipping it entirely (which left the body hugging the
        // headline — the reported hi/th symptom). (A13)
        int32_t shift = below_gap / 2;
        if (shift > spacer_height) {
            shift = spacer_height;
        }
        if (shift > 0) {
            lv_obj_set_style_margin_top(body_label, shift, LV_PART_MAIN);
        }
    }

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
