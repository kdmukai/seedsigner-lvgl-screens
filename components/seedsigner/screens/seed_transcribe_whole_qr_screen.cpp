// seed_transcribe_whole_qr_screen — the "whole QR" overview step of the SeedQR
// hand-transcription flow (Python SeedTranscribeSeedQRWholeQRScreen, a
// WarningEdgesMixin + ButtonListScreen). It shows the COMPLETE SeedQR /
// CompactSeedQR as a small grid overview so the user can see the whole pattern
// before zooming in to copy it cell-by-cell, with a single bottom "Begin NxN"
// button and the pulsing ORANGE WarningEdges (the seed is on screen).
//
// Layout parity with Python __post_init__:
//   - Standard ButtonListScreen chrome (TopNav title "Transcribe SeedQR", one
//     bottom button "Begin {N}x{N}", is_bottom_list=True, DIRE_WARNING status).
//   - A square QR image, centered horizontally, whose top edge sits directly
//     below the TopNav and whose height fills the gap down to the button minus
//     one COMPONENT_PADDING:
//         qr_side = button.top - top_nav.bottom - COMPONENT_PADDING
//     The QR is a WHITE field (1-module quiet zone) with black modules; this
//     mirrors python-qrcode's StyledPilImage, whose back_color is white.
//
// QR rendering: the module matrix is DIRECT-DRAWN in a DRAW_MAIN_END callback
// (per-module rects on the object's own white background), exactly like
// qr_display_screen — NEVER a full-image lv_canvas, which OOMs the ESP32's small
// internal-DRAM LVGL pool. Unlike qr_display's machine-scanned QRs (fast
// qrcodegen auto-mask), a TRANSCRIPTION QR is hand-copied, so its mask pattern
// must match the Pi Zero's python-qrcode output module-for-module. We therefore
// replicate python-qrcode's best_mask_pattern() selection here (an 8-mask
// penalty pass over qr_python_lost_point) rather than using qrcodegen's auto
// mask. See the mask-parity block below.

#include "seedsigner.h"
#include "navigation.h"
#include "gui_constants.h"
#include "screen_scaffold.h"

#include "lvgl.h"

#if LV_USE_QRCODE
// File lives one level deeper (screens/), so this repo-root-relative reach to
// LVGL's bundled qrcodegen needs an extra ../ vs. the component-root copy.
#include "../../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"
#endif

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>
#include <cctype>

using json = nlohmann::json;

namespace {

#if LV_USE_QRCODE

// -------------------------------------------------------------------------
// Payload decoding: JSON cannot carry raw bytes, so a binary CompactSeedQR
// payload arrives hex-encoded (data_encoding:"hex"); a numeric SeedQR digit
// string arrives as-is (utf8). Mirrors qr_display_screen's qr_decode_payload.
// -------------------------------------------------------------------------
bool hexval(char c, int &v) {
    if (c >= '0' && c <= '9') { v = c - '0';        return true; }
    if (c >= 'a' && c <= 'f') { v = c - 'a' + 10;   return true; }
    if (c >= 'A' && c <= 'F') { v = c - 'A' + 10;   return true; }
    return false;
}

std::vector<uint8_t> decode_payload(const std::string &s, const std::string &enc) {
    std::vector<uint8_t> out;
    if (enc == "hex") {
        std::string h;
        for (char c : s) if (!isspace((unsigned char)c)) h.push_back(c);
        if (h.size() % 2 != 0)
            throw std::runtime_error("seed_transcribe_whole_qr_screen: hex payload has odd length");
        out.reserve(h.size() / 2);
        for (size_t i = 0; i < h.size(); i += 2) {
            int hi, lo;
            if (!hexval(h[i], hi) || !hexval(h[i + 1], lo))
                throw std::runtime_error("seed_transcribe_whole_qr_screen: invalid hex payload");
            out.push_back((uint8_t)((hi << 4) | lo));
        }
    } else {  // "utf8": raw string bytes (SeedQR numeric digits)
        out.assign(s.begin(), s.end());
    }
    return out;
}

// -------------------------------------------------------------------------
// python-qrcode mask-selection parity (transcription QRs only).
//
// Reproduces the Python `qrcode` library (pinned qrcode==7.3.1 on the Pi Zero).
// Its automatic mask choice differs from qrcodegen's for some payloads (both
// valid QRs, different module pattern); a hand-copied SeedQR MUST match the
// device exactly. best_mask_pattern() scores each candidate over a matrix whose
// format-info modules are blanked light (makeImpl(test=True)); we do the same
// (qr_is_format_module), take the lowest penalty, break ties by lowest index.
// qr_python_lost_point transcribes qrcode/util.py lost_point (rules 1-4).
//
// This is the same algorithm proven matrix-identical in the SeedQR zoomed-in /
// qr_display parity work; kept self-contained here so the shared qr_display path
// stays on its fast auto-mask.
// -------------------------------------------------------------------------

// Format-info modules (two 15-bit strips by the finders + the fixed dark module),
// read as light while scoring — mirrors python-qrcode's makeImpl(test=True).
bool qr_is_format_module(int row, int col, int size) {
    if (col == 8) return (row <= 5) || (row == 7) || (row == 8) || (row == size - 8) || (row >= size - 7);
    if (row == 8) return (col <= 5) || (col == 7) || (col == 8) || (col >= size - 8);
    return false;
}

// python-qrcode util.lost_point over the format-blanked matrix.
long qr_python_lost_point(const uint8_t *qr, int size) {
    auto M = [&](int row, int col) -> int {
        return qr_is_format_module(row, col, size) ? 0 : (qrcodegen_getModule(qr, col, row) ? 1 : 0);
    };
    long lost = 0;

    // Rule 1: same-color runs >= 5 in every row/col; a run of length L adds (L-2).
    for (int row = 0; row < size; row++) {
        int prev = M(row, 0), len = 0;
        for (int col = 0; col < size; col++) { int c = M(row, col);
            if (c == prev) len++; else { if (len >= 5) lost += len - 2; len = 1; prev = c; } }
        if (len >= 5) lost += len - 2;
    }
    for (int col = 0; col < size; col++) {
        int prev = M(0, col), len = 0;
        for (int row = 0; row < size; row++) { int c = M(row, col);
            if (c == prev) len++; else { if (len >= 5) lost += len - 2; len = 1; prev = c; } }
        if (len >= 5) lost += len - 2;
    }

    // Rule 2: 2x2 uniform blocks add 3 (python's next()-skip reproduced).
    for (int row = 0; row < size - 1; row++) {
        for (int col = 0; col < size - 1; col++) {
            int top_right = M(row, col + 1);
            if      (top_right != M(row + 1, col + 1)) { col++; continue; }
            else if (top_right != M(row, col))         continue;
            else if (top_right != M(row + 1, col))     continue;
            else lost += 3;
        }
    }

    // Rule 3: 1:1:3:1:1 finder-like pattern with a 4-light margin adds 40 (horspool skip).
    for (int row = 0; row < size; row++) {
        for (int col = 0; col < size - 10; col++) {
            #define D(k) M(row, col + (k))
            if (!D(1) && D(4) && !D(5) && D(6) && !D(9) &&
                ((D(0) && D(2) && D(3) && !D(7) && !D(8) && !D(10)) ||
                 (!D(0) && !D(2) && !D(3) && D(7) && D(8) && D(10)))) lost += 40;
            if (M(row, col + 10)) col++;
            #undef D
        }
    }
    for (int col = 0; col < size; col++) {
        for (int row = 0; row < size - 10; row++) {
            #define E(k) M(row + (k), col)
            if (!E(1) && E(4) && !E(5) && E(6) && !E(9) &&
                ((E(0) && E(2) && E(3) && !E(7) && !E(8) && !E(10)) ||
                 (!E(0) && !E(2) && !E(3) && E(7) && E(8) && E(10)))) lost += 40;
            if (M(row + 10, col)) row++;
            #undef E
        }
    }

    // Rule 4: dark-module ratio departure from 50%, every 5% adds 10.
    long dark = 0;
    for (int row = 0; row < size; row++) for (int col = 0; col < size; col++) dark += M(row, col);
    double dev = (double)dark / ((double)size * size) * 100.0 - 50.0;
    if (dev < 0) dev = -dev;
    lost += (long)(dev / 5.0) * 10;
    return lost;
}

// Encode `data` into `out` (qrcodegen buffer), choosing the SAME mask
// python-qrcode would. `mode` is "numeric" | "alphanumeric" | "byte" | "auto".
// ECC=L, no ECC boost — matches python-qrcode's ERROR_CORRECT_L. Returns false
// if the payload cannot be encoded.
bool encode_transcription_qr(const std::string &mode, const std::vector<uint8_t> &data,
                             uint8_t *tmp, uint8_t *out) {
    std::string s((const char *)data.data(), data.size());
    bool clean = !data.empty() && s.find('\0') == std::string::npos;

    bool use_numeric = false, use_alnum = false;
    if (mode == "numeric")            use_numeric = qrcodegen_isNumeric(s.c_str());
    else if (mode == "alphanumeric")  use_alnum   = qrcodegen_isAlphanumeric(s.c_str());
    else if (mode == "byte")          { /* raw bytes */ }
    else {  // auto: numeric > alphanumeric > byte
        if      (clean && qrcodegen_isNumeric(s.c_str()))      use_numeric = true;
        else if (clean && qrcodegen_isAlphanumeric(s.c_str())) use_alnum   = true;
    }

    // Resolve the concrete encoder once, as a lambda over the mask, so every
    // candidate mask below encodes identically.
    auto encode_mask = [&](enum qrcodegen_Mask mk) -> bool {
        if (use_numeric) {
            struct qrcodegen_Segment seg = qrcodegen_makeNumeric(s.c_str(), tmp);
            return qrcodegen_encodeSegmentsAdvanced(&seg, 1, qrcodegen_Ecc_LOW, 1, 40, mk, false, tmp, out);
        }
        if (use_alnum) {
            struct qrcodegen_Segment seg = qrcodegen_makeAlphanumeric(s.c_str(), tmp);
            return qrcodegen_encodeSegmentsAdvanced(&seg, 1, qrcodegen_Ecc_LOW, 1, 40, mk, false, tmp, out);
        }
        if (data.size() > (size_t)qrcodegen_BUFFER_LEN_MAX) return false;
        memcpy(tmp, data.data(), data.size());  // encodeBinary clobbers its input; out is separate
        return qrcodegen_encodeBinary(tmp, data.size(), out, qrcodegen_Ecc_LOW, 1, 40, mk, false);
    };

    // python-qrcode best_mask_pattern(): lowest lost_point wins, ties break to
    // the lowest index (its strict `>`). Only ever a one-shot static QR.
    int best = -1; long best_lp = 0;
    for (int m = 0; m < 8; m++) {
        if (!encode_mask((enum qrcodegen_Mask)m)) return false;
        long lp = qr_python_lost_point(out, qrcodegen_getSize(out));
        if (best < 0 || lp < best_lp) { best_lp = lp; best = m; }
    }
    return encode_mask((enum qrcodegen_Mask)best);  // re-encode the winner into `out`
}

// -------------------------------------------------------------------------
// Direct-draw QR grid.
// -------------------------------------------------------------------------
struct whole_qr_ctx_t {
    uint8_t *out;      // qrcodegen matrix, owned; freed on qr_obj delete
    int      border;   // quiet-zone modules around the matrix (1, like Python)
    lv_obj_t *qr_obj;
};

// Map module-grid index m (0..total) to a pixel offset within the QR object,
// distributing the (generally non-integer) module size so the cells tile the
// full `side` exactly with no accumulated drift — matching PIL's resize of the
// box_size=5 native image up to the on-screen qr_width.
static inline int cell_off(int m, int total, int side) {
    return (int)llround((double)m * (double)side / (double)total);
}

// Paint the black modules on top of the object's white background (DRAW_MAIN_END).
// Draw-event coords are screen-absolute, so offset every rect by the object's
// absolute top-left.
//
// Module STYLE parity with Python QR.STYLE__ROUNDED (python-qrcode's
// CircleModuleDrawer, then downscaled — which smooths tangent circles into
// connected blobs): each dark module is drawn as an inscribed CIRCLE, and every
// orthogonally-adjacent pair of dark modules is bridged by a full-cell rect
// between their centers. The union yields exactly the reference look — isolated
// modules are dots, runs are rounded capsules, and solid regions (finder
// patterns, dense data) fill in with rounded OUTER corners.
void whole_qr_draw_cb(lv_event_t *e) {
    whole_qr_ctx_t *ctx   = (whole_qr_ctx_t *)lv_event_get_user_data(e);
    lv_layer_t     *layer = lv_event_get_layer(e);
    if (!ctx || !layer || !ctx->out) return;

    lv_area_t obj_area;
    lv_obj_get_coords(ctx->qr_obj, &obj_area);
    int side = obj_area.x2 - obj_area.x1 + 1;   // square: width == height

    int size  = qrcodegen_getSize(ctx->out);
    int total = size + 2 * ctx->border;
    int b     = ctx->border;

    // Cell pixel edges for module-grid index m (border-inclusive), absolute.
    auto ex = [&](int m) { return obj_area.x1 + cell_off(m, total, side); };
    auto ey = [&](int m) { return obj_area.y1 + cell_off(m, total, side); };
    auto dark = [&](int qx, int qy) {
        return qx >= 0 && qy >= 0 && qx < size && qy < size &&
               qrcodegen_getModule(ctx->out, qx, qy);
    };

    lv_draw_rect_dsc_t dot;   // inscribed circle per module
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = lv_color_black();
    dot.bg_opa   = LV_OPA_COVER;
    dot.radius   = LV_RADIUS_CIRCLE;

    lv_draw_rect_dsc_t bridge; // square connector between adjacent module centers
    lv_draw_rect_dsc_init(&bridge);
    bridge.bg_color = lv_color_black();
    bridge.bg_opa   = LV_OPA_COVER;
    bridge.radius   = 0;

    for (int qy = 0; qy < size; qy++) {
        int cy1 = ey(b + qy);
        int cy2 = ey(b + qy + 1) - 1;
        int cym = (cy1 + cy2) / 2;
        for (int qx = 0; qx < size; qx++) {
            if (!dark(qx, qy)) continue;
            int cx1 = ex(b + qx);
            int cx2 = ex(b + qx + 1) - 1;
            int cxm = (cx1 + cx2) / 2;

            lv_area_t a = { cx1, cy1, cx2, cy2 };
            lv_draw_rect(layer, &dot, &a);   // the module's circle

            if (dark(qx + 1, qy)) {          // bridge to the right neighbor
                int nx2 = ex(b + qx + 2) - 1;
                int nxm = (ex(b + qx + 1) + nx2) / 2;
                lv_area_t hb = { cxm, cy1, nxm, cy2 };
                lv_draw_rect(layer, &bridge, &hb);
            }
            if (dark(qx, qy + 1)) {          // bridge to the bottom neighbor
                int ny2 = ey(b + qy + 2) - 1;
                int nym = (ey(b + qy + 1) + ny2) / 2;
                lv_area_t vb = { cx1, cym, cx2, nym };
                lv_draw_rect(layer, &bridge, &vb);
            }
        }
    }
}

void whole_qr_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    whole_qr_ctx_t *ctx = (whole_qr_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->out) lv_free(ctx->out);
    lv_free(ctx);
}

#endif  // LV_USE_QRCODE

}  // namespace


extern "C" void seed_transcribe_whole_qr_screen(void *ctx_json) {
#if !LV_USE_QRCODE
    // Built without the bundled QR encoder (no shipping build does this). Load a
    // blank screen so the entry point exists and navigation does not crash.
    (void)ctx_json;
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    load_screen_and_cleanup_previous(scr);
#else
    json cfg;
    parse_screen_json_ctx((const char *)ctx_json, cfg);

    if (!cfg.contains("qr_data") || !cfg["qr_data"].is_string() ||
        cfg["qr_data"].get<std::string>().empty()) {
        throw std::runtime_error("seed_transcribe_whole_qr_screen: non-empty qr_data (string) is required");
    }
    std::string qr_data = cfg["qr_data"].get<std::string>();
    std::string mode    = cfg.value("qr_mode", std::string("auto"));
    std::string enc     = cfg.value("data_encoding", std::string("utf8"));
    if (enc != "utf8" && enc != "hex")
        throw std::runtime_error("seed_transcribe_whole_qr_screen: data_encoding must be utf8|hex");
    int border = cfg.value("border", 1);   // Python WholeQR uses border=1

    // Encode up front so we know the module count (drives the "Begin NxN" label).
    std::vector<uint8_t> payload = decode_payload(qr_data, enc);
    uint8_t *tmp = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);
    uint8_t *out = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);
    if (!tmp || !out) {
        if (tmp) lv_free(tmp);
        if (out) lv_free(out);
        throw std::runtime_error("seed_transcribe_whole_qr_screen: QR buffer alloc failed");
    }
    bool ok = encode_transcription_qr(mode, payload, tmp, out);
    lv_free(tmp);  // scratch only needed during encode
    if (!ok) {
        lv_free(out);
        throw std::runtime_error("seed_transcribe_whole_qr_screen: payload too large to encode");
    }
    int num_modules = qrcodegen_getSize(out);

    // --- Chrome config (ButtonListScreen parity) ---------------------------
    if (!cfg.contains("top_nav") || !cfg["top_nav"].is_object()) cfg["top_nav"] = json::object();
    if (!cfg["top_nav"].contains("title")) cfg["top_nav"]["title"] = "Transcribe SeedQR";
    cfg["is_bottom_list"] = true;
    if (!cfg.contains("button_list")) {
        // TRANSLATOR note (Python): "Begin {}x{}" — the QR module dimensions.
        std::string label = "Begin " + std::to_string(num_modules) + "x" + std::to_string(num_modules);
        cfg["button_list"] = json::array({ label });
    }

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);

    // --- QR object (white field; modules direct-drawn) ---------------------
    // Added to the screen ROOT (not the flex body) and absolutely positioned
    // after layout, mirroring Python's paste at ((canvas_w - qr_w)/2, top_nav.h).
    // A plain object with a white background is the quiet-zone field; the black
    // modules are painted on top in whole_qr_draw_cb (no large canvas buffer).
    whole_qr_ctx_t *ctx = (whole_qr_ctx_t *)lv_malloc(sizeof(whole_qr_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->out    = out;      // ownership transfers to ctx; freed in cleanup cb
    ctx->border = border;

    lv_obj_t *qr_obj = lv_obj_create(screen.screen);
    lv_obj_remove_style_all(qr_obj);
    lv_obj_set_style_bg_color(qr_obj, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(qr_obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(qr_obj, LV_OBJ_FLAG_SCROLLABLE);
    ctx->qr_obj = qr_obj;
    lv_obj_add_event_cb(qr_obj, whole_qr_draw_cb, LV_EVENT_DRAW_MAIN_END, ctx);
    lv_obj_add_event_cb(qr_obj, whole_qr_cleanup_cb, LV_EVENT_DELETE, ctx);

    // WarningEdgesMixin — pulsing ORANGE border (seed material on screen).
    add_warning_edges_overlay(screen.screen, DIRE_WARNING_COLOR);

    bind_screen_navigation(cfg, screen, 0);

    // --- Geometry: size + center the QR from the laid-out chrome ------------
    // Python: qr_side = buttons[0].screen_y - top_nav.height - COMPONENT_PADDING,
    //         pasted at ((canvas_w - qr_side)/2, top_nav.height).
    lv_obj_update_layout(screen.screen);

    lv_area_t nav_a, btn_a;
    lv_obj_get_coords(screen.top_nav, &nav_a);
    int qr_top = nav_a.y2 + 1;   // directly below the TopNav (Python top_nav.height)

    int qr_side;
    if (screen.button_list_count > 0) {
        lv_obj_get_coords(screen.button_list[0], &btn_a);
        qr_side = btn_a.y1 - qr_top - COMPONENT_PADDING;
    } else {
        // Defensive: no button (shouldn't happen) — fill to the display bottom.
        qr_side = lv_display_get_vertical_resolution(NULL) - qr_top - COMPONENT_PADDING;
    }
    if (qr_side < 1) qr_side = 1;

    int screen_w = lv_display_get_horizontal_resolution(NULL);
    int qr_x = (screen_w - qr_side) / 2;

    lv_obj_set_size(qr_obj, qr_side, qr_side);
    lv_obj_align(qr_obj, LV_ALIGN_TOP_LEFT, qr_x, qr_top);

    load_screen_and_cleanup_previous(screen.screen);
#endif
}
