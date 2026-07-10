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

// ===========================================================================
// psbt_overview_screen — animated transaction-flow diagram
// (LVGL port of Python PSBTOverviewScreen + TxExplorerAnimationThread)
// ===========================================================================
//
// The "Review Transaction" screen: a BtcAmount headline over a pictogram that fans
// every input, through a shared center bar, out to every output (recipients, self-
// transfers, change, OP_RETURN, and the miner fee), with an orange pulse continuously
// chasing along the curves to signal "these funds flow this way".
//
// Faithful port of the Python screen, with two deliberate substitutions the user
// approved: the diagram is drawn with native LVGL anti-aliased lines on an lv_canvas
// (Python supersampled a PIL image 4x); the curve MATH is a verbatim port of Python's
// custom quadratic Bezier (components.calc_bezier_curve), so the S-curve geometry is
// identical. The pulse animation is a direct port of TxExplorerAnimationThread: a
// segment-stepping orange head trailed by a gray eraser, driven by an lv_timer at the
// same 20 ms cadence, repainting only the changed segments onto the canvas each frame.
//
// cfg:
//   top_nav            : standard top-nav object (title defaults to "Review Transaction").
//   btc_amount         : {primary, secondary?, unit, network|icon_color} headline (see
//                        btc_amount_from_cfg / components.btc_amount). Optional.
//   num_inputs         : int >= 1.
//   destination_addresses : array of recipient address strings (may be empty).
//   num_self_transfer_outputs, num_change_outputs : ints (default 0).
//   has_op_return      : bool (default false).
//   button             : bottom-button label (default "Review details").
//   labels             : optional { input_n, one_input, recipient_1, recipient_n,
//                        ellipsis_series, ellipsis_trunc, fee, op_return, change,
//                        self_transfer } translated templates ({} = number slot).
//                        English fallbacks when omitted, so the desktop tool works as-is
//                        and per-locale scenario JSON supplies translations.

// ---- Bezier helpers (port of components.linear_interp / calc_bezier_curve). The math
// runs in FLOATING POINT and is snapped to the pixel grid only at the end (psbt_snap),
// because integer lerps floor asymmetrically: a branch and its mirror about the
// centerline would round to different rows and the fork would look lopsided.
struct psbt_fpt { float x, y; };

// PSRAM allocator for the transaction-flow geometry built below. The diagram allocates many
// small std::vector<lv_point_precise_t> (curves + per-frame pulse buffers). On ESP32 the
// default allocator places small blocks in INTERNAL RAM; building/tearing the overview down
// across repeated visits fragments that heap until the camera's internal-only QR-task stack
// can't find a contiguous block (builder doc camera-qr-task-internal-ram-starvation). Routing
// this geometry to PSRAM keeps the churn off the internal heap. Off-device (no ESP_PLATFORM)
// it degrades to the normal global operator new/delete, so the shared code stays portable.
template <class T> struct psram_alloc {
    using value_type = T;
    psram_alloc() noexcept {}
    template <class U> psram_alloc(const psram_alloc<U> &) noexcept {}
    T *allocate(std::size_t n) {
#ifdef ESP_PLATFORM
        void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);  // fall back to internal
        return static_cast<T *>(p);
#else
        return static_cast<T *>(::operator new(n * sizeof(T)));
#endif
    }
    void deallocate(T *p, std::size_t) noexcept {
#ifdef ESP_PLATFORM
        heap_caps_free(p);
#else
        ::operator delete(p);
#endif
    }
};
template <class T, class U>
bool operator==(const psram_alloc<T> &, const psram_alloc<U> &) noexcept { return true; }
template <class T, class U>
bool operator!=(const psram_alloc<T> &, const psram_alloc<U> &) noexcept { return false; }
template <class T> using pvec = std::vector<T, psram_alloc<T>>;

static psbt_fpt psbt_lerp(psbt_fpt a, psbt_fpt b, float t) {
    return { (1.0f - t) * a.x + t * b.x, (1.0f - t) * a.y + t * b.y };
}

// Cubic Bezier p0..p3 over `segments` line segments (De Casteljau); segments+1 points.
static pvec<psbt_fpt> psbt_cubic(psbt_fpt p0, psbt_fpt p1, psbt_fpt p2, psbt_fpt p3, int segments) {
    pvec<psbt_fpt> pts;
    pts.push_back(p0);
    float step = 1.0f / (float)segments;
    for (int i = 1; i <= segments; ++i) {
        if (i == segments) { pts.push_back(p3); break; }
        float t = step * (float)i;
        psbt_fpt a = psbt_lerp(p0, p1, t), b = psbt_lerp(p1, p2, t), c = psbt_lerp(p2, p3, t);
        psbt_fpt d = psbt_lerp(a, b, t), e = psbt_lerp(b, c, t);
        pts.push_back(psbt_lerp(d, e, t));
    }
    return pts;
}

// Snap a float point to the pixel grid, rounding the y-offset ABOUT the centerline vcf
// (lroundf is odd-symmetric, so mirror-image branches land on symmetric rows).
static lv_point_t psbt_snap(psbt_fpt p, int32_t vc, float vcf) {
    lv_point_t q;
    q.x = (int32_t)lroundf(p.x);
    q.y = vc + (int32_t)lroundf(p.y - vcf);
    return q;
}
static pvec<lv_point_t> psbt_snap_curve(const pvec<psbt_fpt> &f, int32_t vc, float vcf) {
    pvec<lv_point_t> out;
    out.reserve(f.size());
    for (const auto &p : f) out.push_back(psbt_snap(p, vc, vcf));
    return out;
}

// Horizontal-tangent hold at the label end (`_TIP`) and at the convergence hub (`_HUB`),
// as fractions of the branch's horizontal span. Smaller `_HUB` lets the branch start
// bending toward its row sooner (gentler, separates from its neighbours earlier).
static const float PSBT_S_TIP = 0.9f;
static const float PSBT_S_HUB = 0.72f;

// A smooth S (sigmoid) branch from the label end `tip` to the convergence `hub`, HORIZONTAL
// at both ends (Python's flow-diagram look). Built as a single cubic so it never forces a
// vertical inflection the way the old two-quadratic did — that forced vertical was the
// "aggressive kink" on the short inner branches (fee / OP_RETURN) that opened the black
// negative-space gap. Returned tip-first.
static pvec<psbt_fpt> psbt_branch(psbt_fpt tip, psbt_fpt hub, int segments) {
    float dx = hub.x - tip.x;   // signed span (hub right of tip for inputs, left for outputs)
    psbt_fpt c1 = { tip.x + dx * PSBT_S_TIP, tip.y };   // horizontal at the label
    psbt_fpt c2 = { hub.x - dx * PSBT_S_HUB, hub.y };   // horizontal at the hub
    return psbt_cubic(tip, c1, c2, hub, segments);
}

// Diagram palette / geometry (native px; Python worked 4x-supersampled, so its
// per-ssf values collapse to these once divided back down).
static const uint32_t PSBT_ASSOC_COLOR   = 0x666666;         // Python association_line_color "#666"
static const uint32_t PSBT_LABEL_COLOR   = 0xdddddd;         // Python chart_font_color "#ddd"
static const uint32_t PSBT_PULSE_COLOR   = (uint32_t)ACCENT_COLOR;  // Python GUIConstants.ACCENT_COLOR
// Stroke width scales with the display profile (3 px at the 240-height reference,
// ~6 px at 480-height) so the pictogram doesn't look thin on the larger panels.
static int32_t psbt_line_width() {
    int32_t w = 3 * active_profile().px_multiplier / 100;
    return w < 3 ? 3 : w;
}
static const int      PSBT_CURVE_STEPS   = 16;               // Python used 4 (hidden by its 4x supersample); we
                                                             // use a finer polyline so the native-AA curve reads smooth
// Pulse motion is WALL-CLOCK based (not per-frame), so the rate is identical whether
// lv_timer_handler runs at 30, 60, or 120 fps. The head crosses the whole
// input->center->output path in PSBT_PULSE_TRAVERSE_MS; the lit band trails it at
// PSBT_PULSE_BAND_FRAC of the path length; and the tick just sets the visual update
// cadence. A starved frame is clamped so the pulse slows rather than teleporting.
static const uint32_t PSBT_PULSE_TICK_MS     = 16;    // ~60 fps update cadence
static const float    PSBT_PULSE_TRAVERSE_MS = 1600;  // wall-clock for the head to cross the pictogram
static const float    PSBT_PULSE_BAND_FRAC   = 0.45f; // lit band length as a fraction of the path
static const float    PSBT_PULSE_GAP_FRAC    = 0.15f; // dark pause between cycles, as a fraction of the path
static const uint32_t PSBT_PULSE_MAX_STEP_MS = 100;   // clamp per-frame advance (starved-frame guard)

// Convert a snapped integer polyline to lv_line's precise-point type. lv_line stores the
// pointer WITHOUT copying, so the returned vector must outlive the widget (it lives in the
// per-screen anim ctx).
static pvec<lv_point_precise_t> psbt_to_precise(const pvec<lv_point_t> &c) {
    pvec<lv_point_precise_t> out;
    out.reserve(c.size());
    for (const auto &p : c) out.push_back({ (lv_value_precise_t)p.x, (lv_value_precise_t)p.y });
    return out;
}

// A polyline as a real lv_line widget (no canvas buffer) with round joins. `pts` must
// outlive the widget (LVGL stores the pointer, not a copy). pts=nullptr makes an empty line
// (used for pulse lines whose points are set per frame).
static lv_obj_t *psbt_make_line(lv_obj_t *parent, const lv_point_precise_t *pts, uint32_t n, uint32_t color) {
    lv_obj_t *ln = lv_line_create(parent);
    lv_obj_set_pos(ln, 0, 0);
    lv_obj_set_style_line_color(ln, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_line_width(ln, psbt_line_width(), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(ln, true, LV_PART_MAIN);
    lv_obj_remove_flag(ln, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    if (pts && n >= 2) lv_line_set_points(ln, pts, n);
    return ln;
}

// Per-screen animation state. The diagram is real lv_line widgets (children of `container`,
// auto-freed with the screen) — NOT a canvas buffer — so nothing large lands in the tiny
// LVGL internal pool and there is no per-frame full-buffer memcpy. Static gray paths are
// drawn once; the pulse is a few ORANGE lines whose points are set to the currently lit band
// each frame (LVGL recomposites over the true background, so there is no AA fringe to erase).
struct psbt_anim_ctx_t {
    lv_obj_t   *container;      // holds every diagram line (validity-guards the timer)
    lv_timer_t *timer;
    // Geometry as precise points; the static lv_lines reference these, so they must persist.
    pvec<pvec<lv_point_precise_t>> in_pts;    // input curves (tip -> hub)
    pvec<pvec<lv_point_precise_t>> out_pts;   // output curves (hub -> tip)
    pvec<lv_point_precise_t>       ctr_pts;   // center bar
    // Orange pulse lines + their per-frame point buffers (one per input/output curve + bar).
    pvec<lv_obj_t *>               pin;
    pvec<pvec<lv_point_precise_t>> pin_buf;
    lv_obj_t                      *pctr;
    pvec<lv_point_precise_t>       pctr_buf;
    pvec<lv_obj_t *>               pout;
    pvec<pvec<lv_point_precise_t>> pout_buf;
    int         in_segs, ctr_segs, out_segs, total_segs;
    int         band_segs, reset_at;
    float       head;
    uint32_t    last_tick;
};

// Point an orange pulse line at the sub-polyline spanning segments [segLo..segHi] of `src`
// (segment s connects points s and s+1), or hide it when that range is empty.
static void psbt_set_pulse(lv_obj_t *line, pvec<lv_point_precise_t> &buf,
                           const pvec<lv_point_precise_t> &src, int segLo, int segHi) {
    if (!line) return;
    if (segHi < segLo || segLo < 0 || segHi + 1 >= (int)src.size()) {
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    buf.assign(src.begin() + segLo, src.begin() + segHi + 2);   // points [segLo .. segHi+1]
    lv_line_set_points(line, buf.data(), (uint32_t)buf.size());
    lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
}

// Wall-clock pulse (rate is frame-rate independent, matching the loading spinner's
// integrator). The lit band [head-band, head] walks the input -> center -> output path;
// during the fan phases every input/output curve lights its band segment at once (all funds
// flow together). Only the orange lines move each frame — the gray paths and labels are
// untouched, so LVGL repaints just the pulse's region (no full-buffer work).
static void psbt_pulse_timer_cb(lv_timer_t *timer) {
    psbt_anim_ctx_t *c = (psbt_anim_ctx_t *)lv_timer_get_user_data(timer);
    if (!c || !lv_obj_is_valid(c->container)) return;

    uint32_t now = lv_tick_get();
    uint32_t dt  = now - c->last_tick;
    c->last_tick = now;
    if (dt > PSBT_PULSE_MAX_STEP_MS) dt = PSBT_PULSE_MAX_STEP_MS;   // starved-frame clamp

    float segs_per_ms = (float)c->total_segs / PSBT_PULSE_TRAVERSE_MS;
    c->head += (float)dt * segs_per_ms;
    if (c->head >= (float)c->reset_at) c->head -= (float)c->reset_at;

    int hi = (int)c->head;
    int lo = (int)(c->head - (float)c->band_segs);
    if (hi > c->total_segs - 1) hi = c->total_segs - 1;
    if (lo < 0) lo = 0;

    // Input phase: global segments [0, in_segs).
    int is = lo > 0 ? lo : 0;
    int ie = hi < c->in_segs - 1 ? hi : c->in_segs - 1;
    for (size_t k = 0; k < c->pin.size(); ++k)
        psbt_set_pulse(c->pin[k], c->pin_buf[k], c->in_pts[k], is, ie);

    // Center phase: global segments [in_segs, in_segs + ctr_segs).
    int cs = (lo > c->in_segs ? lo : c->in_segs) - c->in_segs;
    int ce = (hi < c->in_segs + c->ctr_segs - 1 ? hi : c->in_segs + c->ctr_segs - 1) - c->in_segs;
    psbt_set_pulse(c->pctr, c->pctr_buf, c->ctr_pts, cs, ce);

    // Output phase: global segments [in_segs + ctr_segs, total_segs).
    int base = c->in_segs + c->ctr_segs;
    int os = (lo > base ? lo : base) - base;
    int oe = (hi < c->total_segs - 1 ? hi : c->total_segs - 1) - base;
    for (size_t k = 0; k < c->pout.size(); ++k)
        psbt_set_pulse(c->pout[k], c->pout_buf[k], c->out_pts[k], os, oe);
}

static void psbt_cleanup_cb(lv_event_t *e) {
    psbt_anim_ctx_t *c = (psbt_anim_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->timer) lv_timer_delete(c->timer);   // stop the pulse before the lines are freed
    delete c;                                   // the lv_line children go with the screen
}


void psbt_overview_screen(void *ctx_json) {
    const char *json_str = (const char *)ctx_json;
    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // ---- Read the transaction structure (mirrors the Python dataclass fields).
    int  num_inputs        = cfg.value("num_inputs", 1);
    if (num_inputs < 1) num_inputs = 1;
    int  num_self_transfer = cfg.value("num_self_transfer_outputs", 0);
    int  num_change        = cfg.value("num_change_outputs", 0);
    bool has_op_return     = cfg.value("has_op_return", false);
    std::vector<std::string> dest_addrs;
    if (cfg.contains("destination_addresses") && cfg["destination_addresses"].is_array()) {
        for (const auto &a : cfg["destination_addresses"]) {
            if (a.is_string()) dest_addrs.push_back(a.get<std::string>());
        }
    }

    // ---- Translated labels: cfg["labels"] with English fallbacks (host owns i18n).
    const json labels = (cfg.contains("labels") && cfg["labels"].is_object()) ? cfg["labels"] : json::object();
    auto L = [&](const char *key, const char *dflt) -> std::string {
        if (labels.contains(key) && labels[key].is_string()) return labels[key].get<std::string>();
        return std::string(dflt);
    };
    auto fmt_n = [](const std::string &tmpl, long n) -> std::string {
        std::string out = tmpl;
        size_t pos = out.find("{}");
        if (pos != std::string::npos) out.replace(pos, 2, std::to_string(n));
        return out;
    };
    const std::string input_n         = L("input_n", "input {}");
    const std::string recipient_n     = L("recipient_n", "recipient {}");
    const std::string recipient_1     = L("recipient_1", "recipient 1");
    const std::string recipient_sing  = L("recipient_singular", "recipient");
    const std::string ellipsis_series = L("ellipsis_series", "[ ... ]");
    const std::string self_transfer   = L("self_transfer", "self-transfer");
    const std::string fee_label       = L("fee", "fee");
    const std::string op_return_label = L("op_return", "OP_RETURN");
    const std::string change_label    = L("change", "change");

    // ---- Scaffold: a bottom-pinned single-button list (Python is_bottom_list).
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = L("title", "Review Transaction");
    if (!cfg.contains("button_list")) {
        cfg["button_list"] = json::array({ cfg.value("button", std::string("Review details")) });
    }
    cfg["is_bottom_list"] = true;

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // ---- BtcAmount headline in the upper body (Python's callout above the chart).
    lv_obj_t *headline = nullptr;
    if (cfg.contains("btc_amount") && cfg["btc_amount"].is_object() && screen.upper_body) {
        // Hug the top nav: the body already sits below the nav's own bottom buffer, so a
        // small pad here is enough. Keeping the callout high frees vertical room for the
        // pictogram (which is otherwise cramped on the many-row scenarios).
        lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING / 4, LV_PART_MAIN);
        headline = btc_amount_from_cfg(screen.upper_body, cfg["btc_amount"]);
    }

    // ---- Measure the gap between the headline and the pinned button — that band is
    // the chart (Python: chart_y = below the callout; chart_height = up to the button).
    lv_obj_update_layout(screen.screen);

    const int32_t W    = lv_display_get_horizontal_resolution(NULL);
    const int32_t comp = COMPONENT_PADDING;
    const int32_t edge = EDGE_PADDING;

    int32_t chart_top;
    if (headline) {
        lv_area_t a; lv_obj_get_coords(headline, &a);
        chart_top = a.y2 + comp / 4;
    } else {
        chart_top = TOP_NAV_HEIGHT + comp;
    }
    // Band bottom = the shared scaffold query minus the chart's own `comp` inset
    // (both branches of the inlined form subtracted comp, so hoisting it out of
    // the fallback/override pair is value-identical).
    int32_t chart_bottom = bottom_button_top_y(screen) - comp;
    const int32_t chart_h = chart_bottom - chart_top;

    const int32_t text_h = lv_font_get_line_height(&BODY_FONT);

    // Only build the diagram if there is room for at least one row.
    if (chart_h >= text_h) {
        auto text_w = [&](const std::string &s) -> int32_t {
            lv_point_t sz;
            lv_text_get_size(&sz, s.c_str(), &BODY_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            return sz.x;
        };

        // ---- Input column labels (Python inputs_column).
        std::vector<std::string> inputs_column;
        if (num_inputs == 1) {
            inputs_column.push_back(L("one_input", "1 input"));
        } else if (num_inputs > 5) {
            inputs_column.push_back(fmt_n(input_n, 1));
            inputs_column.push_back(fmt_n(input_n, 2));
            inputs_column.push_back(ellipsis_series);
            inputs_column.push_back(fmt_n(input_n, num_inputs - 1));
            inputs_column.push_back(fmt_n(input_n, num_inputs));
        } else {
            for (int i = 0; i < num_inputs; ++i) inputs_column.push_back(fmt_n(input_n, i + 1));
        }
        int32_t max_inputs_text_width = 0;
        for (const auto &s : inputs_column) max_inputs_text_width = std::max(max_inputs_text_width, text_w(s));

        // ---- Aesthetic geometry knobs (all scale with the display via `comp`). The
        // pictogram is kept PERFECTLY CENTERED on the screen midline: each half mirrors
        // the other (center bar straddles W/2, equal-width curve columns), and the room
        // per half is capped by the WIDER text column — so a long address on one side
        // pulls the short side inward (blank space on the short side) instead of shoving
        // the whole diagram off-center.
        const int32_t text_gap = comp / 2;      // balanced buffer between text and curve, both sides
        const int32_t cw_min   = 4 * comp;      // curve column min (Python's fixed width) ...
        const int32_t cw_max   = 6 * comp;      // ... up to ~50% wider when there's room (graceful curves)
        const int32_t cb_min   = 3 * comp / 2;  // center bar floor — kept short so the labels breathe
        const int32_t cb_max   = 5 * comp / 2;  // ... grows only a little past the floor
        const int32_t cb_floor = comp;          // hard floor when a tight screen can't center (below cb_min)
        const int32_t center_x = W / 2;

        // ---- Destination column. The pictogram conveys transaction STRUCTURE, not the
        // actual addresses (those are verified on the later review screens), so recipients
        // are shown generically: "recipient" for one, "recipient 1..N" for a few, and
        // collapsed to "recipient 1 / [ ... ] / recipient N" when there are many. (A
        // deliberate departure from the Python original, which truncated the addresses.)
        std::vector<std::string> destination_column;
        {
            const int n_recip  = (int)dest_addrs.size();
            const int total_rl = n_recip + num_self_transfer;   // recipient-like outputs
            if (total_rl <= 3) {
                if (n_recip == 1) {
                    destination_column.push_back(recipient_sing);
                } else {
                    for (int i = 0; i < n_recip; ++i) destination_column.push_back(fmt_n(recipient_n, i + 1));
                }
                for (int i = 0; i < num_self_transfer; ++i) destination_column.push_back(self_transfer);
            } else {
                destination_column.push_back(recipient_1);
                destination_column.push_back(ellipsis_series);
                destination_column.push_back(fmt_n(recipient_n, total_rl));
            }
            destination_column.push_back(fee_label);
            if (has_op_return) destination_column.push_back(op_return_label);
            for (int i = 0; i < num_change; ++i) destination_column.push_back(change_label);
        }
        int32_t dest_text_width = 0;
        for (const auto &s : destination_column) dest_text_width = std::max(dest_text_width, text_w(s));

        // ---- Solve the horizontal layout. Two regimes:
        //   Centered (the common case, and preferred): the pictogram is symmetric about
        //   the screen midline; the per-half room is capped by the WIDER text column, so
        //   the short side pulls in and leaves blank margin. Curves grow first (graceful),
        //   then the center bar, then blank.
        //   Fit fallback (tight screen / wide labels, e.g. many recipients at 240): if
        //   even the minimum centered core won't fit, GIVE UP centering to keep the labels
        //   on screen — anchor both text columns at the edges, shrink the center bar to its
        //   hard floor, and split the leftover between the two (still equal-width) curves.
        const int32_t tw_max = std::max(max_inputs_text_width, dest_text_width);
        const int32_t room   = center_x - edge - text_gap - tw_max;   // per-half budget for (cw + cb/2)

        int32_t center_bar_x, center_bar_width, dest_conj_x;
        int32_t input_start_x, inputs_text_right, output_end_x, destination_col_x;

        if (room >= cw_min + cb_min / 2) {
            // Centered regime.
            int32_t cw = room - cb_min / 2;
            if (cw > cw_max) cw = cw_max;
            if (cw < cw_min) cw = cw_min;
            int32_t cb = 2 * (room - cw);
            if (cb < cb_min) cb = cb_min;
            if (cb > cb_max) cb = cb_max;

            center_bar_x      = center_x - cb / 2;
            center_bar_width  = cb;
            dest_conj_x       = center_x + cb / 2;
            input_start_x     = center_bar_x - cw;
            inputs_text_right = input_start_x - text_gap;
            output_end_x      = dest_conj_x + cw;
            destination_col_x = output_end_x + text_gap;
        } else {
            // Fit fallback: labels at the edges, symmetric curves, minimal center bar.
            inputs_text_right = edge + max_inputs_text_width;   // input text left-justified at the edge
            destination_col_x = W - edge - dest_text_width;     // output text right-justified at the edge
            input_start_x     = inputs_text_right + text_gap;
            output_end_x      = destination_col_x - text_gap;
            int32_t avail = output_end_x - input_start_x;       // = cw + cb + cw
            int32_t cb = cb_floor;
            if (cb > avail - 2) cb = avail - 2;                 // keep room for a sliver of curve
            if (cb < 1) cb = 1;
            int32_t cw = (avail - cb) / 2;
            if (cw < 1) cw = 1;
            center_bar_x      = input_start_x + cw;
            center_bar_width  = cb;
            dest_conj_x       = center_bar_x + cb;
            output_end_x      = dest_conj_x + cw;               // re-derive so the two curves match exactly
        }

        // ---- Vertical placement: rows centered SYMMETRICALLY about the mid-line, so an
        // ODD count puts its middle row exactly on the centerline (a straight branch, no
        // bend). Pitch matches Python's even distribution (equal top/inter/bottom gaps);
        // deriving each row from the center avoids the integer-truncation drift that used
        // to nudge the whole block upward.
        const int32_t vc  = chart_h / 2;
        const float   vcf = (float)chart_h / 2.0f;
        const int n_in    = (int)inputs_column.size();
        const int n_dest  = (int)destination_column.size();

        auto row_pitch = [&](int n) -> float {
            if (n <= 1) return 0.0f;
            float natural = (float)(chart_h - n * text_h) / (float)(n + 1) + (float)text_h;
            // Never let the row block overflow the chart band (block = (n-1)*pitch + text_h
            // <= chart_h): when there are more rows than fit at the natural pitch, compress
            // so the top/bottom rows stay inside the canvas instead of poking the button.
            float max_fit = (float)(chart_h - text_h) / (float)(n - 1);
            return natural < max_fit ? natural : max_fit;
        };
        // Float row center (snapped later about vcf), symmetric about the mid-line.
        auto row_center_yf = [&](int n, int k, float pitch) -> float {
            return vcf + ((float)k - (float)(n - 1) / 2.0f) * pitch;
        };
        const float in_pitch  = row_pitch(n_in);
        const float out_pitch = row_pitch(n_dest);

        // ---- The diagram is drawn with real lv_line widgets (no canvas buffer): a full-
        // screen RGB565 canvas can't fit the ESP32 LVGL internal pool, and a canvas would
        // also cost a full-buffer memcpy every animation frame. A transparent container at
        // the chart origin lets every child use the same canvas-local coordinates the
        // geometry is computed in. Static gray paths are drawn once; the orange pulse (below)
        // recolors a moving band via a few extra lines that LVGL recomposites fringe-free.
        (void)center_bar_width;   // was only the static center-bar span; ctr_pts carries it now
        lv_obj_t *chart = lv_obj_create(screen.screen);
        lv_obj_remove_style_all(chart);
        lv_obj_set_pos(chart, 0, chart_top);
        lv_obj_set_size(chart, W, chart_h);
        lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);

        psbt_anim_ctx_t *actx = new psbt_anim_ctx_t();
        actx->container = chart;
        actx->timer     = nullptr;
        actx->pctr      = nullptr;

        // Labels are collected and created LAST so they compose on top of the lines (the
        // canvas version drew the diagram into a bitmap under the label widgets).
        struct label_spec_t { std::string text; int32_t x, y; };
        std::vector<label_spec_t> labels;

        // ---- Input rows: label + a two-arc S-curve to the center bar (built in float,
        // snapped symmetric about vcf, stored as precise points for the static lv_line).
        for (int k = 0; k < n_in; ++k) {
            const std::string &s = inputs_column[k];
            int32_t tw     = text_w(s);
            float   row_cy = row_center_yf(n_in, k, in_pitch);
            labels.push_back({ s, inputs_text_right - tw, (int32_t)lroundf(row_cy) - text_h / 2 });

            psbt_fpt start_pt = { (float)input_start_x, row_cy };
            psbt_fpt conj     = { (float)center_bar_x, vcf };   // input hub = left end of the center bar
            pvec<psbt_fpt> fcurve;
            if (num_inputs == 1) fcurve = { start_pt, conj };   // one input: a straight segment
            else                 fcurve = psbt_branch(start_pt, conj, 2 * PSBT_CURVE_STEPS);
            actx->in_pts.push_back(psbt_to_precise(psbt_snap_curve(fcurve, vc, vcf)));
        }

        // ---- Output rows: mirror of the input side, stored hub -> tip (pulse travels out).
        for (int k = 0; k < n_dest; ++k) {
            const std::string &s = destination_column[k];
            float row_cy = row_center_yf(n_dest, k, out_pitch);
            labels.push_back({ s, destination_col_x, (int32_t)lroundf(row_cy) - text_h / 2 });

            psbt_fpt conj   = { (float)dest_conj_x, vcf };   // output hub = right end of the center bar
            psbt_fpt end_pt = { (float)output_end_x, row_cy };
            pvec<psbt_fpt> fcurve = psbt_branch(end_pt, conj, 2 * PSBT_CURVE_STEPS);
            std::reverse(fcurve.begin(), fcurve.end());
            actx->out_pts.push_back(psbt_to_precise(psbt_snap_curve(fcurve, vc, vcf)));
        }

        // ---- Center bar, segmented so the pulse can step across it (straight, at vcf).
        psbt_fpt cbf_start = { (float)center_bar_x, vcf };
        psbt_fpt cbf_end   = { (float)dest_conj_x, vcf };
        actx->ctr_pts = psbt_to_precise(psbt_snap_curve(
            { cbf_start,
              psbt_lerp(cbf_start, cbf_end, 0.25f),
              psbt_lerp(cbf_start, cbf_end, 0.50f),
              psbt_lerp(cbf_start, cbf_end, 0.75f),
              cbf_end }, vc, vcf));

        // ---- Static gray paths (drawn once). They reference actx's point vectors, which
        // outlive the widgets (actx is freed only when the screen — and these children — is).
        for (auto &c : actx->in_pts)  psbt_make_line(chart, c.data(), (uint32_t)c.size(), PSBT_ASSOC_COLOR);
        psbt_make_line(chart, actx->ctr_pts.data(), (uint32_t)actx->ctr_pts.size(), PSBT_ASSOC_COLOR);
        for (auto &c : actx->out_pts) psbt_make_line(chart, c.data(), (uint32_t)c.size(), PSBT_ASSOC_COLOR);

        // ---- Segment counts + pulse cadence (wall-clock; identical to the old timer).
        actx->in_segs    = (int)actx->in_pts[0].size() - 1;
        actx->ctr_segs   = (int)actx->ctr_pts.size() - 1;
        actx->out_segs   = (int)actx->out_pts[0].size() - 1;
        actx->total_segs = actx->in_segs + actx->ctr_segs + actx->out_segs;
        actx->band_segs  = std::max(1, (int)lroundf(PSBT_PULSE_BAND_FRAC * (float)actx->total_segs));
        actx->reset_at   = actx->total_segs + actx->band_segs
                           + std::max(1, (int)lroundf(PSBT_PULSE_GAP_FRAC * (float)actx->total_segs));
        actx->head       = 0.0f;
        actx->last_tick  = lv_tick_get();

        // ---- Orange pulse lines (created after the gray paths so they compose on top). One
        // per input/output curve + the center bar; each starts hidden and is pointed at the
        // lit band each frame. Buffers are pre-sized so lv_line_set_points never reallocates.
        // "animate": false renders the static diagram only.
        if (cfg.value("animate", true)) {
            for (int k = 0; k < n_in; ++k) {
                lv_obj_t *ln = psbt_make_line(chart, nullptr, 0, PSBT_PULSE_COLOR);
                lv_obj_add_flag(ln, LV_OBJ_FLAG_HIDDEN);
                actx->pin.push_back(ln);
                actx->pin_buf.emplace_back();
                actx->pin_buf.back().reserve(actx->band_segs + 2);
            }
            actx->pctr = psbt_make_line(chart, nullptr, 0, PSBT_PULSE_COLOR);
            lv_obj_add_flag(actx->pctr, LV_OBJ_FLAG_HIDDEN);
            actx->pctr_buf.reserve(actx->band_segs + 2);
            for (int k = 0; k < n_dest; ++k) {
                lv_obj_t *ln = psbt_make_line(chart, nullptr, 0, PSBT_PULSE_COLOR);
                lv_obj_add_flag(ln, LV_OBJ_FLAG_HIDDEN);
                actx->pout.push_back(ln);
                actx->pout_buf.emplace_back();
                actx->pout_buf.back().reserve(actx->band_segs + 2);
            }
            actx->timer = lv_timer_create(psbt_pulse_timer_cb, PSBT_PULSE_TICK_MS, actx);
        }

        // ---- Labels last, on top of every line.
        for (const auto &ls : labels) {
            lv_obj_t *lbl = lv_label_create(chart);
            lv_obj_set_style_pad_all(lbl, 0, LV_PART_MAIN);
            lv_obj_set_style_text_font(lbl, &BODY_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(lbl, lv_color_hex(PSBT_LABEL_COLOR), LV_PART_MAIN);
            lv_label_set_text(lbl, ls.text.c_str());
            lv_obj_set_pos(lbl, ls.x, ls.y);
        }

        lv_obj_add_event_cb(screen.screen, psbt_cleanup_cb, LV_EVENT_DELETE, actx);
    }

    bind_screen_navigation(cfg, screen, 0);

    load_screen_and_cleanup_previous(screen.screen);
}
