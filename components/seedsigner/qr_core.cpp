// qr_core — QR encode/decode core + shared gutter close button. See qr_core.h.
//
// Compiled once, with external linkage, so both chrome-free QR screens
// (qr_display_screen, seed_transcribe_zoomed_qr_screen) can share the encoder,
// the payload decoder, and the python-qrcode mask-parity pass without duplicating
// ~200 lines of finicky, parity-critical code.

#include "qr_core.h"
#include "gui_constants.h"

#include "lvgl.h"

#if LV_USE_QRCODE
#include "../../third_party/lvgl/src/libs/qrcode/qrcodegen.h"

#include <cctype>
#include <cstring>
#include <stdexcept>

// ── Internal helpers (kept file-local, exactly as they were in the monolith's
//    anonymous namespace; only qr_decode_payload/qr_encode_bytes are exported). ──
namespace {

bool qr_hexval(char c, int &v) {
    if (c >= '0' && c <= '9') { v = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
    return false;
}

std::vector<uint8_t> qr_base64_decode(const std::string &in) {
    auto dec = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (char c : in) {
        if (c == '=') break;
        int d = dec(c);
        if (d < 0) continue;  // skip whitespace/newlines
        val = (val << 6) | d;
        bits += 6;
        if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xff)); bits -= 8; }
    }
    return out;
}

// --- python-qrcode mask-selection parity (transcription QRs only) ----------
// Target we reproduce: the Python `qrcode` library at qrcode==7.3.1 (the pin in SeedSigner's
// requirements.txt — what the Pi Zero runs). Its automatic mask-pattern choice differs from
// qrcodegen's qrcodegen_Mask_AUTO for some payloads (both valid QRs, different module
// pattern); the SeedQR TRANSCRIPTION screens have the user copy that pattern BY HAND, so it
// must match the device exactly. The penalty/tie-break is version-specific, so re-verify if
// that pin ever moves.
//
// How it's exact: python-qrcode's QRCode.best_mask_pattern() scores each candidate via
// makeImpl(test=True), which BLANKS every format-info module to light (0). So we score the
// format-blanked matrix (qr_is_format_module), take the lowest penalty, and break ties by
// lowest index (its strict `>`). qr_python_lost_point faithfully transcribes qrcode/util.py
// lost_point (rules 1-4, iterator/horspool skips included); verified matrix-identical to the
// device across the real SeedQR/CompactSeedQR vectors + random numeric/byte payloads. (Only
// format-info is blanked; the version-info block exists only at version>=7 / size>=45, which
// SeedQR never reaches.)

// Format-info modules (the two 15-bit strips by the finders + the fixed dark module),
// read as light while scoring — mirrors python-qrcode's makeImpl(test=True).
bool qr_is_format_module(int row, int col, int size) {
    if (col == 8) return (row <= 5) || (row == 7) || (row == 8) || (row == size - 8) || (row >= size - 7);
    if (row == 8) return (col <= 5) || (col == 7) || (col == 8) || (col >= size - 8);
    return false;
}

// python-qrcode util.lost_point over the format-blanked matrix (best_mask_pattern scoring).
long qr_python_lost_point(const uint8_t *qr, int size) {
    // Module read with the format-info area forced light (0), matching test=True.
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

}  // namespace

// Decode the cfg["qr_data"] string (which crossed the JSON boundary) per data_encoding.
// JSON can't carry raw bytes, so binary payloads (CompactSeedQR) arrive hex/base64.
std::vector<uint8_t> qr_decode_payload(const std::string &s, const std::string &enc) {
    std::vector<uint8_t> out;
    if (enc == "hex") {
        std::string h;
        for (char c : s) if (!isspace((unsigned char)c)) h.push_back(c);
        if (h.size() % 2 != 0) throw std::runtime_error("qr_display_screen: hex payload has odd length");
        out.reserve(h.size() / 2);
        for (size_t i = 0; i < h.size(); i += 2) {
            int hi, lo;
            if (!qr_hexval(h[i], hi) || !qr_hexval(h[i + 1], lo))
                throw std::runtime_error("qr_display_screen: invalid hex payload");
            out.push_back((uint8_t)((hi << 4) | lo));
        }
    } else if (enc == "base64") {
        out = qr_base64_decode(s);
    } else {  // utf8: raw string bytes
        out.assign(s.begin(), s.end());
    }
    return out;
}

// Encode `data` (len bytes) into `out`, using `tmp` as qrcodegen scratch, per `mode`.
// SeedQR stays NUMERIC (to match the SeedQR standard version); BBQR and similar all-uppercase
// payloads use ALPHANUMERIC (denser QR, how real devices show BBQR); everything else (incl.
// binary CompactSeedQR) is BYTE. "auto" picks the most compact mode the payload allows,
// mirroring Python's qrcode auto-detect. ECC=L and boostEcl=false mirror Python's non-boosting
// qrcode lib. Returns false if it can't fit. Both `tmp` and `out` must be qrcodegen_BUFFER_LEN_MAX.
//
// match_python_mask: false = qrcodegen's fast built-in auto mask (qr_display's static +
// per-frame animated QRs — machine-scanned, mask invisible). true = pick the same mask
// python-qrcode would (the SeedQR transcribe screens, whose pattern is hand-copied), via
// an 8-mask penalty pass. Shared by qr_display_screen and the SeedQR transcribe screens.
bool qr_encode_bytes(qr_encode_mode_t mode, const uint8_t *data, size_t len,
                     uint8_t *tmp, uint8_t *out, bool match_python_mask) {
    if (mode == QR_ENC_AUTO) {
        // numeric > alphanumeric > byte. isNumeric/isAlphanumeric take a C string, so a NUL
        // in the payload (binary) disqualifies both and it falls through to byte.
        std::string s((const char *)data, len);
        bool clean = len > 0 && s.find('\0') == std::string::npos;
        if      (clean && qrcodegen_isNumeric(s.c_str()))      mode = QR_ENC_NUMERIC;
        else if (clean && qrcodegen_isAlphanumeric(s.c_str())) mode = QR_ENC_ALNUM;
        else                                                   mode = QR_ENC_BYTE;
    }

    // Resolve the concrete encoder ONCE (including the mislabeled-mode -> byte fallback) as a
    // lambda over the mask, so the auto path and every candidate mask below encode identically.
    std::string s((const char *)data, len);
    bool use_numeric = (mode == QR_ENC_NUMERIC) && qrcodegen_isNumeric(s.c_str());
    bool use_alnum   = (mode == QR_ENC_ALNUM)   && qrcodegen_isAlphanumeric(s.c_str());
    auto encode_mask = [&](enum qrcodegen_Mask mk) -> bool {
        if (use_numeric) {
            // makeNumeric writes the segment into tmp; encodeSegmentsAdvanced reuses tmp as scratch.
            struct qrcodegen_Segment seg = qrcodegen_makeNumeric(s.c_str(), tmp);
            return qrcodegen_encodeSegmentsAdvanced(&seg, 1, qrcodegen_Ecc_LOW, 1, 40, mk, false, tmp, out);
        }
        if (use_alnum) {
            struct qrcodegen_Segment seg = qrcodegen_makeAlphanumeric(s.c_str(), tmp);
            return qrcodegen_encodeSegmentsAdvanced(&seg, 1, qrcodegen_Ecc_LOW, 1, 40, mk, false, tmp, out);
        }
        if (len > (size_t)qrcodegen_BUFFER_LEN_MAX) return false;
        memcpy(tmp, data, len);  // encodeBinary clobbers dataAndTemp; out must be separate
        return qrcodegen_encodeBinary(tmp, len, out, qrcodegen_Ecc_LOW, 1, 40, mk, false);
    };

    // Fast path: a FIXED mask, a single encode. Used wherever the mask is invisible because a
    // machine scans it (qr_display's static + animated frames). qrcodegen_Mask_AUTO would apply
    // all 8 masks and run a 4-pass penalty score over the whole matrix EVERY frame — measured
    // ~91% of encode time (v34: ~7ms -> ~0.6ms desktop, 11x). The chosen mask is irrelevant to a
    // scanner, so pinning Mask_0 keeps the QR valid while dropping the auto-mask scoring entirely.
    // (Only the animated/static qr_display path takes this branch; the SeedQR transcribe screens
    // use match_python_mask=true below, which still parity-matches python-qrcode.)
    if (!match_python_mask) return encode_mask(qrcodegen_Mask_0);

    // Parity path (SeedQR transcription): choose the SAME mask python-qrcode would so the
    // hand-copied pattern matches a Pi Zero. Encode each candidate, score it with
    // qr_python_lost_point, keep the lowest penalty (lowest index breaks ties). ~9 encodes,
    // only ever a one-shot static QR — never the per-frame animated path.
    int best = -1; long best_lp = 0;
    for (int m = 0; m < 8; m++) {
        if (!encode_mask((enum qrcodegen_Mask)m)) return false;
        long lp = qr_python_lost_point(out, qrcodegen_getSize(out));
        if (best < 0 || lp < best_lp) { best_lp = lp; best = m; }
    }
    return encode_mask((enum qrcodegen_Mask)best);  // re-encode the winner into `out`
}

// Top-right gutter "X" close button — the touch exit affordance for chrome-free QR
// screens (qr_display + the SeedQR transcribe views). Placed in the screen GUTTER
// (aligned to the parent, NOT over the QR): a QR's top-right finder pattern sits only
// a couple quiet-zone modules in, so a content-corner button would overlap it and
// break scannability. Touch profiles are landscape (short_dim > 240) and always have
// side gutters — same assumption as the camera-overlay gutter buttons. `cb` fires on
// LV_EVENT_CLICKED with `user_data`.
lv_obj_t *build_gutter_close_button(lv_obj_t *parent, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_button_create(parent);
    int sz = TOP_NAV_BUTTON_SIZE;
    lv_obj_set_size(btn, sz, sz);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_align(btn, LV_ALIGN_TOP_RIGHT, -EDGE_PADDING, EDGE_PADDING);

    lv_obj_t *x = lv_label_create(btn);
    lv_label_set_text(x, SeedSignerIconConstants::CLOSE);
    lv_obj_set_style_text_font(x, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
    lv_obj_set_style_text_color(x, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_center(x);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

#endif  // LV_USE_QRCODE
