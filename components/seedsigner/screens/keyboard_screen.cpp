// keyboard_screen
//
// Python provenance: KeyboardScreen (screen.py)
//
// Generalized single-charset text/char entry: one static keyboard page built
// from cfg-supplied keys writes into a text-entry strip; the final string is
// returned through seedsigner_lvgl_on_text_entered(), the same host hook the
// passphrase screen uses. Consumers: dice-roll / coin-flip entropy, BIP-85
// child index, custom derivation path. Unlike seed_add_passphrase_screen there
// is no page/mode switching, so the keyboard is a plain lv_buttonmatrix built
// on keyboard_core (text-entry box, key theming, joystick row-wrap navigation).
//
// Because the native screen owns the input loop and only returns the final
// string, two things that live in Python's KeyboardScreen move native here:
// the per-keystroke title counter (via `title_keystroke_template`) and the
// `return_after_n_chars` auto-completion.
//
// Layout: absolute positioning inside a non-scrollable scaffold body — the
// text-entry strip across the top, the key grid centered just below it
// (per-key size capped at BUTTON_HEIGHT*2 so sparse grids stay tidy blocks of
// comfortable targets), and the optional guidance legend beneath the grid,
// its height pre-reserved out of the grid's vertical budget. Value keys wrap
// into rows of `cols`; the control keys (optional cursor chevrons, always a
// DELETE/backspace, CHECK when show_save_button) sit on their own row.
// Deviation from Python: Python's Keyboard renders only the value charset and
// puts the save button in a right-hand KEY3 side panel; here every control —
// cursor keys, backspace, save — lives in-grid on the extra control row, so
// both touch and joystick reach them without a side panel.
//
// Lifecycle: stateful (Tier 2) — one heap ctx (new/delete: C++ containers)
// released by keyboard_cleanup_cb on the screen root's LV_EVENT_DELETE.
//
// Navigation: deliberately no bind_screen_navigation — the buttonmatrix owns
// directional input natively (kb_handle_directional row-wrap + top-nav handoff
// to the back button), so hardware mode wires a local group and touch mode
// needs no wiring at all.
//
// cfg:
//   top_nav.title             (string, required)     localized title (read by the
//            scaffold layer); live-replaced by the keystroke counter when
//            title_keystroke_template is set
//   top_nav.show_back_button  (bool, default true)   scaffold layer (Python
//            BaseTopNavScreen default)
//   top_nav.show_power_button (bool, default false)  scaffold layer (Python
//            BaseTopNavScreen default)
//   cols                      (int, default 10, min 1)  value-key grid columns;
//            rows derive from the key count
//   keys                      (array of strings, required, non-empty)  the value
//            keys, one label per cell, row-major
//   keys_to_values            (object, optional)     label -> emitted-value map
//            (e.g. a dice glyph -> "1"); absent => the label IS the value
//   keyboard_font             (string, default "fixed")  key glyph font: "fixed" =
//            KEYBOARD_FONT, "icon"/"fontawesome" = ICON_FONT__SEEDSIGNER
//   return_after_n_chars      (int, default 0 = off) auto-return once the entered
//            text reaches this length. Length is counted in BYTES (std::strlen),
//            which equals characters for the ASCII values all current consumers emit.
//   show_save_button          (bool, default false)  append the in-grid green CHECK
//            confirm key (an empty entry never submits)
//   show_cursor_keys          (bool, default true)   lead the control row with
//            cursor-left / cursor-right chevron keys
//   initial_value             (string, default "")   prefill the text entry
//   title_keystroke_template  (string, default "" = static title)  e.g.
//            "Dice Roll {n}/{total}"; {n} = the index of the entry about to be
//            made, {total} = return_after_n_chars; live-updated on every keystroke
//   max_length                (int, default 0 = no cap)  cap on the entered length,
//            enforced natively by the textarea
//   guidance_text             (string, default "" = none)  localized legend below
//            the keys (e.g. the coin-flip "Heads = 1\nTails = 0"); embedded
//            newlines stack lines
//   input.mode                ("touch" | "hardware", optional)  per-screen input-
//            mode override (shared nav-policy reader); absent -> the global input
//            profile decides
//   allow_screensaver         (bool, default true)   normalized by the parse layer;
//            false stamps the screensaver opt-out flag on the root (scaffold layer)

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / load_screen_and_cleanup_previous
#include "seedsigner.h"       // keyboard_screen decl, screen_scaffold_t fields, seedsigner_lvgl_on_text_entered, seedsigner_lvgl_is_static_render
#include "components.h"       // top_nav_layout_title (live keystroke-counter re-staging)
#include "gui_constants.h"    // KEYBOARD_FONT, ICON_FONT__SEEDSIGNER, BUTTON_HEIGHT, COMPONENT_PADDING, BODY_FONT, BODY_LINE_SPACING, BODY_FONT_COLOR, SeedSignerIconConstants
#include "input_profile.h"    // input_mode_t, input_profile_get_mode
#include "keyboard_core.h"    // kb_make_text_entry, kb_style_matrix, kb_handle_directional, kb_back_down_to_matrix, kb_connect_indevs
#include "screen_helpers.h"   // nav_mode_override_from_cfg, ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_buttonmatrix / lv_textarea / lv_label / lv_group + per-object style setters

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <algorithm>          // std::min (key-size cap), std::count (guidance line count)
#include <cstring>            // std::strcmp / std::strlen (key dispatch, entered length)
#include <map>                // std::map (label -> value mapping)
#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string, std::to_string
#include <vector>             // std::vector (map/ctrl storage)

using json = nlohmann::json;


// Button-matrix control entries for keyboard_screen. KEYBOARD_VALUE_KEY(n): a
// plain value key of relative width n. KEYBOARD_CONTROL_KEY(n): a control key
// (cursor chevrons / DELETE / CHECK) — marked CHECKED so kb_style_matrix paints
// it as a control + the icon draw-cb recolors it, plus NO_REPEAT/CLICK_TRIG so
// a hold doesn't auto-repeat.
#define KEYBOARD_VALUE_KEY(n)   ((lv_buttonmatrix_ctrl_t)(n))
#define KEYBOARD_CONTROL_KEY(n) ((lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG | LV_BUTTONMATRIX_CTRL_CHECKED | (n)))


// ---------------------------------------------------------------------------
// Screen state + callbacks
// ---------------------------------------------------------------------------

namespace {

// Per-screen state. C++ (vectors/strings/map), so new/delete rather than
// lv_malloc; freed in keyboard_cleanup_cb. The text-entry box (c->text_entry)
// is the source of truth for the entered string — control keys edit it at the
// cursor.
struct keyboard_screen_ctx_t {
    lv_obj_t   *matrix = nullptr;
    lv_obj_t   *text_entry = nullptr;
    lv_obj_t   *back_btn = nullptr;
    lv_obj_t   *top_nav = nullptr;       // for re-laying-out the title on each update
    lv_obj_t   *title_label = nullptr;
    bool        title_has_power = false; // power button present (for title centering)
    lv_group_t *group = nullptr;

    std::vector<std::string>            key_storage;  // backs the value-key char*s
    std::vector<const char *>           map;          // buttonmatrix map (persistent)
    std::vector<lv_buttonmatrix_ctrl_t> ctrl;         // one entry per button
    std::map<std::string, std::string>  values;       // label -> emitted value

    std::string title_template;
    int  return_after_n_chars = 0;  // 0 = no auto-return
    int  total = 0;                 // {total} in the title template
};

// Substitute {n} / {total} in a title template.
std::string keyboard_render_title(const std::string &template_text, int entry_number, int total) {
    std::string out = template_text;
    auto replace_token = [&](const char *token, int value) {
        std::string value_text = std::to_string(value);
        size_t position;
        while ((position = out.find(token)) != std::string::npos) out.replace(position, std::strlen(token), value_text);
    };
    replace_token("{n}", entry_number);
    replace_token("{total}", total);
    return out;
}

// Refresh the top-nav title from the template: {n} is the index of the entry
// the user is about to make — entered length + 1, clamped to total. The length
// is std::strlen BYTES, which equals characters for the ASCII values all
// current consumers emit.
void keyboard_update_title(keyboard_screen_ctx_t *c) {
    if (c->title_template.empty() || !c->title_label || !lv_obj_is_valid(c->title_label)) return;
    int entered_length = (c->text_entry && lv_obj_is_valid(c->text_entry)) ? (int)std::strlen(lv_textarea_get_text(c->text_entry)) : 0;
    int entry_number = entered_length + 1;
    if (c->total > 0 && entry_number > c->total) entry_number = c->total;
    lv_label_set_text(c->title_label,
                      keyboard_render_title(c->title_template, entry_number, c->total).c_str());
    // The counter changes width as it grows, so re-run the top-nav title staging
    // (center → left-pin → scroll) against the new text instead of marquee-scrolling
    // within the slice top_nav measured for the initial value.
    top_nav_layout_title(c->top_nav, c->title_label, c->back_btn != nullptr,
                         c->title_has_power, nullptr);
}

// Report the final string to the host — the same seedsigner_lvgl_on_text_entered
// hook the passphrase screen uses.
void keyboard_complete(keyboard_screen_ctx_t *c) {
    if (c->text_entry && lv_obj_is_valid(c->text_entry)) {
        seedsigner_lvgl_on_text_entered(lv_textarea_get_text(c->text_entry));
    }
}

// Buttonmatrix click handler (both input modes). The control glyphs act on the
// text-entry box directly so the cursor position is honored; any other key inserts
// its (possibly mapped) value at the cursor.
void keyboard_value_changed_cb(lv_event_t *e) {
    lv_obj_t *matrix = lv_event_get_target_obj(e);
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    lv_obj_t *text_entry = c->text_entry;
    if (!text_entry || !lv_obj_is_valid(text_entry)) return;

    uint32_t button_id = lv_buttonmatrix_get_selected_button(matrix);
    if (button_id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char *key_text = lv_buttonmatrix_get_button_text(matrix, button_id);
    if (!key_text) return;

    if (std::strcmp(key_text, SeedSignerIconConstants::DELETE) == 0)        { lv_textarea_delete_char(text_entry);  keyboard_update_title(c); return; }
    if (std::strcmp(key_text, SeedSignerIconConstants::CHEVRON_LEFT) == 0)  { lv_textarea_cursor_left(text_entry);  return; }
    if (std::strcmp(key_text, SeedSignerIconConstants::CHEVRON_RIGHT) == 0) { lv_textarea_cursor_right(text_entry); return; }
    if (std::strcmp(key_text, SeedSignerIconConstants::CHECK) == 0) {
        if (std::strlen(lv_textarea_get_text(text_entry)) > 0) keyboard_complete(c);  // don't submit empty (Python parity)
        return;
    }

    // A value key: insert its mapped value (or the label) at the cursor. The
    // textarea enforces max_length natively.
    auto it = c->values.find(key_text);
    lv_textarea_add_text(text_entry, (it != c->values.end()) ? it->second.c_str() : key_text);
    keyboard_update_title(c);

    if (c->return_after_n_chars > 0 && (int)std::strlen(lv_textarea_get_text(text_entry)) >= c->return_after_n_chars) {
        keyboard_complete(c);
    }
}

// Hardware key filter: the generic top-nav handoff + row-wrap (no aux keys — the
// confirm/backspace are in-grid and joystick-navigable).
void keyboard_kb_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    kb_handle_directional(e, c->map.data(), c->matrix, c->back_btn);
}

void keyboard_back_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_KEY) return;
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (c) kb_back_down_to_matrix(e, c->matrix);
}

// LV_EVENT_DELETE teardown on the screen root: drop the joystick group (if any),
// then free the heap ctx.
void keyboard_cleanup_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    keyboard_screen_ctx_t *c = (keyboard_screen_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (c->group) lv_group_del(c->group);
    delete c;
}

}  // namespace


void keyboard_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Input mode selects the joystick group wiring (touch needs none).
    bool has_mode_override = false;
    input_mode_t mode_override = INPUT_MODE_TOUCH;
    nav_mode_override_from_cfg(cfg, has_mode_override, mode_override);
    bool hardware = (has_mode_override ? mode_override : input_profile_get_mode()) == INPUT_MODE_HARDWARE;

    // Structural top-nav defaults (write-if-absent; Python BaseTopNavScreen:
    // show_back_button=True, show_power_button=False) + the required localized
    // title. Title text is CONTENT and must arrive localized from the host view
    // layer; the throw fires before the scaffold exists, so nothing leaks here.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "keyboard_screen");

    // --- Scaffold ---

    // No button_list: the body is a custom textarea + keyboard. upper_body == body.
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, false);
    lv_obj_t *body = screen.body;
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);       // absolute-positioned body: nothing scrolls
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);

    const int32_t content_width  = lv_obj_get_content_width(body);
    const int32_t content_height = lv_obj_get_content_height(body);

    // --- Body ---

    // Tier-2 heap ctx (new/delete — see the struct declaration); released by
    // keyboard_cleanup_cb when the screen root is deleted. The top-nav handles
    // are captured for the live keystroke-counter title re-staging.
    keyboard_screen_ctx_t *c = new keyboard_screen_ctx_t();
    c->back_btn = screen.top_back_btn;
    c->top_nav = screen.top_nav;
    c->title_label = screen.title_label;
    c->title_has_power = (screen.top_power_btn != nullptr);

    // Per-field cfg extraction (defaults per the banner contract). NOTE the
    // preserved statement order: `keys` is validated below, AFTER the scaffold
    // and ctx above already exist — a throw on malformed cfg leaks both.
    int cols = 10;
    if (cfg.contains("cols") && cfg["cols"].is_number_integer()) cols = cfg["cols"].get<int>();
    if (cols < 1) cols = 1;

    if (!cfg.contains("keys") || !cfg["keys"].is_array() || cfg["keys"].empty()) {
        throw std::runtime_error("keyboard_screen: keys is required and must be a non-empty array");
    }
    c->key_storage.reserve(cfg["keys"].size());
    for (const auto &key : cfg["keys"]) {
        if (!key.is_string()) throw std::runtime_error("keyboard_screen: keys entries must be strings");
        c->key_storage.push_back(key.get<std::string>());
    }

    if (cfg.contains("keys_to_values") && cfg["keys_to_values"].is_object()) {
        for (auto it = cfg["keys_to_values"].begin(); it != cfg["keys_to_values"].end(); ++it) {
            if (it.value().is_string()) c->values[it.key()] = it.value().get<std::string>();
        }
    }

    if (cfg.contains("return_after_n_chars") && cfg["return_after_n_chars"].is_number_integer()) {
        c->return_after_n_chars = cfg["return_after_n_chars"].get<int>();
    }
    c->total = c->return_after_n_chars;
    bool show_save_button = cfg.value("show_save_button", false);
    bool show_cursor_keys = cfg.value("show_cursor_keys", true);
    if (cfg.contains("title_keystroke_template") && cfg["title_keystroke_template"].is_string()) {
        c->title_template = cfg["title_keystroke_template"].get<std::string>();
    }
    int max_length = 0;
    if (cfg.contains("max_length") && cfg["max_length"].is_number_integer()) {
        max_length = cfg["max_length"].get<int>();
    }
    std::string initial_value = cfg.value("initial_value", std::string());

    const lv_font_t *key_font = &KEYBOARD_FONT;
    if (cfg.contains("keyboard_font") && cfg["keyboard_font"].is_string()) {
        std::string keyboard_font_name = cfg["keyboard_font"].get<std::string>();
        if (keyboard_font_name == "icon" || keyboard_font_name == "fontawesome") key_font = &ICON_FONT__SEEDSIGNER;
    }

    // 1. Assemble the buttonmatrix map. Value keys wrap by `cols`; the control keys
    //    (cursor left/right, backspace, optional save check) go on their OWN row
    //    beneath — so a sparse value grid (e.g. coin flip) doesn't stretch the
    //    controls across the width. The map char*s reference c->key_storage (fully
    //    populated before any .c_str() is taken, so no realloc invalidates them) and
    //    static icon/literal strings; c->ctrl has one entry per button (no "\n").
    struct cell_t { const char *text; lv_buttonmatrix_ctrl_t ctrl; };
    std::vector<cell_t> controls;
    if (show_cursor_keys) {
        controls.push_back({SeedSignerIconConstants::CHEVRON_LEFT,  KEYBOARD_CONTROL_KEY(1)});
        controls.push_back({SeedSignerIconConstants::CHEVRON_RIGHT, KEYBOARD_CONTROL_KEY(1)});
    }
    controls.push_back({SeedSignerIconConstants::DELETE, KEYBOARD_CONTROL_KEY(1)});       // always a backspace
    if (show_save_button) controls.push_back({SeedSignerIconConstants::CHECK, KEYBOARD_CONTROL_KEY(1)});

    c->map.clear();
    c->ctrl.clear();
    const int value_count = (int)c->key_storage.size();
    int column = 0;
    for (int i = 0; i < value_count; ++i) {
        c->map.push_back(c->key_storage[i].c_str());
        c->ctrl.push_back(KEYBOARD_VALUE_KEY(1));
        if (++column == cols && i + 1 < value_count) { c->map.push_back("\n"); column = 0; }
    }
    c->map.push_back("\n");
    for (const cell_t &control : controls) { c->map.push_back(control.text); c->ctrl.push_back(control.ctrl); }
    c->map.push_back("");

    const int value_rows = (value_count + cols - 1) / cols;
    const int total_rows = value_rows + 1;  // + the control row

    // 2. Text-entry strip: the shared cursor-styled box (the entry's source of truth).
    lv_obj_t *text_entry = kb_make_text_entry(body, content_width, seedsigner_lvgl_is_static_render());
    if (!initial_value.empty()) lv_textarea_set_text(text_entry, initial_value.c_str());
    if (max_length > 0) lv_textarea_set_max_length(text_entry, (uint32_t)max_length);
    lv_textarea_set_cursor_pos(text_entry, LV_TEXTAREA_CURSOR_LAST);
    c->text_entry = text_entry;

    // 3. Keyboard: a plain buttonmatrix styled like the passphrase keyboard.
    lv_obj_t *matrix = lv_buttonmatrix_create(body);
    lv_buttonmatrix_set_map(matrix, c->map.data());
    lv_buttonmatrix_set_ctrl_map(matrix, c->ctrl.data());
    kb_style_matrix(matrix, key_font);
    c->matrix = matrix;

    // 4. Optional guidance text (e.g. the coin-flip "Heads = 1 / Tails = 0" legend),
    //    centered below the keyboard. The caller passes it already translated; embedded
    //    newlines split it into lines (Python renders these as stacked TextAreas). Its
    //    height is reserved from the keyboard's vertical budget below so it never gets
    //    pushed off-screen by the (capped, but still tall) keys.
    std::string guidance_text = cfg.value("guidance_text", std::string());
    int32_t guidance_height = 0;
    if (!guidance_text.empty()) {
        int lines = 1 + (int)std::count(guidance_text.begin(), guidance_text.end(), '\n');
        guidance_height = lines * (int32_t)lv_font_get_line_height(&BODY_FONT)
                          + (lines - 1) * BODY_LINE_SPACING + 2 * COMPONENT_PADDING;
    }

    // 5. Cap the per-key size so a sparse grid stays a tidy block of comfortably large
    //    targets instead of stretching to fill the screen (e.g. the lone backspace on a
    //    coin-flip keyboard). Center the grid horizontally, just below the text entry.
    const int32_t kb_top  = BUTTON_HEIGHT + COMPONENT_PADDING;
    const int32_t available_width  = content_width;
    const int32_t available_height = content_height - kb_top - guidance_height;
    const int32_t max_key_size = BUTTON_HEIGHT * 2;
    int32_t key_width  = std::min(available_width / cols, max_key_size);
    int32_t key_height = std::min(available_height / total_rows, max_key_size);
    lv_obj_set_size(matrix, key_width * cols, key_height * total_rows);
    lv_obj_align(matrix, LV_ALIGN_TOP_MID, 0, kb_top);

    // One click handler serves both touch taps and joystick presses.
    lv_obj_add_event_cb(matrix, keyboard_value_changed_cb, LV_EVENT_VALUE_CHANGED, c);

    // 6. Guidance legend label (its height was already reserved in step 4).
    if (!guidance_text.empty()) {
        lv_obj_t *guidance_label = lv_label_create(body);
        lv_label_set_text(guidance_label, guidance_text.c_str());
        lv_obj_set_width(guidance_label, content_width);
        lv_obj_set_style_text_font(guidance_label, &BODY_FONT, LV_PART_MAIN);
        lv_obj_set_style_text_color(guidance_label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
        lv_obj_set_style_text_align(guidance_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        // Match regular body text's line-to-line spacing for the multi-line legend.
        lv_obj_set_style_text_line_space(guidance_label, BODY_LINE_SPACING, LV_PART_MAIN);
        lv_obj_align(guidance_label, LV_ALIGN_TOP_MID, 0, kb_top + key_height * total_rows + COMPONENT_PADDING);
    }

    // 7. Initial title staging: keyboard_update_title lays out the top-nav title
    //    for the current counter value (subsequent keystrokes keep it correct as
    //    the counter widens). Static titles keep top_nav's own layout.
    keyboard_update_title(c);

    // --- Navigation + load ---

    // No bind_screen_navigation on this screen: the buttonmatrix owns directional
    // input natively (kb_handle_directional row-wrap + top-nav handoff), so
    // hardware mode wires its own group here and touch mode needs no wiring.
    // Slated for extraction at the rollout decision: keyboard-family group wiring shared with the passphrase/mnemonic screens (conformance spec §10, cluster #16).
    if (hardware) {
        // Joystick: the matrix is the group-focused object so its own directional
        // navigation drives key selection; the back button is the other member for
        // the top-nav handoff (mirrors the passphrase screen, sans side panel).
        c->group = lv_group_create();
        lv_group_set_wrap(c->group, false);
        lv_group_add_obj(c->group, matrix);
        if (screen.top_back_btn) {
            lv_group_add_obj(c->group, screen.top_back_btn);
            lv_obj_add_event_cb(matrix, keyboard_kb_key_cb,
                                (lv_event_code_t)(LV_EVENT_KEY | LV_EVENT_PREPROCESS), c);
            lv_obj_add_event_cb(screen.top_back_btn, keyboard_back_key_cb, LV_EVENT_KEY, c);
        }
        lv_group_focus_obj(matrix);
        kb_connect_indevs(c->group);

        // Pre-select the first key so the joystick selection is visible immediately;
        // static-render adds PRESSED so the highlight shows in screenshots.
        lv_buttonmatrix_set_selected_button(matrix, 0);
        if (seedsigner_lvgl_is_static_render()) lv_obj_add_state(matrix, LV_STATE_PRESSED);
    }

    lv_obj_add_event_cb(screen.screen, keyboard_cleanup_cb, LV_EVENT_DELETE, c);
    load_screen_and_cleanup_previous(screen.screen);
}
