#include "runner_core.h"

#include <nlohmann/json.hpp>

#include "gui_constants.h"
#include "seedsigner.h"
#include "input_profile.h"

#include <unordered_map>

using json = nlohmann::ordered_json;

namespace runner_core {

namespace {

typedef void (*screen_fn_t)(void* ctx_json);

const std::unordered_map<std::string, screen_fn_t> k_screen_registry = {
    {"main_menu_screen", main_menu_screen},
    {"button_list_screen", button_list_screen},
    {"screensaver_screen", screensaver_screen},
    {"opening_splash_screen", opening_splash_screen},
    {"large_icon_status_screen", large_icon_status_screen},
    {"seed_add_passphrase_screen", seed_add_passphrase_screen},
    {"camera_preview_overlay_screen", camera_preview_overlay_screen},
    {"camera_entropy_overlay_screen", camera_entropy_overlay_screen},
    {"keyboard_screen", keyboard_screen},
    {"seed_mnemonic_entry_screen", seed_mnemonic_entry_screen},
    {"seed_finalize_screen", seed_finalize_screen},
    {"seed_export_xpub_details_screen", seed_export_xpub_details_screen},
    {"seed_review_passphrase_screen", seed_review_passphrase_screen},
    {"seed_words_screen", seed_words_screen},
    {"loading_spinner_screen", loading_spinner_screen},
    {"qr_display_screen", qr_display_screen},
    {"seed_transcribe_zoomed_qr_screen", seed_transcribe_zoomed_qr_screen},
    {"psbt_overview_screen", psbt_overview_screen},
    {"psbt_address_details_screen", psbt_address_details_screen},
    {"psbt_change_details_screen", psbt_change_details_screen},
    {"psbt_math_screen", psbt_math_screen},
    {"settings_locale_picker_screen", settings_locale_picker_screen},
    {"multisig_wallet_descriptor_screen", multisig_wallet_descriptor_screen},
    {"seed_sign_message_confirm_address_screen", seed_sign_message_confirm_address_screen},
    {"settings_qr_confirmation_screen", settings_qr_confirmation_screen},
    {"seed_sign_message_confirm_message_screen", seed_sign_message_confirm_message_screen},
    {"seed_address_verification_screen", seed_address_verification_screen},
    {"tools_calc_final_word_screen", tools_calc_final_word_screen},
    {"tools_calc_final_word_done_screen", tools_calc_final_word_done_screen},
    {"seed_transcribe_whole_qr_screen", seed_transcribe_whole_qr_screen},
    {"tools_address_explorer_address_list_screen", tools_address_explorer_address_list_screen},
    {"reset_screen", reset_screen},
    {"power_off_not_required_screen", power_off_not_required_screen},
    {"donate_screen", donate_screen},
    {"psbt_op_return_screen", psbt_op_return_screen},
    {"io_test_screen", io_test_screen},
};

// Display / framebuffer state.
int g_width = 0;
int g_height = 0;
std::vector<uint16_t> g_fb;        // RGB565, width*height
std::vector<uint8_t> g_draw_buf;   // LVGL render buffer (2 bytes/px)
lv_display_t* g_disp = nullptr;

// Keypad indev state (HARDWARE mode).
uint32_t g_pending_key = 0;
bool g_key_ready = false;

// Pointer indev state (TOUCH mode), in viewport-local coordinates.
int g_pointer_x = 0;
int g_pointer_y = 0;
bool g_pointer_pressed = false;

bool g_initialized = false;

// Animated-QR test driver (interactive runners only). qr_display_screen is host-driven:
// the real device pushes each frame from the encode_qr fountain/Specter sequence in Python
// (UR fountain frames are generated on the fly and cannot be precomputed). Since the
// runners have no Python, a scenario may carry a "test_frames" array and the runner STANDS
// IN FOR THE HOST, pushing each frame via qr_display_set_frame() at ~6 FPS (5/30 s, matching
// Python's QRDisplayThread). Empty => a static QR, nothing to cycle.
std::vector<std::string> g_qr_test_frames;
size_t   g_qr_frame_idx = 0;
uint32_t g_qr_frame_accum_ms = 0;
bool     g_qr_tip_was_active = false;
constexpr uint32_t QR_FRAME_INTERVAL_MS = 167;  // 5/30 s

// --- Live density demo (runner host stand-in) ---
// The real host reacts to seedsigner_lvgl_on_qr_density() by re-fragmenting the payload and pushing
// a frame whose QR version renders at the chosen px/module. The runner has no fountain, so it
// synthesizes that here: pick the densest QR version that still renders at >= px px/module on this
// screen and push a digit payload sized to fill it, so the on-screen QR visibly changes density as
// the slider moves. Numeric ECC-L max-digit capacity per version 1..40 (numeric/auto scenarios
// encode digits as numeric, so the version lands exactly; other modes still get denser/sparser).
const int kNumericCapacity[40] = {
      41,   77,  127,  187,  255,  322,  370,  461,  552,  652,
     772,  883, 1022, 1101, 1250, 1408, 1548, 1725, 1903, 2061,
    2232, 2409, 2620, 2812, 3057, 3283, 3517, 3669, 3909, 4158,
    4417, 4686, 4965, 5253, 5529, 5836, 6153, 6479, 6743, 7089,
};
// Byte + alphanumeric ECC-L capacities, so the digit payload lands on the intended version for a
// screen configured in byte / alphanumeric mode too (numeric/auto encode digits as numeric).
const int kByteCapacity[40] = {
      17,   32,   53,   78,  106,  134,  154,  192,  230,  271,
     321,  367,  425,  458,  520,  586,  644,  718,  792,  858,
     929, 1003, 1091, 1171, 1273, 1367, 1465, 1528, 1628, 1732,
    1840, 1952, 2068, 2188, 2303, 2431, 2563, 2699, 2809, 2953,
};
const int kAlnumCapacity[40] = {
      25,   47,   77,  114,  154,  195,  224,  279,  335,  395,
     468,  535,  619,  667,  758,  854,  938, 1046, 1153, 1249,
    1352, 1460, 1588, 1704, 1853, 1990, 2132, 2223, 2369, 2520,
    2677, 2840, 3009, 3183, 3351, 3537, 3729, 3927, 4087, 4296,
};
std::string g_qr_density_payload;         // synthesized frame; once set, shown statically
bool        g_qr_density_active = false;  // true once density has been moved on this screen
int         g_qr_border = 2;              // qr_display quiet-zone modules (from cfg; default 2)
std::string g_qr_mode = "auto";           // qr_display encode mode (from cfg) -> capacity table

// Synthesize + push a QR that renders at `px` px/module on the current screen.
void apply_density_demo(int px) {
    if (px < 2) px = 2;
    if (px > 6) px = 6;
    int sd = g_width < g_height ? g_width : g_height;
    if (sd <= 0) return;
    int target_v = 1;
    for (int v = 1; v <= 40; ++v) {
        int total_modules = (4 * v + 17) + 2 * g_qr_border;
        if (sd / total_modules >= px) target_v = v;   // densest version still >= px px/module
    }
    // Fill the version in the SCREEN's encode mode so the encoder actually selects target_v.
    const int* cap = kNumericCapacity;                 // numeric / auto (auto picks numeric for digits)
    if (g_qr_mode == "byte")               cap = kByteCapacity;
    else if (g_qr_mode == "alphanumeric")  cap = kAlnumCapacity;
    int len = cap[target_v - 1];
    g_qr_density_payload.resize((size_t)len);
    for (int i = 0; i < len; ++i)
        g_qr_density_payload[(size_t)i] = char('0' + (i * 7 + 3) % 10);  // looks like data, not a block
    g_qr_density_active = true;
    qr_display_set_frame(g_qr_density_payload.data(), g_qr_density_payload.size());
}

void configure_qr_test_frames(const std::string& fn_name, const std::string& json_ctx) {
    g_qr_test_frames.clear();
    g_qr_frame_idx = 0;
    g_qr_frame_accum_ms = 0;
    g_qr_tip_was_active = false;
    g_qr_density_active = false;
    g_qr_density_payload.clear();
    g_qr_border = 2;
    g_qr_mode = "auto";
    if (fn_name != "qr_display_screen" || json_ctx.empty()) return;
    int  initial_px      = 4;      // qr_display_screen's initial_px_per_module default
    bool density_control = false;  // matches the screen's default (fixed QRs have no density)
    try {
        json ctx = json::parse(json_ctx);
        if (ctx.contains("border") && ctx["border"].is_number_integer())
            g_qr_border = ctx["border"].get<int>();
        if (ctx.contains("qr_mode") && ctx["qr_mode"].is_string())
            g_qr_mode = ctx["qr_mode"].get<std::string>();
        if (ctx.contains("initial_px_per_module") && ctx["initial_px_per_module"].is_number_integer())
            initial_px = ctx["initial_px_per_module"].get<int>();
        if (ctx.contains("density_control") && ctx["density_control"].is_boolean())
            density_control = ctx["density_control"].get<bool>();
        if (ctx.contains("test_frames") && ctx["test_frames"].is_array()) {
            for (const auto& f : ctx["test_frames"]) {
                if (f.is_string()) g_qr_test_frames.push_back(f.get<std::string>());
            }
        }
    } catch (...) {
        // invalid ctx -> treat as a static QR (nothing to cycle)
    }
    // Density-enabled STATIC QR: the scenario's fixed qr_data renders at its own natural px/module
    // (often outside the 3..6 band on a large screen), so it would NOT match the density slider's
    // initial position. Re-render the initial frame at initial_px_per_module so the shown QR matches
    // the slider — the match the real host gives by sizing its first fountain fragment to the
    // setting. A fixed QR (density_control off) keeps its real qr_data; animated scenarios keep
    // cycling test_frames and sync density only on user interaction.
    if (density_control && g_qr_test_frames.empty()) apply_density_demo(initial_px);
}

// LVGL flush: copy the rendered RGB565 region into our framebuffer.
void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    const uint16_t* color_p = reinterpret_cast<const uint16_t*>(px_map);
    for (int y = area->y1; y <= area->y2; ++y) {
        if (y < 0 || y >= g_height) {
            // Still consume the source pixels for this row so the pointer stays aligned.
            color_p += (area->x2 - area->x1 + 1);
            continue;
        }
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x >= 0 && x < g_width) {
                g_fb[static_cast<size_t>(y) * g_width + x] = *color_p;
            }
            ++color_p;
        }
    }
    lv_display_flush_ready(disp);
}

void keypad_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    if (input_profile_get_mode() != INPUT_MODE_HARDWARE) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (g_key_ready) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = g_pending_key;
        g_key_ready = false;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void pointer_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    (void)indev;
    if (input_profile_get_mode() != INPUT_MODE_TOUCH) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->point.x = static_cast<int32_t>(g_pointer_x);
    data->point.y = static_cast<int32_t>(g_pointer_y);
    data->state = g_pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// Create the LVGL display + buffers for the current g_width/g_height.
void create_display() {
    g_fb.assign(static_cast<size_t>(g_width) * g_height, 0);
    g_draw_buf.assign(static_cast<size_t>(g_width) * g_height * 2u, 0);

    g_disp = lv_display_create(g_width, g_height);
    lv_display_set_flush_cb(g_disp, flush_cb);
    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(g_disp, g_draw_buf.data(), nullptr, g_draw_buf.size(),
                           LV_DISPLAY_RENDER_MODE_FULL);
}

std::string slugify_token(const std::string& in) {
    std::string out;
    for (char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            out.push_back(c);
        } else if (c == ' ' || c == '-') {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "variation";
    return out;
}

}  // namespace

void init(int w, int h) {
    // lv_init() must run before set_display(): set_display() installs the
    // compiled-in OpenSans Western baseline for the translated text roles via
    // lv_tiny_ttf, which needs LVGL's allocator initialized. (set_display() skips
    // the install when LVGL is not yet initialized, so the order matters here.)
    if (!g_initialized) {
        lv_init();
        g_initialized = true;
    }

    set_display(w, h);
    g_width = w;
    g_height = h;

    create_display();

    lv_indev_t* kb = lv_indev_create();
    lv_indev_set_type(kb, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(kb, keypad_read_cb);

    lv_indev_t* ptr = lv_indev_create();
    lv_indev_set_type(ptr, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ptr, pointer_read_cb);
}

void resize(int w, int h) {
    set_display(w, h);
    g_width = w;
    g_height = h;

    // Deleting the display also deletes its screens and detaches its indevs.
    if (g_disp) {
        lv_display_delete(g_disp);
        g_disp = nullptr;
    }

    create_display();

    // lv_display_delete left every indev with a NULL display (silently
    // disabling it). Reattach each to the freshly created display.
    lv_indev_t* indev = nullptr;
    while ((indev = lv_indev_get_next(indev)) != nullptr) {
        lv_indev_set_display(indev, g_disp);
    }
}

int width() { return g_width; }
int height() { return g_height; }

const uint16_t* framebuffer() { return g_fb.data(); }

void tick(uint32_t elapsed_ms) {
    lv_tick_inc(elapsed_ms);

    // Host stand-in for animated QR: push the next frame at ~6 FPS. A no-op for every
    // other screen (empty test_frames); qr_display_set_frame() itself no-ops if the QR
    // screen has since been torn down. Mirrors Python QRDisplayScreen: HOLD while the
    // brightness tip is up (on start + after a brightness change), and RESTART from frame 0
    // when it clears (so the valuable pure first frames are re-delivered).
    if (!g_qr_density_active && !g_qr_test_frames.empty()) {
        bool tip = qr_display_is_tip_active();
        if (tip) {
            g_qr_frame_accum_ms = 0;          // hold on the current (first) frame
            g_qr_tip_was_active = true;
        } else if (g_qr_tip_was_active) {     // tip just cleared -> restart the sequence
            g_qr_tip_was_active = false;
            g_qr_frame_idx = 0;
            g_qr_frame_accum_ms = 0;
            const std::string& f0 = g_qr_test_frames[0];
            qr_display_set_frame(f0.data(), f0.size());
        } else {
            g_qr_frame_accum_ms += elapsed_ms;
            if (g_qr_frame_accum_ms >= QR_FRAME_INTERVAL_MS) {
                g_qr_frame_accum_ms = 0;
                g_qr_frame_idx = (g_qr_frame_idx + 1) % g_qr_test_frames.size();
                const std::string& f = g_qr_test_frames[g_qr_frame_idx];
                qr_display_set_frame(f.data(), f.size());
            }
        }
    }

    lv_timer_handler();
}

void push_key(uint32_t lv_key) {
    g_pending_key = lv_key;
    g_key_ready = true;
}

void set_pointer(int x, int y, bool pressed) {
    g_pointer_x = x;
    g_pointer_y = y;
    g_pointer_pressed = pressed;
}

void scroll_active(int dy_px) {
    lv_obj_t* scr = lv_screen_active();
    if (!scr) return;

    // Find the first scrollable child that actually has vertical overflow (the
    // body content container; the top_nav has no overflow). Clamp to the
    // remaining scroll range so we never overshoot.
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* child = lv_obj_get_child(scr, static_cast<int32_t>(i));
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE) &&
            lv_obj_get_scroll_dir(child) != LV_DIR_NONE &&
            (lv_obj_get_scroll_top(child) > 0 || lv_obj_get_scroll_bottom(child) > 0)) {
            int32_t dy = dy_px;
            if (dy > 0) {
                int32_t avail = lv_obj_get_scroll_top(child);
                if (dy > avail) dy = avail;
            } else if (dy < 0) {
                int32_t avail = lv_obj_get_scroll_bottom(child);
                if (-dy > avail) dy = -avail;
            }
            if (dy != 0) lv_obj_scroll_by(child, 0, dy, LV_ANIM_OFF);
            return;
        }
    }
}

bool has_screen(const std::string& fn_name) {
    return k_screen_registry.find(fn_name) != k_screen_registry.end();
}

bool load_screen(const std::string& fn_name, const std::string& json_ctx) {
    auto it = k_screen_registry.find(fn_name);
    if (it == k_screen_registry.end()) return false;

    // Screens take a JSON C-string or NULL (no context). The screen functions
    // create a fresh root screen and delete the previous one, so re-invoking is
    // a clean re-render — no manual teardown required here.
    const char* ctx = (!json_ctx.empty() && json_ctx != "{}") ? json_ctx.c_str() : nullptr;
    it->second(reinterpret_cast<void*>(const_cast<char*>(ctx)));

    // Arm (or clear) the animated-QR host stand-in for this screen.
    configure_qr_test_frames(fn_name, json_ctx);
    return true;
}

bool load_scenarios_grouped(const char* path, std::vector<ScreenScenarios>& out) {
    out.clear();

    json root;
    {
        FILE* fp = fopen(path, "rb");
        if (!fp) return false;
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) text.append(buf, n);
        fclose(fp);
        try {
            root = json::parse(text);
        } catch (...) {
            return false;
        }
    }
    if (!root.is_object()) return false;

    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string screen_name = it.key();
        const json& screen_def = it.value();
        if (!screen_def.is_object()) return false;

        json base_ctx = json::object();
        if (screen_def.contains("context")) {
            if (!screen_def["context"].is_object()) return false;
            base_ctx = screen_def["context"];
        }

        ScreenScenarios group;
        group.screen = screen_name;
        group.scenarios.push_back({"(default)", base_ctx.dump()});

        if (screen_def.contains("variations")) {
            if (!screen_def["variations"].is_array()) return false;
            for (const auto& var : screen_def["variations"]) {
                if (!var.is_object()) return false;
                std::string var_name = var.value("name", std::string("variation"));
                if (var_name.empty()) var_name = "variation";

                json merged = base_ctx;
                if (var.contains("context")) {
                    if (!var["context"].is_object()) return false;
                    merged.merge_patch(var["context"]);
                }
                group.scenarios.push_back({slugify_token(var_name), merged.dump()});
            }
        }

        out.push_back(std::move(group));
    }

    return !out.empty();
}

}  // namespace runner_core

// Strong override of the qr_display weak host hook: the runner stands in for the host, so moving
// the density slider live re-renders the QR at the new px/module (see apply_density_demo). Real
// hosts (Pi / MicroPython) provide their own strong definition; the desktop/web runner provides
// this one.
extern "C" void seedsigner_lvgl_on_qr_density(uint8_t px_per_module) {
    runner_core::apply_density_demo((int)px_per_module);
}
