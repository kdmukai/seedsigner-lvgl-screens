// psbt_overview_screen
//
// Python provenance: PSBTOverviewScreen (psbt_screens.py), including its nested
// TxExplorerAnimationThread; the branch-curve math descends from
// calc_bezier_curve / linear_interp (components.py).
//
// The "Review Transaction" screen: a BtcAmount headline over an animated
// transaction-flow pictogram that fans every input, through a shared center
// bar, out to every output (recipients, self-transfers, change, OP_RETURN, and
// the miner fee), with an orange pulse continuously chasing along the curves to
// signal "these funds flow this way". Returns through the standard navigation
// callback: the bottom-pinned action button (host-localized, Python's "Review
// details") or the top-nav back button.
//
// Lifecycle Tier 2 (stateful): one heap animation ctx (C++ new/delete — it owns
// std::vector members) + one LV_EVENT_DELETE cleanup callback on the screen
// root, which deletes the pulse timer before freeing the ctx. When the free
// band is too short for even one text row, no chart/ctx/timer/cleanup is
// created at all and the screen degrades to headline + button (a clean,
// stateless skip).
//
// Layout: standard bottom-pinned scaffold (is_bottom_list forced true). The
// headline hugs the top-nav; the chart claims the measured band between the
// headline and the button. The pictogram stays symmetric about the screen
// midline whenever the two text columns fit (centered regime); otherwise the
// labels anchor at the screen edges and the curves shrink (fit fallback) — see
// the horizontal solve pass in the entry point.
//
// Documented deviations from Python (user-approved):
//   - The diagram is real lv_line widgets, NOT a canvas: a full-screen RGB565
//     canvas cannot fit the ESP32 LVGL internal pool, and a canvas would cost a
//     full-buffer memcpy every animation frame. LVGL recomposites the lines
//     fringe-free, so no eraser pass is needed (Python's animation thread drew
//     an orange head trailed by a gray eraser onto a 4x-supersampled PIL image).
//   - Each branch is ONE cubic Bezier, horizontal at both ends, instead of
//     Python's two chained quadratics — the two-quadratic form forces a vertical
//     inflection mid-branch, an "aggressive kink" on the short inner branches
//     (fee / OP_RETURN) that opened a black negative-space gap.
//   - 16-segment polylines instead of Python's 4: Python hid the coarseness
//     behind its 4x supersample; the native-AA curves need finer steps to read
//     smooth.
//   - The pulse is WALL-CLOCK driven (16 ms visual tick, starved-frame clamp)
//     rather than Python's per-frame 20 ms thread stepping, so the traverse
//     rate is identical at any frame rate.
//   - Recipients render as generic "recipient 1..N" labels (collapsed with an
//     ellipsis row when there are many) instead of Python's truncated
//     addresses: the pictogram conveys transaction STRUCTURE; the real
//     addresses are verified on the later review screens.
//
// Content-policy note (docs/screen-conformance-spec.md §5): the chart-label
// templates in cfg["labels"] RETAIN built-in English fallbacks — no scenario
// supplies `labels` today and the desktop tools rely on the fallbacks — so they
// are flagged, not converted; per-locale hosts pass translated templates.
//
// cfg:
//   top_nav.title             (string, required)     localized screen title (read by
//            the scaffold; enforced here via require_top_nav_title; Python:
//            _("Review Transaction")).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   top_nav.icon / top_nav.icon_color (optional)     title icon glyph + color (read
//            by the scaffold).
//   button                    (string, required unless button_list is given)
//            localized label for the single bottom action button (Python:
//            ButtonOption "Review details"); wrapped into a one-entry
//            button_list for the scaffold.
//   button_list               (array, optional)      full button list passed through
//            verbatim to the scaffold; when present, `button` is ignored.
//   is_bottom_list            forced true (Python: is_bottom_list = True); a
//            host-supplied value is ignored.
//   btc_amount                (object, optional)     {primary, secondary?, unit,
//            network|icon_color} headline spec (see btc_amount_from_cfg); absent =
//            no headline, and the chart band starts below the top-nav instead.
//   num_inputs                (int, default 1, clamped >= 1)  transaction input count.
//   destination_addresses     (array of strings, default empty)  recipient
//            addresses — only the COUNT is rendered (see the recipient-label
//            deviation above); non-string entries are skipped.
//   num_self_transfer_outputs (int, default 0)       self-transfer output count.
//   num_change_outputs        (int, default 0)       change output count.
//   has_op_return             (bool, default false)  adds the OP_RETURN output row.
//   animate                   (bool, default true)   false renders the static
//            diagram only (no pulse lines, no timer) — still-screenshot mode.
//   labels                    (object, optional)     translated chart-label templates
//            ({} = the number slot): input_n, one_input, recipient_n, recipient_1,
//            recipient_singular, ellipsis_series, self_transfer, fee, op_return,
//            change. English fallbacks when omitted (see the content-policy note).
//   initial_selected_index    (int, default 0)       initial focused button (read by
//            the navigation layer).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bottom_button_top_y / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // psbt_overview_screen decl, screen_scaffold_t fields
#include "gui_constants.h"    // COMPONENT_PADDING, EDGE_PADDING, TOP_NAV_HEIGHT, BODY_FONT, ACCENT_COLOR, active_profile
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title, btc_amount_from_cfg

#include "lvgl.h"             // lv_line / lv_label / lv_timer, per-object style setters, layout/coords queries, lv_text_get_size

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <algorithm>          // std::max (column widths, pulse band sizing), std::reverse (output curves stored hub -> tip)
#include <cmath>              // lroundf (pixel-grid snapping, pulse band sizing)
#include <cstddef>            // size_t ("{}"-slot find, pulse-line loops)
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string, std::to_string
#include <vector>             // std::vector (label columns, addresses)

// ESP32 only: heap_caps_malloc / heap_caps_free back the PSRAM allocator below.
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

using json = nlohmann::json;


// ---------------------------------------------------------------------------
// Diagram palette / geometry constants (native px; Python worked
// 4x-supersampled, so its per-ssf values collapse to these once divided back
// down).

// Horizontal-tangent hold at the label end (`_TIP`) and at the convergence hub
// (`_HUB`), as fractions of the branch's horizontal span. Smaller `_HUB` lets
// the branch start bending toward its row sooner (gentler, separates from its
// neighbours earlier).
static const float PSBT_OVERVIEW_S_TIP = 0.9f;
static const float PSBT_OVERVIEW_S_HUB = 0.72f;

static const uint32_t PSBT_OVERVIEW_ASSOCIATION_COLOR = 0x666666;               // Python association_line_color "#666"
static const uint32_t PSBT_OVERVIEW_LABEL_COLOR       = 0xdddddd;               // Python chart_font_color "#ddd"
static const uint32_t PSBT_OVERVIEW_PULSE_COLOR       = (uint32_t)ACCENT_COLOR; // Python GUIConstants.ACCENT_COLOR

static const int PSBT_OVERVIEW_CURVE_STEPS = 16;   // Python used 4 (hidden by its 4x supersample); we use a
                                                   // finer polyline so the native-AA curve reads smooth

// Pulse motion is WALL-CLOCK based (not per-frame), so the rate is identical
// whether lv_timer_handler runs at 30, 60, or 120 fps. The head crosses the
// whole input->center->output path in PSBT_OVERVIEW_PULSE_TRAVERSE_MS; the lit
// band trails it at PSBT_OVERVIEW_PULSE_BAND_FRACTION of the path length; and
// the tick just sets the visual update cadence. A starved frame is clamped so
// the pulse slows rather than teleporting.
static const uint32_t PSBT_OVERVIEW_PULSE_TICK_MS       = 16;    // ~60 fps update cadence
static const float    PSBT_OVERVIEW_PULSE_TRAVERSE_MS   = 1600;  // wall-clock for the head to cross the pictogram
static const float    PSBT_OVERVIEW_PULSE_BAND_FRACTION = 0.45f; // lit band length as a fraction of the path
static const float    PSBT_OVERVIEW_PULSE_GAP_FRACTION  = 0.15f; // dark pause between cycles, as a fraction of the path
static const uint32_t PSBT_OVERVIEW_PULSE_MAX_STEP_MS   = 100;   // clamp per-frame advance (starved-frame guard)


namespace {

// ---------------------------------------------------------------------------
// Bezier helpers (port of Python components.linear_interp / calc_bezier_curve,
// with the single-cubic branch substitution documented in the banner). The math
// runs in FLOATING POINT and is snapped to the pixel grid only at the end
// (psbt_overview_snap_point), because integer lerps floor asymmetrically: a
// branch and its mirror about the centerline would round to different rows and
// the fork would look lopsided.
struct psbt_overview_float_point_t { float x, y; };

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

// Linear interpolation between two float points (Python components.linear_interp).
psbt_overview_float_point_t psbt_overview_linear_interp(psbt_overview_float_point_t point_a,
                                                        psbt_overview_float_point_t point_b,
                                                        float t) {
    return { (1.0f - t) * point_a.x + t * point_b.x, (1.0f - t) * point_a.y + t * point_b.y };
}

// Cubic Bezier p0..p3 over `segments` line segments (De Casteljau); returns
// segments+1 points. Intermediate names follow the De Casteljau levels:
// p01 = lerp(p0, p1, t); p012 = lerp(p01, p12, t); the final point is
// lerp(p012, p123, t).
pvec<psbt_overview_float_point_t> psbt_overview_cubic_bezier(psbt_overview_float_point_t p0,
                                                             psbt_overview_float_point_t p1,
                                                             psbt_overview_float_point_t p2,
                                                             psbt_overview_float_point_t p3,
                                                             int segments) {
    pvec<psbt_overview_float_point_t> points;
    points.push_back(p0);
    float step = 1.0f / (float)segments;
    for (int i = 1; i <= segments; ++i) {
        if (i == segments) { points.push_back(p3); break; }
        float t = step * (float)i;
        psbt_overview_float_point_t p01  = psbt_overview_linear_interp(p0, p1, t);
        psbt_overview_float_point_t p12  = psbt_overview_linear_interp(p1, p2, t);
        psbt_overview_float_point_t p23  = psbt_overview_linear_interp(p2, p3, t);
        psbt_overview_float_point_t p012 = psbt_overview_linear_interp(p01, p12, t);
        psbt_overview_float_point_t p123 = psbt_overview_linear_interp(p12, p23, t);
        points.push_back(psbt_overview_linear_interp(p012, p123, t));
    }
    return points;
}

// Snap a float point to the pixel grid, rounding the y-offset ABOUT the
// centerline `vertical_center_float` (lroundf is odd-symmetric, so mirror-image
// branches land on symmetric rows).
lv_point_t psbt_overview_snap_point(psbt_overview_float_point_t point,
                                    int32_t vertical_center, float vertical_center_float) {
    lv_point_t snapped;
    snapped.x = (int32_t)lroundf(point.x);
    snapped.y = vertical_center + (int32_t)lroundf(point.y - vertical_center_float);
    return snapped;
}

pvec<lv_point_t> psbt_overview_snap_curve(const pvec<psbt_overview_float_point_t> &float_curve,
                                          int32_t vertical_center, float vertical_center_float) {
    pvec<lv_point_t> out;
    out.reserve(float_curve.size());
    for (const auto &point : float_curve) {
        out.push_back(psbt_overview_snap_point(point, vertical_center, vertical_center_float));
    }
    return out;
}

// A smooth S (sigmoid) branch from the label end `tip` to the convergence `hub`,
// HORIZONTAL at both ends (Python's flow-diagram look). Built as a single cubic
// so it never forces a vertical inflection the way Python's two-quadratic chain
// does — that forced vertical is the "aggressive kink" on the short inner
// branches (fee / OP_RETURN) that opened a black negative-space gap. Returned
// tip-first.
pvec<psbt_overview_float_point_t> psbt_overview_branch(psbt_overview_float_point_t tip,
                                                       psbt_overview_float_point_t hub,
                                                       int segments) {
    float delta_x = hub.x - tip.x;   // signed span (hub right of tip for inputs, left for outputs)
    psbt_overview_float_point_t control_near_tip = { tip.x + delta_x * PSBT_OVERVIEW_S_TIP, tip.y };   // horizontal at the label
    psbt_overview_float_point_t control_near_hub = { hub.x - delta_x * PSBT_OVERVIEW_S_HUB, hub.y };   // horizontal at the hub
    return psbt_overview_cubic_bezier(tip, control_near_tip, control_near_hub, hub, segments);
}

// Stroke width scales with the display profile (3 px at the 240-height
// reference, ~6 px at 480-height) so the pictogram doesn't look thin on the
// larger panels.
int32_t psbt_overview_line_width() {
    int32_t width = 3 * active_profile().px_multiplier / 100;
    return width < 3 ? 3 : width;
}

// Convert a snapped integer polyline to lv_line's precise-point type. lv_line
// stores the pointer WITHOUT copying, so the returned vector must outlive the
// widget (it lives in the per-screen animation ctx).
pvec<lv_point_precise_t> psbt_overview_to_precise(const pvec<lv_point_t> &curve) {
    pvec<lv_point_precise_t> out;
    out.reserve(curve.size());
    for (const auto &point : curve) {
        out.push_back({ (lv_value_precise_t)point.x, (lv_value_precise_t)point.y });
    }
    return out;
}

// A polyline as a real lv_line widget (no canvas buffer) with round joins.
// `points` must outlive the widget (LVGL stores the pointer, not a copy).
// points=nullptr makes an empty line (used for pulse lines whose points are set
// per frame).
lv_obj_t *psbt_overview_make_line(lv_obj_t *parent, const lv_point_precise_t *points,
                                  uint32_t point_count, uint32_t color) {
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_line_width(line, psbt_overview_line_width(), LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    if (points && point_count >= 2) lv_line_set_points(line, points, point_count);
    return line;
}

// Per-screen animation state. The diagram is real lv_line widgets (children of
// `container`, auto-freed with the screen) — NOT a canvas buffer — so nothing
// large lands in the tiny LVGL internal pool and there is no per-frame
// full-buffer memcpy. Static gray paths are drawn once; the pulse is a few
// ORANGE lines whose points are set to the currently lit band each frame (LVGL
// recomposites over the true background, so there is no AA fringe to erase).
//
// Allocation idiom: C++ `new` / `delete` (in psbt_overview_cleanup_cb) — the
// struct owns std::vector members, so lv_malloc would skip their constructors
// and destructors (UB).
struct psbt_overview_animation_ctx_t {
    lv_obj_t   *container;   // holds every diagram line (validity-guards the timer)
    lv_timer_t *timer;
    // Geometry as precise points; the static lv_lines reference these, so they
    // must persist for the widgets' lifetime.
    pvec<pvec<lv_point_precise_t>> input_points;    // input curves (tip -> hub)
    pvec<pvec<lv_point_precise_t>> output_points;   // output curves (hub -> tip)
    pvec<lv_point_precise_t>       center_points;   // center bar
    // Orange pulse lines + their per-frame point buffers (one per input/output
    // curve + one for the center bar).
    pvec<lv_obj_t *>               pulse_input_lines;
    pvec<pvec<lv_point_precise_t>> pulse_input_buffers;
    lv_obj_t                      *pulse_center_line;
    pvec<lv_point_precise_t>       pulse_center_buffer;
    pvec<lv_obj_t *>               pulse_output_lines;
    pvec<pvec<lv_point_precise_t>> pulse_output_buffers;
    // Segment bookkeeping for the wall-clock pulse walk.
    int         input_segments, center_segments, output_segments, total_segments;
    int         band_segments, reset_at;
    float       head;        // pulse head position in global segments (fractional)
    uint32_t    last_tick;   // lv_tick timestamp of the previous frame
};

// Point an orange pulse line at the sub-polyline spanning segments
// [segment_low .. segment_high] of `source` (segment s connects points s and
// s+1), or hide it when that range is empty.
void psbt_overview_set_pulse(lv_obj_t *line, pvec<lv_point_precise_t> &buffer,
                             const pvec<lv_point_precise_t> &source,
                             int segment_low, int segment_high) {
    if (!line) return;
    if (segment_high < segment_low || segment_low < 0 || segment_high + 1 >= (int)source.size()) {
        lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    buffer.assign(source.begin() + segment_low, source.begin() + segment_high + 2);   // points [segment_low .. segment_high+1]
    lv_line_set_points(line, buffer.data(), (uint32_t)buffer.size());
    lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
}

// Wall-clock pulse (rate is frame-rate independent, matching the loading
// spinner's integrator). The lit band [head-band, head] walks the input ->
// center -> output path; during the fan phases every input/output curve lights
// its band segment at once (all funds flow together). Only the orange lines
// move each frame — the gray paths and labels are untouched, so LVGL repaints
// just the pulse's region (no full-buffer work).
void psbt_overview_pulse_timer_cb(lv_timer_t *timer) {
    psbt_overview_animation_ctx_t *ctx = (psbt_overview_animation_ctx_t *)lv_timer_get_user_data(timer);
    if (!ctx || !lv_obj_is_valid(ctx->container)) return;

    uint32_t now        = lv_tick_get();
    uint32_t elapsed_ms = now - ctx->last_tick;
    ctx->last_tick = now;
    if (elapsed_ms > PSBT_OVERVIEW_PULSE_MAX_STEP_MS) elapsed_ms = PSBT_OVERVIEW_PULSE_MAX_STEP_MS;   // starved-frame clamp

    float segments_per_ms = (float)ctx->total_segments / PSBT_OVERVIEW_PULSE_TRAVERSE_MS;
    ctx->head += (float)elapsed_ms * segments_per_ms;
    if (ctx->head >= (float)ctx->reset_at) ctx->head -= (float)ctx->reset_at;

    // The lit band's global segment range [tail_segment .. head_segment],
    // clamped to the path.
    int head_segment = (int)ctx->head;
    int tail_segment = (int)(ctx->head - (float)ctx->band_segments);
    if (head_segment > ctx->total_segments - 1) head_segment = ctx->total_segments - 1;
    if (tail_segment < 0) tail_segment = 0;

    // Input phase: global segments [0, input_segments).
    int input_band_start = tail_segment > 0 ? tail_segment : 0;
    int input_band_end   = head_segment < ctx->input_segments - 1 ? head_segment : ctx->input_segments - 1;
    for (size_t i = 0; i < ctx->pulse_input_lines.size(); ++i) {
        psbt_overview_set_pulse(ctx->pulse_input_lines[i], ctx->pulse_input_buffers[i],
                                ctx->input_points[i], input_band_start, input_band_end);
    }

    // Center phase: global segments [input_segments, input_segments + center_segments).
    int center_band_start = (tail_segment > ctx->input_segments ? tail_segment : ctx->input_segments) - ctx->input_segments;
    int center_band_end   = (head_segment < ctx->input_segments + ctx->center_segments - 1
                                 ? head_segment
                                 : ctx->input_segments + ctx->center_segments - 1) - ctx->input_segments;
    psbt_overview_set_pulse(ctx->pulse_center_line, ctx->pulse_center_buffer,
                            ctx->center_points, center_band_start, center_band_end);

    // Output phase: global segments [input_segments + center_segments, total_segments).
    int base = ctx->input_segments + ctx->center_segments;
    int output_band_start = (tail_segment > base ? tail_segment : base) - base;
    int output_band_end   = (head_segment < ctx->total_segments - 1 ? head_segment : ctx->total_segments - 1) - base;
    for (size_t i = 0; i < ctx->pulse_output_lines.size(); ++i) {
        psbt_overview_set_pulse(ctx->pulse_output_lines[i], ctx->pulse_output_buffers[i],
                                ctx->output_points[i], output_band_start, output_band_end);
    }
}

// Screen-root LV_EVENT_DELETE cleanup: stop the pulse timer FIRST (so it can
// never fire against dying widgets), then free the ctx. The lv_line and label
// children are freed with the screen itself.
void psbt_overview_cleanup_cb(lv_event_t *e) {
    psbt_overview_animation_ctx_t *ctx = (psbt_overview_animation_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->timer) lv_timer_delete(ctx->timer);   // stop the pulse before the lines are freed
    delete ctx;                                    // the lv_line children go with the screen
}

}  // namespace


void psbt_overview_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required content: the bottom button's label is user-visible CONTENT, which
    // always arrives localized from the host view layer (a string literal baked
    // here would be English-only by construction). The host either passes a full
    // button_list (used verbatim) or the single label as `button` (wrapped into a
    // one-entry button_list below). Thrown before the scaffold exists, so no
    // throw path can leak LVGL objects.
    if (!cfg.contains("button_list") &&
        (!cfg.contains("button") || !cfg["button"].is_string())) {
        throw std::runtime_error("psbt_overview_screen: button is required and must be a string (or pass a button_list array)");
    }

    // Transaction structure (mirrors the Python dataclass fields) — structural
    // values, so each reads with a write-if-absent-style default.
    int num_inputs = cfg.value("num_inputs", 1);
    if (num_inputs < 1) num_inputs = 1;   // at least one input row (a transaction always has one)
    int  num_self_transfer_outputs = cfg.value("num_self_transfer_outputs", 0);
    int  num_change_outputs        = cfg.value("num_change_outputs", 0);
    bool has_op_return             = cfg.value("has_op_return", false);
    std::vector<std::string> destination_addresses;
    if (cfg.contains("destination_addresses") && cfg["destination_addresses"].is_array()) {
        for (const auto &address : cfg["destination_addresses"]) {
            if (address.is_string()) destination_addresses.push_back(address.get<std::string>());
        }
    }

    // Chart-label templates: cfg["labels"] with English fallbacks. CONTENT-POLICY
    // FLAG (docs/screen-conformance-spec.md §5): these are user-visible strings
    // with baked English defaults, retained because no scenario supplies `labels`
    // and the desktop tools rely on the fallbacks; per-locale hosts pass
    // translated templates instead.
    const json label_templates =
        (cfg.contains("labels") && cfg["labels"].is_object()) ? cfg["labels"] : json::object();
    auto localized = [&](const char *key, const char *fallback) -> std::string {
        if (label_templates.contains(key) && label_templates[key].is_string()) {
            return label_templates[key].get<std::string>();
        }
        return std::string(fallback);
    };
    // Substitute the one "{}" slot in a label template ("input {}" -> "input 3").
    auto substitute_number = [](const std::string &template_text, long number) -> std::string {
        std::string out = template_text;
        size_t pos = out.find("{}");
        if (pos != std::string::npos) out.replace(pos, 2, std::to_string(number));
        return out;
    };
    const std::string input_n             = localized("input_n", "input {}");
    const std::string recipient_n         = localized("recipient_n", "recipient {}");
    const std::string recipient_1         = localized("recipient_1", "recipient 1");
    const std::string recipient_singular  = localized("recipient_singular", "recipient");
    const std::string ellipsis_series     = localized("ellipsis_series", "[ ... ]");
    const std::string self_transfer_label = localized("self_transfer", "self-transfer");
    const std::string fee_label           = localized("fee", "fee");
    const std::string op_return_label     = localized("op_return", "OP_RETURN");
    const std::string change_label        = localized("change", "change");

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False
    // (identical to the scaffold's own implicit defaults). The localized title
    // is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "psbt_overview_screen");

    // Wrap the host's localized `button` label (validated above) into the
    // scaffold's button_list shape when the host didn't pass a full list itself.
    if (!cfg.contains("button_list")) {
        cfg["button_list"] = json::array({ cfg["button"].get<std::string>() });
    }

    cfg["is_bottom_list"] = true;   // forced, not defaulted — Python: is_bottom_list = True

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // 1. BtcAmount headline in the upper body (Python's callout above the chart).
    lv_obj_t *headline = nullptr;
    if (cfg.contains("btc_amount") && cfg["btc_amount"].is_object() && screen.upper_body) {
        // Hug the top nav: the body already sits below the nav's own bottom buffer, so a
        // small pad here is enough. Keeping the callout high frees vertical room for the
        // pictogram (which is otherwise cramped on the many-row scenarios).
        lv_obj_set_style_pad_top(screen.upper_body, COMPONENT_PADDING / 4, LV_PART_MAIN);
        headline = btc_amount_from_cfg(screen.upper_body, cfg["btc_amount"]);
    }

    // --- Geometry ---
    //
    // Deviation from the canonical Body-then-Geometry order: this screen's ONE
    // measure pass runs BEFORE its main body content exists. The chart is drawn
    // into whatever vertical band remains between the headline and the
    // bottom-pinned button, so the band must be measured and the row/curve
    // coordinates fully solved first; the chart widgets (Body steps 2-9 below)
    // are then created from the solved geometry.

    // Chart-band measure pass — settle the scaffold + headline layout, then read
    // the free band between the headline and the pinned button (Python:
    // chart_y = below the callout; chart_height = up to the button).
    lv_obj_update_layout(screen.screen);

    const int32_t display_width     = lv_display_get_horizontal_resolution(NULL);
    const int32_t component_padding = COMPONENT_PADDING;
    const int32_t edge_padding      = EDGE_PADDING;

    int32_t chart_top;
    if (headline) {
        lv_area_t headline_coords;
        lv_obj_get_coords(headline, &headline_coords);
        chart_top = headline_coords.y2 + component_padding / 4;
    } else {
        chart_top = TOP_NAV_HEIGHT + component_padding;
    }
    // Band bottom = the shared scaffold query minus the chart's own
    // `component_padding` inset (both branches of the inlined form subtracted
    // it, so hoisting it out of the fallback/override pair is value-identical).
    int32_t chart_bottom = bottom_button_top_y(screen) - component_padding;
    const int32_t chart_height = chart_bottom - chart_top;

    const int32_t text_height = lv_font_get_line_height(&BODY_FONT);

    // Only build the diagram if there is room for at least one text row. (When
    // there is not, the screen renders headline + button only — and stays fully
    // stateless: no ctx, no timer, no cleanup callback.)
    if (chart_height >= text_height) {
        // Rendered width of a chart label in the body font (sizes both text columns).
        auto text_width = [&](const std::string &text) -> int32_t {
            lv_point_t size;
            lv_text_get_size(&size, text.c_str(), &BODY_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            return size.x;
        };

        // Input-column pass (Python inputs_column) — "1 input" for one; each row
        // numbered for a few; collapsed to 1 / 2 / [...] / N-1 / N when there are
        // many. Measure the column's widest text for the horizontal solver.
        std::vector<std::string> inputs_column;
        if (num_inputs == 1) {
            inputs_column.push_back(localized("one_input", "1 input"));
        } else if (num_inputs > 5) {
            inputs_column.push_back(substitute_number(input_n, 1));
            inputs_column.push_back(substitute_number(input_n, 2));
            inputs_column.push_back(ellipsis_series);
            inputs_column.push_back(substitute_number(input_n, num_inputs - 1));
            inputs_column.push_back(substitute_number(input_n, num_inputs));
        } else {
            for (int i = 0; i < num_inputs; ++i) inputs_column.push_back(substitute_number(input_n, i + 1));
        }
        int32_t max_inputs_text_width = 0;
        for (const auto &row_text : inputs_column) {
            max_inputs_text_width = std::max(max_inputs_text_width, text_width(row_text));
        }

        // Aesthetic geometry knobs (all scale with the display via
        // `component_padding`). The pictogram is kept PERFECTLY CENTERED on the
        // screen midline: each half mirrors the other (center bar straddles the
        // midline, equal-width curve columns), and the room per half is capped by
        // the WIDER text column — so a long label on one side pulls the short side
        // inward (blank space on the short side) instead of shoving the whole
        // diagram off-center.
        const int32_t text_gap         = component_padding / 2;      // balanced buffer between text and curve, both sides
        const int32_t curve_width_min  = 4 * component_padding;      // curve column min (Python's fixed width) ...
        const int32_t curve_width_max  = 6 * component_padding;      // ... up to ~50% wider when there's room (graceful curves)
        const int32_t center_bar_min   = 3 * component_padding / 2;  // center bar floor — kept short so the labels breathe
        const int32_t center_bar_max   = 5 * component_padding / 2;  // ... grows only a little past the floor
        const int32_t center_bar_floor = component_padding;          // hard floor when a tight screen can't center (below center_bar_min)
        const int32_t center_x         = display_width / 2;

        // Destination-column pass. The pictogram conveys transaction STRUCTURE,
        // not the actual addresses (those are verified on the later review
        // screens), so recipients are shown generically: "recipient" for one,
        // "recipient 1..N" for a few, and collapsed to
        // "recipient 1 / [ ... ] / recipient N" when there are many. (A deliberate
        // departure from the Python original, which truncated the addresses.)
        std::vector<std::string> destination_column;
        {
            const int num_recipients     = (int)destination_addresses.size();
            const int num_recipient_like = num_recipients + num_self_transfer_outputs;   // recipient-like outputs
            if (num_recipient_like <= 3) {
                if (num_recipients == 1) {
                    destination_column.push_back(recipient_singular);
                } else {
                    for (int i = 0; i < num_recipients; ++i) {
                        destination_column.push_back(substitute_number(recipient_n, i + 1));
                    }
                }
                for (int i = 0; i < num_self_transfer_outputs; ++i) {
                    destination_column.push_back(self_transfer_label);
                }
            } else {
                destination_column.push_back(recipient_1);
                destination_column.push_back(ellipsis_series);
                destination_column.push_back(substitute_number(recipient_n, num_recipient_like));
            }
            destination_column.push_back(fee_label);
            if (has_op_return) destination_column.push_back(op_return_label);
            for (int i = 0; i < num_change_outputs; ++i) destination_column.push_back(change_label);
        }
        int32_t destination_text_width = 0;
        for (const auto &row_text : destination_column) {
            destination_text_width = std::max(destination_text_width, text_width(row_text));
        }

        // Horizontal solve pass — two regimes:
        //   Centered (the common case, and preferred): the pictogram is symmetric
        //   about the screen midline; the per-half room is capped by the WIDER text
        //   column, so the short side pulls in and leaves blank margin. Curves grow
        //   first (graceful), then the center bar, then blank.
        //   Fit fallback (tight screen / wide labels, e.g. many recipients at 240):
        //   if even the minimum centered core won't fit, GIVE UP centering to keep
        //   the labels on screen — anchor both text columns at the edges, shrink
        //   the center bar to its hard floor, and split the leftover between the
        //   two (still equal-width) curves.
        const int32_t max_text_width = std::max(max_inputs_text_width, destination_text_width);
        const int32_t room = center_x - edge_padding - text_gap - max_text_width;   // per-half budget for (curve + half the center bar)

        int32_t center_bar_x, center_bar_width, output_hub_x;
        int32_t input_start_x, inputs_text_right, output_end_x, destination_column_x;

        if (room >= curve_width_min + center_bar_min / 2) {
            // Centered regime: spend the per-half room on the curve first (clamped
            // to its window); the remainder becomes the center bar (also clamped).
            int32_t curve_width = room - center_bar_min / 2;
            if (curve_width > curve_width_max) curve_width = curve_width_max;
            if (curve_width < curve_width_min) curve_width = curve_width_min;
            int32_t bar_width = 2 * (room - curve_width);
            if (bar_width < center_bar_min) bar_width = center_bar_min;
            if (bar_width > center_bar_max) bar_width = center_bar_max;

            center_bar_x         = center_x - bar_width / 2;
            center_bar_width     = bar_width;
            output_hub_x         = center_x + bar_width / 2;
            input_start_x        = center_bar_x - curve_width;
            inputs_text_right    = input_start_x - text_gap;
            output_end_x         = output_hub_x + curve_width;
            destination_column_x = output_end_x + text_gap;
        } else {
            // Fit fallback: labels at the edges, symmetric curves, minimal center bar.
            inputs_text_right    = edge_padding + max_inputs_text_width;                       // input text left-justified at the edge
            destination_column_x = display_width - edge_padding - destination_text_width;      // output text right-justified at the edge
            input_start_x        = inputs_text_right + text_gap;
            output_end_x         = destination_column_x - text_gap;
            int32_t available_width = output_end_x - input_start_x;                            // = curve + bar + curve
            int32_t bar_width = center_bar_floor;
            if (bar_width > available_width - 2) bar_width = available_width - 2;              // keep room for a sliver of curve
            if (bar_width < 1) bar_width = 1;
            int32_t curve_width = (available_width - bar_width) / 2;
            if (curve_width < 1) curve_width = 1;
            center_bar_x         = input_start_x + curve_width;
            center_bar_width     = bar_width;
            output_hub_x         = center_bar_x + bar_width;
            output_end_x         = output_hub_x + curve_width;                                 // re-derive so the two curves match exactly
        }

        // Vertical pitch pass — rows centered SYMMETRICALLY about the mid-line, so
        // an ODD count puts its middle row exactly on the centerline (a straight
        // branch, no bend). Pitch matches Python's even distribution (equal
        // top/inter/bottom gaps); deriving each row from the center avoids the
        // integer-truncation drift that used to nudge the whole block upward.
        const int32_t vertical_center       = chart_height / 2;
        const float   vertical_center_float = (float)chart_height / 2.0f;
        const int num_input_rows       = (int)inputs_column.size();
        const int num_destination_rows = (int)destination_column.size();

        auto row_pitch = [&](int row_count) -> float {
            if (row_count <= 1) return 0.0f;
            float natural = (float)(chart_height - row_count * text_height) / (float)(row_count + 1) + (float)text_height;
            // Never let the row block overflow the chart band
            // (block = (row_count-1)*pitch + text_height <= chart_height): when
            // there are more rows than fit at the natural pitch, compress so the
            // top/bottom rows stay inside the band instead of poking the button.
            float max_fit = (float)(chart_height - text_height) / (float)(row_count - 1);
            return natural < max_fit ? natural : max_fit;
        };
        // Float row center (snapped later about vertical_center_float), symmetric
        // about the mid-line.
        auto row_center_y_float = [&](int row_count, int row_index, float pitch) -> float {
            return vertical_center_float + ((float)row_index - (float)(row_count - 1) / 2.0f) * pitch;
        };
        const float input_pitch  = row_pitch(num_input_rows);
        const float output_pitch = row_pitch(num_destination_rows);

        // 2. Chart container + animation ctx. The diagram is real lv_line widgets
        //    (no canvas buffer): a full-screen RGB565 canvas can't fit the ESP32
        //    LVGL internal pool, and a canvas would also cost a full-buffer memcpy
        //    every animation frame. The transparent container is parented on
        //    screen.screen (not the body) because the chart band was solved in
        //    ABSOLUTE screen coordinates spanning the region between the upper
        //    body content and the bottom button, outside the body's padded flex
        //    layout and clipping; it also lets every child use the same
        //    chart-local coordinates the geometry above is computed in. Static
        //    gray paths are drawn once; the orange pulse (step 8) recolors a
        //    moving band via a few extra lines that LVGL recomposites fringe-free.
        (void)center_bar_width;   // the solver reports the bar as x + width, but the
                                  // build below consumes only center_bar_x /
                                  // output_hub_x (the snapped center-bar polyline in
                                  // step 5 spans those directly); the (void) keeps
                                  // the solver's outputs symmetric in both regimes
                                  // without an unused-variable warning.
        lv_obj_t *chart = lv_obj_create(screen.screen);
        lv_obj_remove_style_all(chart);
        lv_obj_set_pos(chart, 0, chart_top);
        lv_obj_set_size(chart, display_width, chart_height);
        lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);

        // Heap animation ctx — C++ new; see the allocation-idiom note at the
        // struct declaration. Freed by psbt_overview_cleanup_cb on screen delete.
        psbt_overview_animation_ctx_t *ctx = new psbt_overview_animation_ctx_t();
        ctx->container         = chart;
        ctx->timer             = nullptr;
        ctx->pulse_center_line = nullptr;

        // Chart labels are collected here and created LAST (step 9) so they
        // compose on top of every line.
        struct label_spec_t { std::string text; int32_t x, y; };
        std::vector<label_spec_t> chart_labels;

        // 3. Input rows: label + an S-curve to the center bar (built in float,
        //    snapped symmetric about vertical_center_float, stored as precise
        //    points for the static lv_line).
        for (int i = 0; i < num_input_rows; ++i) {
            const std::string &row_text = inputs_column[i];
            int32_t label_width = text_width(row_text);
            float   row_center  = row_center_y_float(num_input_rows, i, input_pitch);
            chart_labels.push_back({ row_text, inputs_text_right - label_width,
                                     (int32_t)lroundf(row_center) - text_height / 2 });

            psbt_overview_float_point_t start_point = { (float)input_start_x, row_center };
            psbt_overview_float_point_t hub         = { (float)center_bar_x, vertical_center_float };   // input hub = left end of the center bar
            pvec<psbt_overview_float_point_t> float_curve;
            if (num_inputs == 1) float_curve = { start_point, hub };   // one input: a straight segment
            else                 float_curve = psbt_overview_branch(start_point, hub, 2 * PSBT_OVERVIEW_CURVE_STEPS);
            ctx->input_points.push_back(psbt_overview_to_precise(
                psbt_overview_snap_curve(float_curve, vertical_center, vertical_center_float)));
        }

        // 4. Output rows: mirror of the input side, stored hub -> tip (the pulse
        //    travels outward).
        for (int i = 0; i < num_destination_rows; ++i) {
            const std::string &row_text = destination_column[i];
            float row_center = row_center_y_float(num_destination_rows, i, output_pitch);
            chart_labels.push_back({ row_text, destination_column_x,
                                     (int32_t)lroundf(row_center) - text_height / 2 });

            psbt_overview_float_point_t hub       = { (float)output_hub_x, vertical_center_float };   // output hub = right end of the center bar
            psbt_overview_float_point_t end_point = { (float)output_end_x, row_center };
            pvec<psbt_overview_float_point_t> float_curve = psbt_overview_branch(end_point, hub, 2 * PSBT_OVERVIEW_CURVE_STEPS);
            std::reverse(float_curve.begin(), float_curve.end());
            ctx->output_points.push_back(psbt_overview_to_precise(
                psbt_overview_snap_curve(float_curve, vertical_center, vertical_center_float)));
        }

        // 5. Center bar, segmented so the pulse can step across it (straight, on
        //    the centerline).
        psbt_overview_float_point_t center_bar_start = { (float)center_bar_x, vertical_center_float };
        psbt_overview_float_point_t center_bar_end   = { (float)output_hub_x, vertical_center_float };
        ctx->center_points = psbt_overview_to_precise(psbt_overview_snap_curve(
            { center_bar_start,
              psbt_overview_linear_interp(center_bar_start, center_bar_end, 0.25f),
              psbt_overview_linear_interp(center_bar_start, center_bar_end, 0.50f),
              psbt_overview_linear_interp(center_bar_start, center_bar_end, 0.75f),
              center_bar_end }, vertical_center, vertical_center_float));

        // 6. Static gray paths (drawn once). They reference ctx's point vectors,
        //    which outlive the widgets (ctx is freed only when the screen — and
        //    these children — is).
        for (auto &curve : ctx->input_points) {
            psbt_overview_make_line(chart, curve.data(), (uint32_t)curve.size(), PSBT_OVERVIEW_ASSOCIATION_COLOR);
        }
        psbt_overview_make_line(chart, ctx->center_points.data(), (uint32_t)ctx->center_points.size(), PSBT_OVERVIEW_ASSOCIATION_COLOR);
        for (auto &curve : ctx->output_points) {
            psbt_overview_make_line(chart, curve.data(), (uint32_t)curve.size(), PSBT_OVERVIEW_ASSOCIATION_COLOR);
        }

        // 7. Segment counts + pulse cadence (wall-clock; see the constants block).
        ctx->input_segments  = (int)ctx->input_points[0].size() - 1;
        ctx->center_segments = (int)ctx->center_points.size() - 1;
        ctx->output_segments = (int)ctx->output_points[0].size() - 1;
        ctx->total_segments  = ctx->input_segments + ctx->center_segments + ctx->output_segments;
        ctx->band_segments   = std::max(1, (int)lroundf(PSBT_OVERVIEW_PULSE_BAND_FRACTION * (float)ctx->total_segments));
        ctx->reset_at        = ctx->total_segments + ctx->band_segments
                               + std::max(1, (int)lroundf(PSBT_OVERVIEW_PULSE_GAP_FRACTION * (float)ctx->total_segments));
        ctx->head            = 0.0f;
        ctx->last_tick       = lv_tick_get();

        // 8. Orange pulse lines (created after the gray paths so they compose on
        //    top). One per input/output curve + one for the center bar; each
        //    starts hidden and is pointed at the lit band each frame. Buffers are
        //    pre-sized so lv_line_set_points never reallocates. "animate": false
        //    renders the static diagram only.
        if (cfg.value("animate", true)) {
            for (int i = 0; i < num_input_rows; ++i) {
                lv_obj_t *line = psbt_overview_make_line(chart, nullptr, 0, PSBT_OVERVIEW_PULSE_COLOR);
                lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
                ctx->pulse_input_lines.push_back(line);
                ctx->pulse_input_buffers.emplace_back();
                ctx->pulse_input_buffers.back().reserve(ctx->band_segments + 2);
            }
            ctx->pulse_center_line = psbt_overview_make_line(chart, nullptr, 0, PSBT_OVERVIEW_PULSE_COLOR);
            lv_obj_add_flag(ctx->pulse_center_line, LV_OBJ_FLAG_HIDDEN);
            ctx->pulse_center_buffer.reserve(ctx->band_segments + 2);
            for (int i = 0; i < num_destination_rows; ++i) {
                lv_obj_t *line = psbt_overview_make_line(chart, nullptr, 0, PSBT_OVERVIEW_PULSE_COLOR);
                lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
                ctx->pulse_output_lines.push_back(line);
                ctx->pulse_output_buffers.emplace_back();
                ctx->pulse_output_buffers.back().reserve(ctx->band_segments + 2);
            }
            ctx->timer = lv_timer_create(psbt_overview_pulse_timer_cb, PSBT_OVERVIEW_PULSE_TICK_MS, ctx);
        }

        // 9. Labels last, on top of every line.
        for (const auto &spec : chart_labels) {
            lv_obj_t *label = lv_label_create(chart);
            lv_obj_set_style_pad_all(label, 0, LV_PART_MAIN);
            lv_obj_set_style_text_font(label, &BODY_FONT, LV_PART_MAIN);
            lv_obj_set_style_text_color(label, lv_color_hex(PSBT_OVERVIEW_LABEL_COLOR), LV_PART_MAIN);
            lv_label_set_text(label, spec.text.c_str());
            lv_obj_set_pos(label, spec.x, spec.y);
        }

        // Tier-2 cleanup: one LV_EVENT_DELETE callback on the screen root deletes
        // the pulse timer, then frees the ctx.
        lv_obj_add_event_cb(screen.screen, psbt_overview_cleanup_cb, LV_EVENT_DELETE, ctx);
    }

    // --- Navigation + load ---

    // Menu-style default index: the single bottom action button starts focused
    // (the host may override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
}
