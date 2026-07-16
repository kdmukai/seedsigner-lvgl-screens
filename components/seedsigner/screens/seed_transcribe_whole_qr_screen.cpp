// seed_transcribe_whole_qr_screen
//
// Python provenance: SeedTranscribeSeedQRWholeQRScreen (seed_screens.py)
//
// The "whole QR" overview step of the SeedQR hand-transcription flow (Python:
// a WarningEdgesMixin + ButtonListScreen). Shows the COMPLETE SeedQR /
// CompactSeedQR as a small on-screen grid so the user can take in the whole
// pattern before zooming in to copy it cell-by-cell
// (seed_transcribe_zoomed_qr_screen). The single bottom button advances into
// the zoomed flow through the standard navigation callback, and the pulsing
// DIRE_WARNING WarningEdges border signals that seed material is on screen.
//
// Layout parity with Python __post_init__:
//   - Standard ButtonListScreen chrome: localized TopNav title, one bottom
//     button, is_bottom_list forced true, DIRE_WARNING status color.
//   - A square QR field, centered horizontally, whose top edge sits directly
//     below the TopNav and whose side fills the gap down to the button minus
//     one COMPONENT_PADDING:
//         qr_side = button.top - top_nav.bottom - COMPONENT_PADDING
//     The QR is a WHITE field (1-module quiet zone) with black modules; this
//     mirrors python-qrcode's StyledPilImage, whose back_color is white.
//
// QR rendering: the module matrix is DIRECT-DRAWN in a DRAW_MAIN_END callback
// (per-module shapes over the object's own white background), exactly like
// qr_display_screen — NEVER a full-image lv_canvas, which OOMs the ESP32's small
// internal-DRAM LVGL pool. Unlike qr_display's machine-scanned QRs (fast
// qrcodegen auto-mask), a TRANSCRIPTION QR is hand-copied, so its mask pattern
// must match the Pi Zero's python-qrcode output module-for-module. We therefore
// replicate python-qrcode's best_mask_pattern() selection here (an 8-mask
// penalty pass over qr_python_lost_point) rather than using qrcodegen's auto
// mask. See the mask-parity block below.
//
// Lifecycle: Tier 2 (stateful) in its minimal form — one heap ctx (the encoded
// matrix + draw parameters) with one LV_EVENT_DELETE cleanup callback,
// registered on the QR object rather than the screen root; the QR object lives
// exactly as long as the screen, so teardown timing is identical. No timers,
// no animations owned here, no host-push state.
//
// cfg:
//   qr_data        (string, required, non-empty)  the SeedQR payload: a numeric
//          digit stream (standard SeedQR) or hex-encoded entropy bytes
//          (CompactSeedQR).
//   data_encoding  (string, default "utf8")  "utf8" (payload bytes = the string
//          bytes) | "hex" (payload arrives hex-encoded; JSON cannot carry raw
//          bytes). Deliberately narrower than qr_core's shared decoder: no
//          base64 — SeedQR payloads are digits or hex only.
//   qr_mode        (string, default "auto")  "numeric" | "alphanumeric" |
//          "byte" | "auto" (probe order numeric > alphanumeric > byte). Must
//          name the mode the Pi Zero encoder used so the module matrix matches;
//          an unrecognized string throws.
//   border         (int, default 1)   quiet-zone modules around the matrix
//          (Python: qr.qrimage(border=1)). Accepted unvalidated (qr_display
//          clamps 0..20): a negative value silently mis-maps the module grid —
//          kept as-is, tracked in the conformance bug ledger.
//   top_nav.title  (string, required)  localized title (Python:
//          _("Transcribe SeedQR")).
//   top_nav.show_back_button  (bool, default true)   Python ButtonListScreen default.
//   top_nav.show_power_button (bool, default false)  Python ButtonListScreen default.
//   button_list    (array, required, non-empty)  the localized action button.
//          Python formats _("Begin {}x{}") with the QR module count
//          (TRANSLATOR_NOTE: refers to the QR size — 21x21, 25x25, or 29x29),
//          so the host supplies the already-formatted "Begin NxN" label.
//   is_bottom_list  forced true (Python: is_bottom_list = True); a host-supplied
//          value is ignored.
//   initial_selected_index    (int, optional)        overrides the default initial
//            focus of 0 (navigation layer; Python selected_button).
//   input.mode                (string, optional)     "touch" | "hardware" input-mode
//            override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / add_warning_edges_overlay / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // seed_transcribe_whole_qr_screen decl, screen_scaffold_t fields
#include "gui_constants.h"    // BACKGROUND_COLOR, COMPONENT_PADDING, DIRE_WARNING_COLOR
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_obj tree, draw-event direct rendering, display resolution queries

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <cctype>             // isspace (hex-payload whitespace skip)
#include <cmath>              // llround (drift-free module-grid mapping)
#include <cstdint>            // uint8_t (qrcodegen matrix bytes)
#include <cstring>            // memcpy (byte-mode encode scratch)
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string
#include <vector>             // std::vector (decoded payload bytes)

// Feature-gated last: the bundled qrcodegen lives inside the LVGL source tree.
// This file sits one level deeper (screens/) than the component root, so the
// repo-root-relative reach needs the extra ../.
#if LV_USE_QRCODE
#include "../../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"  // qrcodegen_* encoder + module accessors
#endif

using json = nlohmann::json;


#if LV_USE_QRCODE

namespace {

// NOTE: the decode + mask-parity region below duplicates qr_core internals;
// slated for merge into qr_core at the rollout consolidation decision — kept
// verbatim-identical to qr_core's copies until then so the two stay diffable.

// -------------------------------------------------------------------------
// Payload decoding: JSON cannot carry raw bytes, so a binary CompactSeedQR
// payload arrives hex-encoded (data_encoding:"hex"); a numeric SeedQR digit
// string arrives as-is (utf8). Mirrors qr_core's shared qr_decode_payload
// hex/utf8 paths (base64 deliberately dropped — SeedQR payloads are digits
// or hex only).
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
// qr_display parity work. The identical pass also lives in qr_core.cpp behind
// qr_encode_bytes' match_python_mask flag (the path
// seed_transcribe_zoomed_qr_screen consumes); this file still carries its own
// private copy — see the merge-slated NOTE at the top of this region.
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

// Draw context. Pure POD (pointer + ints) -> lv_malloc + lv_memzero + lv_free
// (no C++ members, so there are no ctors/dtors an lv_malloc would skip).
struct seed_transcribe_whole_qr_ctx_t {
    uint8_t  *qr_matrix;  // qrcodegen matrix, owned; freed on qr_obj delete
    int       border;     // quiet-zone modules around the matrix (1, like Python)
    lv_obj_t *qr_obj;
};

// Map module-grid index module_index (0..total) to a pixel offset within the QR
// object, distributing the (generally non-integer) module size so the cells tile
// the full `side` exactly with no accumulated drift — matching PIL's resize of
// the box_size=5 native image up to the on-screen qr_width.
int seed_transcribe_whole_qr_cell_offset(int module_index, int total, int side) {
    return (int)llround((double)module_index * (double)side / (double)total);
}

// Paint the black modules on top of the object's white background (DRAW_MAIN_END).
// Draw-event coords are screen-absolute, so offset every rect by the object's
// absolute top-left.
//
// Module style: each dark module is a plain full-cell SQUARE, so adjacent dark cells
// abut into the finder/alignment/solid regions with no seams and the grid reads as a
// standard, fully-scannable QR — exactly what a transcriber copies cell-by-cell. (This
// screen previously mimicked python-qrcode's rounded CircleModuleDrawer with a
// per-module circle plus adjacency bridges. Squares drop that for a far cheaper draw:
// a plain fill instead of an anti-aliased circle, and one rect per module instead of
// up to three. Together with the partial-refresh cull below, that brings the initial
// paint down to the SPI panel's flush floor — no off-screen buffer, no extra memory.)
void seed_transcribe_whole_qr_draw_cb(lv_event_t *e) {
    seed_transcribe_whole_qr_ctx_t *ctx =
        (seed_transcribe_whole_qr_ctx_t *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    if (!ctx || !layer || !ctx->qr_matrix) return;

    lv_area_t obj_area;
    lv_obj_get_coords(ctx->qr_obj, &obj_area);
    int side = obj_area.x2 - obj_area.x1 + 1;   // square: width == height

    int size   = qrcodegen_getSize(ctx->qr_matrix);
    int total  = size + 2 * ctx->border;
    int border = ctx->border;

    // Cell pixel edges for module-grid index module_index (border-inclusive), absolute.
    auto edge_x = [&](int module_index) {
        return obj_area.x1 + seed_transcribe_whole_qr_cell_offset(module_index, total, side);
    };
    auto edge_y = [&](int module_index) {
        return obj_area.y1 + seed_transcribe_whole_qr_cell_offset(module_index, total, side);
    };
    auto dark = [&](int module_x, int module_y) {
        return module_x >= 0 && module_y >= 0 && module_x < size && module_y < size &&
               qrcodegen_getModule(ctx->qr_matrix, module_x, module_y);
    };

    lv_draw_rect_dsc_t module_dsc;   // full-cell black square per dark module
    lv_draw_rect_dsc_init(&module_dsc);
    module_dsc.bg_color = lv_color_black();
    module_dsc.bg_opa   = LV_OPA_COVER;
    module_dsc.radius   = 0;   // adjacent dark cells abut into solid regions, no seams

    // Partial-refresh cull. On an SPI panel LVGL renders this screen in horizontal
    // stripes, firing this callback once per stripe the QR spans; without a cull each
    // firing redraws the WHOLE module grid, so a large QR paints in a slow top-to-bottom
    // sweep (one full grid per stripe). Skip cells that don't overlap the current
    // stripe's clip. A full cell that straddles a stripe seam still overlaps — and is
    // drawn (LVGL-clipped) in — BOTH stripes, so squares tile across seams with no gap
    // and no margin fudge; total work is one grid pass. No-op on a full-frame DSI panel
    // (single stripe: clip == the whole object).
    const lv_area_t clip = layer->_clip_area;

    for (int module_y = 0; module_y < size; module_y++) {
        int cell_top    = edge_y(border + module_y);
        int cell_bottom = edge_y(border + module_y + 1) - 1;
        if (cell_bottom < clip.y1 || cell_top > clip.y2) continue;   // stripe row cull
        for (int module_x = 0; module_x < size; module_x++) {
            if (!dark(module_x, module_y)) continue;
            int cell_left  = edge_x(border + module_x);
            int cell_right = edge_x(border + module_x + 1) - 1;
            if (cell_right < clip.x1 || cell_left > clip.x2) continue;  // column cull
            lv_area_t cell_area = { cell_left, cell_top, cell_right, cell_bottom };
            lv_draw_rect(layer, &module_dsc, &cell_area);
        }
    }
}

// LV_EVENT_DELETE teardown, registered on the QR object (which lives exactly as
// long as the screen): free the owned matrix, then the ctx.
void seed_transcribe_whole_qr_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    seed_transcribe_whole_qr_ctx_t *ctx =
        (seed_transcribe_whole_qr_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    if (ctx->qr_matrix) lv_free(ctx->qr_matrix);
    lv_free(ctx);
}

}  // namespace

#endif  // LV_USE_QRCODE


void seed_transcribe_whole_qr_screen(void *ctx_json) {
#if !LV_USE_QRCODE
    // Built without the bundled QR encoder (no shipping build does this). Load a
    // blank screen so the entry point exists and navigation does not crash.
    (void)ctx_json;
    lv_obj_t *blank_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(blank_screen, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    load_screen_and_cleanup_previous(blank_screen);
#else
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required fields: one throw per field, before any allocation and before the
    // scaffold exists, so no throw path can leak buffers or LVGL objects.
    if (!cfg.contains("qr_data") || !cfg["qr_data"].is_string() ||
        cfg["qr_data"].get<std::string>().empty()) {
        throw std::runtime_error("seed_transcribe_whole_qr_screen: qr_data is required and must be a non-empty string");
    }
    std::string qr_data       = cfg["qr_data"].get<std::string>();
    std::string qr_mode       = cfg.value("qr_mode", std::string("auto"));
    std::string data_encoding = cfg.value("data_encoding", std::string("utf8"));
    if (data_encoding != "utf8" && data_encoding != "hex") {
        throw std::runtime_error("seed_transcribe_whole_qr_screen: data_encoding must be utf8|hex");
    }

    // qr_mode selects WHICH encoder mode reproduces the Pi Zero's matrix. For a
    // hand-transcription screen a silently different mode yields a validly-encoded
    // but WRONG module pattern (the user copies a QR the device never rendered),
    // so an unrecognized string is a developer error and throws — the same policy
    // as the sibling QR screens.
    if (qr_mode != "auto" && qr_mode != "numeric" && qr_mode != "alphanumeric" && qr_mode != "byte") {
        throw std::runtime_error("seed_transcribe_whole_qr_screen: qr_mode must be auto|numeric|alphanumeric|byte");
    }

    // button_list is user-visible CONTENT and always arrives localized from the
    // host view layer (Python formats the TRANSLATOR-noted _("Begin {}x{}") with
    // the module count host-side); a string literal baked here would be
    // English-only by construction.
    if (!cfg.contains("button_list") || !cfg["button_list"].is_array() || cfg["button_list"].empty()) {
        throw std::runtime_error("seed_transcribe_whole_qr_screen: button_list is required and must be a non-empty array");
    }

    // Structural defaults (write-if-absent, never user-visible text). Python
    // ButtonListScreen defaults: show_back_button=True, show_power_button=False.
    // The localized title itself is content and must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "seed_transcribe_whole_qr_screen");

    cfg["is_bottom_list"] = true;   // forced, not defaulted — Python: is_bottom_list = True

    int border = cfg.value("border", 1);   // structural default — Python: qr.qrimage(border=1)

    // --- QR encode ---

    // Encode up front, before any LVGL object exists: payload decode/encode
    // failures throw leak-free, and the finished matrix is handed to the draw
    // callback below.
    std::vector<uint8_t> payload = decode_payload(qr_data, data_encoding);
    uint8_t *temp_buffer = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);
    uint8_t *qr_matrix   = (uint8_t *)lv_malloc(qrcodegen_BUFFER_LEN_MAX);
    if (!temp_buffer || !qr_matrix) {
        if (temp_buffer) lv_free(temp_buffer);
        if (qr_matrix) lv_free(qr_matrix);
        throw std::runtime_error("seed_transcribe_whole_qr_screen: QR buffer alloc failed");
    }
    bool encode_succeeded = encode_transcription_qr(qr_mode, payload, temp_buffer, qr_matrix);
    lv_free(temp_buffer);  // scratch only needed during encode
    if (!encode_succeeded) {
        lv_free(qr_matrix);
        throw std::runtime_error("seed_transcribe_whole_qr_screen: payload too large to encode");
    }

    // --- Scaffold ---

    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/false);

    // --- Body ---

    // 1. QR object: a plain object whose white background is the quiet-zone
    //    field; the black modules are painted on top in
    //    seed_transcribe_whole_qr_draw_cb (no large canvas buffer). Parented to
    //    the screen ROOT — not the flex body — to escape the body's layout and
    //    clipping, then absolutely positioned in the Geometry pass below,
    //    mirroring Python's paste at ((canvas_w - qr_w)/2, top_nav.height).
    seed_transcribe_whole_qr_ctx_t *ctx =
        (seed_transcribe_whole_qr_ctx_t *)lv_malloc(sizeof(seed_transcribe_whole_qr_ctx_t));
    lv_memzero(ctx, sizeof(*ctx));
    ctx->qr_matrix = qr_matrix;   // ownership transfers to ctx; freed in the cleanup cb
    ctx->border    = border;

    lv_obj_t *qr_obj = lv_obj_create(screen.screen);
    lv_obj_remove_style_all(qr_obj);
    lv_obj_set_style_bg_color(qr_obj, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(qr_obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_remove_flag(qr_obj, LV_OBJ_FLAG_SCROLLABLE);
    ctx->qr_obj = qr_obj;
    lv_obj_add_event_cb(qr_obj, seed_transcribe_whole_qr_draw_cb, LV_EVENT_DRAW_MAIN_END, ctx);
    lv_obj_add_event_cb(qr_obj, seed_transcribe_whole_qr_cleanup_cb, LV_EVENT_DELETE, ctx);

    // 2. WarningEdgesMixin — pulsing ORANGE border (seed material on screen).
    add_warning_edges_overlay(screen.screen, DIRE_WARNING_COLOR);

    // --- Geometry ---

    // Free-band fit pass (the single measure-and-place pass): size + center the
    // QR from the laid-out chrome.
    // Python: qr_side = buttons[0].screen_y - top_nav.height - COMPONENT_PADDING,
    //         pasted at ((canvas_w - qr_side)/2, top_nav.height).
    lv_obj_update_layout(screen.screen);

    lv_area_t top_nav_area, button_area;
    lv_obj_get_coords(screen.top_nav, &top_nav_area);
    int qr_top = top_nav_area.y2 + 1;   // directly below the TopNav (Python top_nav.height)

    // Band bottom measured from button_list[0] directly (deliberately NOT the
    // shared bottom_button_top_y helper: its no-button fallback is
    // display_height - BUTTON_HEIGHT, this screen's fills to the display bottom).
    int qr_side;
    if (screen.button_list_count > 0) {
        lv_obj_get_coords(screen.button_list[0], &button_area);
        qr_side = button_area.y1 - qr_top - COMPONENT_PADDING;
    } else {
        // Defensive only — unreachable now that button_list is validated
        // non-empty above: fill to the display bottom.
        qr_side = lv_display_get_vertical_resolution(NULL) - qr_top - COMPONENT_PADDING;
    }
    if (qr_side < 1) qr_side = 1;

    int screen_width = lv_display_get_horizontal_resolution(NULL);
    int qr_x = (screen_width - qr_side) / 2;

    lv_obj_set_size(qr_obj, qr_side, qr_side);
    lv_obj_align(qr_obj, LV_ALIGN_TOP_LEFT, qr_x, qr_top);

    // --- Navigation + load ---

    // Menu-style default index: the single action button starts focused (the
    // host may override via cfg initial_selected_index).
    bind_screen_navigation(cfg, screen, /*default_initial_index=*/0);

    load_screen_and_cleanup_previous(screen.screen);
#endif
}
