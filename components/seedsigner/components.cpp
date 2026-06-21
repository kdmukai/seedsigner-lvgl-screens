#include "components.h"
#include "gui_constants.h"
#include "font_registry.h"
#include "glyph_runs.h"
#include "seedsigner.h"
#include "lvgl.h"

#include <stdint.h>

extern "C" __attribute__((weak)) void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label);

// A zero-duration transition. The default theme attaches a bg-color fade to
// buttons (transition_delayed/normal), so the top-nav buttons' highlight fades
// in/out — unlike the keyboard keys and button-list items, which snap. Apply
// this to make the top-nav highlight instantaneous to match. Initialized lazily.
static const lv_style_transition_dsc_t *instant_transition() {
    static lv_style_transition_dsc_t dsc;
    static bool inited = false;
    if (!inited) {
        static const lv_style_prop_t props[] = { LV_STYLE_BG_COLOR, LV_STYLE_BG_OPA, LV_STYLE_PROP_INV };
        lv_style_transition_dsc_init(&dsc, props, lv_anim_path_linear, 0, 0, NULL);
        inited = true;
    }
    return &dsc;
}

// Reset LVGL's default button chrome (shadow, outline, border) so our
// buttons render as flat colored rectangles with rounded corners.
static void reset_button_chrome(lv_obj_t* btn) {
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, BUTTON_RADIUS, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUS_KEY);
}

// Find the last lv_label child of a widget (skips non-label children like icons).
static lv_obj_t* find_last_label_child(lv_obj_t *parent) {
    lv_obj_t *result = NULL;
    uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            result = child;
        }
    }
    return result;
}

// User-data attached to each top-nav icon button: the reserved code the host
// receives via seedsigner_lvgl_on_button_selected(), plus a short informational
// label (logging / desktop status line only — the host dispatches on the code).
struct top_nav_event_t {
    uint32_t    code;
    const char *label;
};
static const top_nav_event_t TOP_NAV_BACK_EVENT  = { SEEDSIGNER_RET_BACK_BUTTON,  "back"  };
static const top_nav_event_t TOP_NAV_POWER_EVENT = { SEEDSIGNER_RET_POWER_BUTTON, "power" };

static void top_nav_button_event_callback(lv_event_t* e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }
    const top_nav_event_t *event = (const top_nav_event_t *)lv_event_get_user_data(e);
    if (!event) {
        return;
    }
    seedsigner_lvgl_on_button_selected(event->code, event->label);
}

static lv_obj_t* top_nav_icon_button(lv_obj_t* lv_parent, const char* icon, lv_align_t align, int32_t x_ofs, const top_nav_event_t* event) {
    lv_obj_t* btn = lv_button_create(lv_parent);
    lv_obj_set_size(btn, TOP_NAV_BUTTON_SIZE, TOP_NAV_BUTTON_SIZE);
    lv_obj_align(btn, align, x_ofs, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(BUTTON_BACKGROUND_COLOR), LV_PART_MAIN);
    reset_button_chrome(btn);
    lv_obj_set_style_bg_color(btn, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(btn, lv_color_hex(ACCENT_COLOR), LV_PART_MAIN | LV_STATE_PRESSED);
    // Snap the highlight instantly instead of the theme's fade (the keyboard keys
    // and button-list items don't fade). The theme attaches its transition to the
    // default state and to PRESSED, so override both.
    lv_obj_set_style_transition(btn, instant_transition(), LV_PART_MAIN);
    lv_obj_set_style_transition(btn, instant_transition(), LV_PART_MAIN | LV_STATE_PRESSED);

    // Glyph color follows the button state: light normally, black when the button
    // is highlighted (focused / pressed) so it stays legible on the orange
    // highlight. Set on the BUTTON (not the label) so the state selectors apply
    // and the label inherits the color.
    lv_obj_set_style_text_color(btn, lv_color_hex(BUTTON_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_color(btn, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN | LV_STATE_FOCUS_KEY);
    lv_obj_set_style_text_color(btn, lv_color_hex(BUTTON_SELECTED_FONT_COLOR), LV_PART_MAIN | LV_STATE_PRESSED);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, icon ? icon : "");
    lv_obj_set_style_text_font(lbl, &ICON_FONT__SEEDSIGNER, LV_PART_MAIN);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, top_nav_button_event_callback, LV_EVENT_CLICKED, (void*)event);
    return btn;
}

// Configure an already start-justified, width-constrained single-line label to
// auto-scroll its overflowing text: optionally hold start-justified for an initial
// beat (`begin_hold_ms`, so the reader absorbs the screen + the start of the line),
// then continuously marquee-scroll (circular wrap) at a steady ~LINE_SCROLL_PX_PER_SEC,
// holding again (`loop_hold_ms`) each time the line wraps back to the start. The two
// holds are independent: the top-nav title + long status headline pass
// LINE_SCROLL_BEGIN_HOLD_MS (~1 s) for both; the touch long-press-to-scroll gesture
// passes begin_hold=0 (immediate — the long-press IS the pause, and the label clips back
// on release so an initial hold would hide the motion behind a quick release) but keeps
// loop_hold=LINE_SCROLL_BEGIN_HOLD_MS so it still pauses each time it returns to the
// start. The circular wrap reads better than Python's back-and-forth ping-pong; the
// per-loop start hold is the part of Python's feel worth keeping.
//
// Speed: we set an EXPLICIT per-line duration (style anim_duration in ms) rather than
// LVGL's px/sec speed encoding — the encoding caps the resolved duration at ~10 s, so
// a long line (e.g. the 60-char German stress title) would otherwise run noticeably
// faster than the target. CIRCULAR scrolls the whole line plus a WAIT_CHAR gap before
// it wraps, so duration = (text_width + gap) / px_per_sec gives a true constant rate.
// The holds come from a static template anim: the label's circular scroll setup
// (lv_label.c overwrite_anim_property, SCROLL_CIRCULAR) copies our act_time /
// repeat_cnt / repeat_delay out of it —
//   - act_time = -begin_hold_ms   -> the FIRST hold (negative act_time == start delay),
//   - repeat_delay = loop_hold_ms -> the hold each time the loop wraps to the start,
//   - repeat_cnt = INFINITE       -> keep looping (lv_anim_init defaults this to 1!).
// (begin_hold_ms == 0 leaves act_time at 0 -> scrolls immediately; loop_hold_ms is
// independent, so the wrap-to-start pause can stay on.) CIRCULAR has no reverse phase,
// so reverse_delay is not copied / set.
void label_set_line_autoscroll(lv_obj_t* label, uint32_t begin_hold_ms, uint32_t loop_hold_ms) {
    if (!label) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);  // continuous wrap

    // ~40 px/sec at the Pi Zero reference (PX_MULTIPLIER=100), scaled for taller
    // displays so the visual speed (text-widths per second) stays constant.
    uint32_t px_per_sec = (uint32_t)LINE_SCROLL_PX_PER_SEC * active_profile().px_multiplier / 100;
    if (px_per_sec < 1) {
        px_per_sec = 1;
    }

    // Measure the line LVGL will scroll: the stored (presentation-form) text at the
    // label's font + letter spacing, plus the WAIT_CHAR space gap CIRCULAR adds before
    // the wrap — i.e. the same distance the offset animation travels. Set the duration
    // so that distance is covered at px_per_sec (floored so a tiny overflow can't
    // produce a jittery sub-300 ms scroll).
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_point_t line_size = {0, 0};
    lv_text_get_size(&line_size, lv_label_get_text(label), font,
                     lv_obj_get_style_text_letter_space(label, LV_PART_MAIN), 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int32_t scroll_distance = line_size.x +
        lv_font_get_glyph_width(font, ' ', ' ') * LV_LABEL_WAIT_CHAR_COUNT;
    uint32_t duration_ms = (uint32_t)((int64_t)scroll_distance * 1000 / px_per_sec);
    if (duration_ms < (uint32_t)LINE_SCROLL_MIN_MS) {
        duration_ms = (uint32_t)LINE_SCROLL_MIN_MS;
    }
    lv_obj_set_style_anim_duration(label, duration_ms, LV_PART_MAIN);

    // The template is a function-local static, but it carries the CALLER's holds
    // (titles vs touch use different values), so it must be re-initialized each call —
    // the label copies the fields out immediately (it never retains the pointer), so
    // overwriting the shared instance per call is safe.
    static lv_anim_t scroll_feel_template;
    lv_anim_init(&scroll_feel_template);
    lv_anim_set_delay(&scroll_feel_template, begin_hold_ms);   // act_time = -begin hold (0 = none)
    scroll_feel_template.repeat_cnt   = LV_ANIM_REPEAT_INFINITE;  // keep the infinite loop
    scroll_feel_template.repeat_delay = loop_hold_ms;            // hold on each wrap to start
    lv_obj_set_style_anim(label, &scroll_feel_template, LV_PART_MAIN);
}

lv_obj_t* top_nav(lv_obj_t* lv_parent, const char *title, bool show_back_button, bool show_power_button, lv_obj_t **out_back_btn, lv_obj_t **out_power_btn, const lv_font_t *title_font) {

    lv_parent = lv_parent ? lv_parent : lv_scr_act();
    lv_obj_t* lv_top_nav = lv_obj_create(lv_parent);

    // TopNav should be the full horizontal width
    lv_obj_set_size(lv_top_nav, lv_pct(100), TOP_NAV_HEIGHT);
    lv_obj_set_scrollbar_mode(lv_top_nav, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(lv_top_nav, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(lv_top_nav, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_set_style_bg_color(lv_top_nav, lv_color_hex(BACKGROUND_COLOR), LV_PART_MAIN);
    lv_obj_set_style_radius(lv_top_nav, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(lv_top_nav, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(lv_top_nav, 0, LV_PART_MAIN);

    lv_obj_set_style_outline_width(lv_top_nav, 0, LV_PART_MAIN);

    lv_obj_t* back_btn = NULL;
    lv_obj_t* power_btn = NULL;

    if (show_back_button) {
        back_btn = top_nav_icon_button(lv_top_nav, SeedSignerIconConstants::CHEVRON_LEFT, LV_ALIGN_LEFT_MID, EDGE_PADDING, &TOP_NAV_BACK_EVENT);
    }

    if (show_power_button) {
        power_btn = top_nav_icon_button(lv_top_nav, SeedSignerIconConstants::POWER, LV_ALIGN_RIGHT_MID, -EDGE_PADDING, &TOP_NAV_POWER_EVENT);
    }

    const char *label_text = title ? title : "";
    lv_obj_t* label = lv_label_create(lv_top_nav);
    lv_label_set_text(label, label_text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_SCROLL_CIRCULAR);

    lv_obj_set_style_text_color(label, lv_color_hex(BODY_FONT_COLOR), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, title_font ? title_font : &TOP_NAV_TITLE_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_update_layout(lv_top_nav);
    int32_t nav_w = lv_obj_get_content_width(lv_top_nav);
    if (nav_w <= 0) {
        nav_w = lv_obj_get_width(lv_parent);
    }
    if (nav_w <= 0) {
        nav_w = 320;
    }

    // Clip title within the available region between nav buttons.
    // This prevents any title text from appearing to the left of the back button.
    int32_t left_pad = EDGE_PADDING;
    int32_t right_pad = EDGE_PADDING;
    if (back_btn) {
        left_pad += TOP_NAV_BUTTON_SIZE + COMPONENT_PADDING;
    }
    if (power_btn) {
        right_pad += TOP_NAV_BUTTON_SIZE + COMPONENT_PADDING;
    }

    int32_t label_w = nav_w - left_pad - right_pad;
    if (label_w < 16) {
        label_w = 16;
    }

    // Measure the title at its ACTUAL rendered width. With LV_USE_ARABIC_PERSIAN_
    // CHARS, lv_label_set_text rewrites Arabic/Persian into (narrower) presentation
    // forms and stores THAT; the subset fonts carry the presentation forms, not the
    // base codepoints. So measuring the original `label_text` over-counts massively
    // (raw codepoints fall back to missing-glyph boxes) and wrongly tripped the
    // overflow branch, left-aligning RTL titles off-center. Measure the label's
    // stored (shaped) text instead — identical to `label_text` for LTR locales, so
    // their layout is unchanged. Width is direction-independent, so measuring before
    // the RTL base_dir post-pass is fine.
    const lv_font_t *eff_font = title_font ? title_font : &TOP_NAV_TITLE_FONT;
    lv_point_t text_size = {0, 0};
    lv_text_get_size(&text_size, lv_label_get_text(label), eff_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);

    if (text_size.x > label_w) {
        // Overflow case: clip + scroll within the region between the buttons. The
        // title starts start-justified (left here; shaped RTL via the glyph-run draw)
        // then continuously marquee-scrolls with an initial hold + a hold each time it
        // wraps back to the start, at ~40 px/sec (label_set_line_autoscroll; it tunes
        // the bare LONG_SCROLL_CIRCULAR set above). Shaped (hi/th) titles ride the same
        // offset animation now that the glyph-run draw honors label->offset.x (Task 0).
        lv_obj_set_width(label, label_w);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, left_pad, 0);
        label_set_line_autoscroll(label, LINE_SCROLL_BEGIN_HOLD_MS, LINE_SCROLL_BEGIN_HOLD_MS);
    } else {
        // Title fits. Prefer centering on the full nav width (visually centered
        // on screen). But if that would push the text under a side button — the
        // in-between case where the title is too short to scroll yet long enough
        // to intrude given the asymmetric button padding — fall back to centering
        // it within the available region between the buttons so it never overlaps.
        int32_t centered_left = (nav_w - text_size.x) / 2;
        bool full_center_safe = (centered_left >= left_pad) &&
                                (centered_left + text_size.x <= nav_w - right_pad);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        if (full_center_safe) {
            lv_obj_set_width(label, text_size.x);
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        } else {
            lv_obj_set_width(label, label_w);
            lv_obj_align(label, LV_ALIGN_LEFT_MID, left_pad, 0);
        }
    }

    if (out_back_btn) {
        *out_back_btn = back_btn;
    }
    if (out_power_btn) {
        *out_power_btn = power_btn;
    }

    return lv_top_nav;
}



void button_set_active(lv_obj_t* lv_button, bool active) {
    if (!lv_button) {
        return;
    }
    if (active) {
        lv_obj_set_style_bg_color(lv_button, lv_color_hex(ACCENT_COLOR), 0);
    } else {
        lv_obj_set_style_bg_color(lv_button, lv_color_hex(BUTTON_BACKGROUND_COLOR), 0);
    }

    uint32_t child_count = lv_obj_get_child_count(lv_button);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t* child = lv_obj_get_child(lv_button, i);
        if (!child) {
            continue;
        }
        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_obj_set_style_text_color(child, active ? lv_color_hex(BUTTON_SELECTED_FONT_COLOR) : lv_color_hex(BUTTON_FONT_COLOR), 0);
        }
    }
}


// In hardware/joystick mode the focused button's too-wide text label marquee-
// scrolls (LONG_SCROLL_CIRCULAR); when it loses focus it clips back to its START
// edge (LONG_CLIP) so the beginning of the label shows again. Called from the nav
// layer (navigation.cpp update_visual_focus) — body buttons are deliberately kept
// out of the LVGL focus group, so LVGL never emits the FOCUSED/DEFOCUSED that would
// otherwise drive this (and emitting them by hand would add LV_STATE_FOCUSED and
// fight our manual highlight). Touch has no persistent focus, so it never calls in.
//
// Shaped (glyph-run) locales are excluded: their labels are painted by
// glyph_run_draw_cb from a baked alpha mask that ignores the label's scroll offset
// (so a marquee animation wouldn't move the glyphs), AND that draw path start-
// justifies an overflowing run only while the label stays LONG_CLIP. They keep
// LONG_CLIP (start-justified) — no active-scroll. (A14)
void button_set_label_marquee(lv_obj_t* lv_button, bool marquee) {
    if (!lv_button || seedsigner_locale_uses_glyph_runs()) {
        return;
    }
    lv_obj_t* label = find_last_label_child(lv_button);
    if (!label) {
        return;
    }

    // lv_label_set_long_mode unconditionally deletes the scroll animation, resets
    // the offset, and marks a text refresh — so only touch it on an ACTUAL change.
    // update_visual_focus re-asserts every non-focused button on each keypress, so
    // an unguarded call would needlessly re-clip (and redraw) the whole list every
    // step, and re-setting SCROLL on the still-focused button would restart its
    // marquee from the beginning.
    lv_label_long_mode_t want = marquee ? LV_LABEL_LONG_SCROLL_CIRCULAR
                                        : LV_LABEL_LONG_CLIP;
    if (lv_label_get_long_mode(label) != want) {
        lv_label_set_long_mode(label, want);
    }
}


extern "C" __attribute__((weak)) void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label) {
    (void)index;
    (void)label;
}

// --- Touch long-press-to-scroll (Item 3) ------------------------------------------
//
// In TOUCH mode a button has no persistent focus, so an overflowing label can't
// marquee on focus the way it does on hardware (button_set_label_marquee, driven from
// the nav layer). The discovery gesture instead: press-and-HOLD a button to scroll its
// label and read the full text WITHOUT selecting; a short tap still selects. A
// long-press "consumes" the gesture so the release does not select. The hardware path
// synthesizes CLICKED directly (no press cycle) and never reaches the long-press here.

// Whether a button's (single-line) text label is wider than its content box — i.e. it
// clips and would benefit from scrolling. Mirrors the overflow tests the rest of the
// code already makes so the gesture scrolls exactly the labels that clip: the shaped
// path asks the baked glyph run (the codepoint measure mis-counts presentation forms /
// conjuncts), the subset/Latin path measures the stored presentation-form text.
static bool button_label_overflows(lv_obj_t* label) {
    if (!label) {
        return false;
    }
    int32_t content_w = lv_obj_get_content_width(label);
    if (content_w <= 0) {
        return false;
    }

    // Shaped (glyph-run) label: trust the baked run's true typographic width. -1 means
    // no run is attached (a plain codepoint label even within a shaping locale) — fall
    // through to the codepoint measure.
    int run_overflow = seedsigner_label_run_overflows(label);
    if (run_overflow >= 0) {
        return run_overflow > 0;
    }

    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_point_t text_size = {0, 0};
    lv_text_get_size(&text_size, lv_label_get_text(label), font, 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return text_size.x > content_w;
}

// Start a button's text label scrolling for a touch long-press. Returns true only if a
// scroll was actually started (label overflows AND the locale is in scope), so the
// caller marks the press "consumed" and suppresses the release-select — when nothing
// scrolls, a too-long press on a normal button still selects. RTL (fa/ur) is excluded:
// the glyph-run draw's offset/scroll path is LTR-only for now (Task 0), matching the
// hardware marquee and the title/headline auto-scroll. Shaped LTR (hi/th) ride Task 0.
static bool button_start_label_scroll(lv_obj_t* btn) {
    if (!btn || seedsigner_locale_is_rtl()) {
        return false;
    }
    lv_obj_t* label = find_last_label_child(btn);
    if (!label || !button_label_overflows(label)) {
        return false;
    }
    // Start scrolling immediately (begin_hold = 0): the long-press itself is the pause,
    // and the label clips back on release, so an initial hold would hide the motion
    // behind a quick release. Keep the per-wrap hold so it still pauses each time it
    // returns to the start (same beat the title/headline scroll uses).
    label_set_line_autoscroll(label, 0, LINE_SCROLL_BEGIN_HOLD_MS);
    return true;
}

// Restore a button's text label to its at-rest clipped state (start-justified, tail
// clipped) after a touch long-press scroll ends. Guarded against a redundant set:
// lv_label_set_long_mode unconditionally tears down the scroll anim and forces a text
// refresh, so only touch it when the label is actually mid-scroll.
static void button_clip_label(lv_obj_t* btn) {
    if (!btn) {
        return;
    }
    lv_obj_t* label = find_last_label_child(btn);
    if (label && lv_label_get_long_mode(label) != LV_LABEL_LONG_CLIP) {
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    }
}

static lv_obj_t *s_press_btn = NULL;
static lv_point_t s_press_point = {0, 0};
static bool s_press_dragged = false;
// A long-press scrolled this press's label, so its release must NOT select. Reset on
// each new PRESSED; cleared once the suppressed CLICKED (or a lost press) handles it.
static bool s_press_scrolled = false;
void button_toggle_callback(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t* btn = (lv_obj_t *)lv_event_get_target(e);

    lv_indev_t *indev = lv_indev_active();

    if (code == LV_EVENT_PRESSED) {
        s_press_btn = btn;
        s_press_dragged = false;
        s_press_scrolled = false;
        if (indev) {
            lv_indev_get_point(indev, &s_press_point);
        }
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (btn == s_press_btn && indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            int32_t dx = p.x - s_press_point.x;
            int32_t dy = p.y - s_press_point.y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            const int32_t drag_threshold = BUTTON_HEIGHT / 2;
            if (dx > drag_threshold || dy > drag_threshold) {
                s_press_dragged = true;
            }
        }
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        // Touch press held past the long-press threshold: reveal an overflowing label
        // by scrolling it and consume the gesture so the upcoming release does NOT
        // select. Only for touch (the hardware path never presses, so it never gets
        // here), on the still-pressed button, and only if the press hasn't become a
        // drag. button_start_label_scroll returns false (leaving the press unconsumed)
        // when nothing actually scrolls — a fitting label or an RTL locale — so a
        // too-long press on a normal button still selects on release.
        bool is_touch = (indev != NULL && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER);
        if (is_touch && btn == s_press_btn && !s_press_dragged) {
            s_press_scrolled = button_start_label_scroll(btn);
        }
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        // Press ended (finger up). If a long-press had started the label scrolling,
        // clip it back to its start now. RELEASED always precedes the CLICKED that
        // follows (and still fires when the press became a list-scroll, where no
        // CLICKED comes at all), so this is the reliable place to restore the label.
        // s_press_scrolled is left set for CLICKED to suppress the selection.
        if (s_press_scrolled && btn == s_press_btn) {
            button_clip_label(btn);
        }
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        // The press was taken over (e.g. the list began scrolling) or otherwise lost;
        // no CLICKED will follow, so the gesture is over. Clip any long-press scroll
        // back and clear the consumed flag.
        if (s_press_scrolled && btn == s_press_btn) {
            button_clip_label(btn);
        }
        s_press_scrolled = false;
        return;
    }

    if (code != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t* parent = lv_obj_get_parent(btn);

    // Touch-originated clicks must correspond to the same pressed button and
    // must not have been a drag gesture.
    // Hardware-triggered clicks (sent via lv_event_send from nav_key_handler)
    // arrive with a keypad indev active, not a pointer — is_touch is false and
    // these guards are skipped so seedsigner_lvgl_on_button_selected fires.
    bool is_touch = (indev != NULL && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER);
    if (is_touch) {
        if (s_press_btn != btn || s_press_dragged) {
            s_press_btn = NULL;
            s_press_dragged = false;
            s_press_scrolled = false;
            return;
        }
        if (s_press_scrolled) {
            // A long-press already revealed this label by scrolling it; this release is
            // the END of that discovery gesture, not a selection. (RELEASED already
            // clipped the label back.) Swallow the click.
            s_press_btn = NULL;
            s_press_dragged = false;
            s_press_scrolled = false;
            return;
        }
    }

    // Enforce single-select behavior: deactivate all sibling buttons first.
    if (parent) {
        uint32_t child_count = lv_obj_get_child_count(parent);
        for (uint32_t i = 0; i < child_count; ++i) {
            lv_obj_t* child = lv_obj_get_child(parent, i);
            if (!child || child == btn) {
                continue;
            }
            if (lv_obj_check_type(child, &lv_button_class)) {
                button_set_active(child, false);
            }
        }
    }

    button_set_active(btn, true);

    uint32_t selected_index = 0;
    if (parent) {
        uint32_t child_count = lv_obj_get_child_count(parent);
        uint32_t btn_pos = 0;
        for (uint32_t i = 0; i < child_count; ++i) {
            lv_obj_t* child = lv_obj_get_child(parent, i);
            if (!child || !lv_obj_check_type(child, &lv_button_class)) {
                continue;
            }
            if (child == btn) {
                selected_index = btn_pos;
                break;
            }
            btn_pos++;
        }
    }

    // Derive label text from the button's last label child at event time.
    // This avoids dangling pointers when original source strings are temporary.
    const char *label_text = "";
    lv_obj_t *text_child = find_last_label_child(btn);
    if (text_child) {
        const char *t = lv_label_get_text(text_child);
        if (t && t[0] != '\0') label_text = t;
    }
    seedsigner_lvgl_on_button_selected(selected_index, label_text);

    s_press_btn = NULL;
    s_press_dragged = false;
    s_press_scrolled = false;
}


// Size a button's text label to the button's content box and pick the label's
// text alignment from whether the (single-line) text fits:
//   fits      -> centered (visual parity with the Python screens)
//   too wide  -> START edge (LTR: left, RTL: right) so the BEGINNING of the label
//                shows instead of LONG_CLIP clipping to its middle.
//
// Re-runs on every resize (registered as an LV_EVENT_SIZE_CHANGED handler) so a
// button whose final width is set AFTER creation gets correct label geometry.
// The driving case is the main-menu 2x2 grid: large_icon_button() builds a
// full-body-width label via button(), then main_menu_screen resizes the button
// to a half-width grid cell — leaving the label stale-wide so overflow was never
// detected and the centered label clipped to its middle (the Persian "Seeds"
// symptom). Fixing the width here re-evaluates that automatically.
//
// Shaped (glyph-run) locales are NOT measured here: lv_text_get_size over their
// codepoint text mis-counts the on-screen presentation forms / conjuncts. Their
// alignment is decided later in glyph_run_draw_cb, which knows the run's true
// advance and start-justifies an overflowing run there. So we only fix the label
// WIDTH for them and leave the alignment CENTER.
static void apply_button_label_layout(lv_obj_t* btn) {
    lv_obj_t* label = find_last_label_child(btn);
    if (!label) return;

    // Give the label the button's FULL content box. The button already carries the
    // theme's horizontal padding (text off the rounded corners), so no extra inset
    // is needed — and an extra inset only shows up as oversized side margins once a
    // too-wide label is start-justified (it's invisible while centered). Using the
    // full width also stops a snug label (e.g. the narrow main-menu grid's
    // "Settings") from being falsely treated as overflowing and clipped.
    int32_t available_w = lv_obj_get_content_width(btn);
    if (available_w < 0) available_w = 0;
    lv_obj_set_width(label, available_w);

    // Default to centered (parity with the Python screens). Unshaped locales then
    // measure the text here and flip to the START edge when it overflows; shaped
    // (glyph-run) locales stay CENTER and let glyph_run_draw_cb start-justify an
    // overflowing run — lv_text_get_size over their codepoint text mis-counts the
    // on-screen presentation forms / conjuncts, so the run's true advance (known
    // only in the draw pass) is the right measure for them.
    lv_text_align_t align = LV_TEXT_ALIGN_CENTER;
    if (!seedsigner_locale_uses_glyph_runs()) {
        // Measure the label's STORED text — with LV_USE_ARABIC_PERSIAN_CHARS,
        // lv_label_set_text rewrites Arabic/Persian into (narrower) presentation
        // forms and stores THAT; the subset fonts carry those forms, not the base
        // codepoints. Measuring the original argument would over-count and falsely
        // trip overflow (same rationale as top_nav()'s A4 fix). Width is
        // direction-independent.
        const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
        lv_point_t text_size = {0, 0};
        lv_text_get_size(&text_size, lv_label_get_text(label), font, 0, 0,
                         LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (text_size.x > available_w) {
            align = seedsigner_locale_is_rtl() ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
        }
    }
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
}

static void button_size_changed_cb(lv_event_t* e) {
    lv_obj_t* btn = lv_event_get_target_obj(e);
    // Only the main-menu grid (large_icon_button, a FLEX column) needs its label
    // re-fixed on resize: button() builds the label at the full body width, then
    // main_menu_screen shrinks the button to an exact half-width cell. Plain
    // buttons are left exactly as created — screens that resize them (e.g. the
    // status screen, which insets its buttons to body_w - 2*EDGE_PADDING) already
    // size to their final width, and re-fixing here would double-inset and wrongly
    // clip the label. The grid is sized to an EXACT width (no inset), so re-fixing
    // it converges on the right geometry.
    if (lv_obj_get_style_layout(btn, LV_PART_MAIN) == LV_LAYOUT_FLEX) {
        apply_button_label_layout(btn);
    }
}


lv_obj_t* button(lv_obj_t* lv_parent, const char* text, lv_obj_t* align_to) {
    lv_obj_t* lv_button = lv_button_create(lv_parent);
    lv_obj_set_size(lv_button, lv_obj_get_content_width(lv_parent), BUTTON_HEIGHT);

    if (align_to != NULL) {
        // Align to the outside bottom of the provided object
        lv_obj_align_to(lv_button, align_to, LV_ALIGN_OUT_BOTTOM_MID, 0, LIST_ITEM_PADDING);
    } else {
        // Align to the inside top of the parent container
        lv_obj_align_to(lv_button, lv_parent, LV_ALIGN_TOP_MID, 0, 0);
    }

    reset_button_chrome(lv_button);

    // Use COMPONENT_PADDING for the label's side margin instead of the LVGL theme's
    // default button pad_hor (PAD_DEF, ~13px at 240 — wider than our 8px rhythm).
    // Invisible while a label is centered (the content box stays symmetric, so
    // centered text is byte-identical), this gives a start-justified too-wide label
    // a consistent COMPONENT_PADDING gutter on both the plain and grid buttons.
    lv_obj_set_style_pad_hor(lv_button, COMPONENT_PADDING, LV_PART_MAIN);

    lv_obj_t* label = lv_label_create(lv_button);
    lv_obj_set_style_text_font(label, &BUTTON_FONT, LV_PART_MAIN);

    // Labels are STATIC (LONG_CLIP) at rest — a too-wide label start-justifies and
    // clips its tail rather than marquee-scrolling. On hardware the nav layer
    // promotes the focused button's label to a marquee scroll via
    // button_set_label_marquee() (driven from update_visual_focus, since body
    // buttons are kept out of the LVGL focus group). Touch has no persistent focus,
    // so its labels stay clipped.
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);

    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    // Size the label to the button's content box and choose its text alignment
    // (centered if it fits, start-justified if too wide). Re-run on every resize so
    // buttons resized after creation (the main-menu grid) fix their label geometry.
    apply_button_label_layout(lv_button);
    lv_obj_add_event_cb(lv_button, button_size_changed_cb, LV_EVENT_SIZE_CHANGED, NULL);

    // Wire up gesture-aware input callback. LONG_PRESSED drives the touch
    // long-press-to-scroll reveal; PRESS_LOST restores the label if the press is taken
    // over (e.g. the list scrolls) without a CLICKED.
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(lv_button, button_toggle_callback, LV_EVENT_PRESS_LOST, NULL);

    // Default to inactive state
    button_set_active(lv_button, false);

    return lv_button;
}



lv_obj_t* large_icon_button(lv_obj_t* lv_parent, const char* icon, const char* text, lv_obj_t* align_to) {
    // Start from a regular button (handles styling, label, events, scroll behavior).
    lv_obj_t* lv_button = button(lv_parent, text, align_to);

    // Override the text label font: at 240px, large buttons use 20px (matching the
    // Python LargeButtonScreen's get_button_font_size() + 2). At larger displays
    // this resolves to the regular button font — no change needed there.
    lv_obj_t* text_label = lv_obj_get_child(lv_button, 0);
    lv_obj_set_style_text_font(text_label, &LARGE_BUTTON_FONT, LV_PART_MAIN);

    // Vertical flex layout: icon above text, the block VERTICALLY CENTERED in the
    // button. Plain centering would float the icon up because the text font's empty
    // descent (no menu label has descenders) sits below the visible text and biases
    // the box's geometric center below its visual center. Compensate by reserving a
    // top pad equal to that descent (the text font's base_line): centering the block
    // within [base_line .. height] then leaves EQUAL visible space above the icon ink
    // and below the text ink at every display size (240/320 stay tight like Python's
    // MainMenuView.png; 480/800 no longer float high in their taller buttons).
    lv_obj_set_layout(lv_button, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(lv_button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lv_button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(lv_button, LARGE_BUTTON_FONT.base_line, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(lv_button, 0, LV_PART_MAIN);

    // Insert the icon label before the text label (which button() already created).
    lv_obj_t* icon_label = lv_label_create(lv_button);
    lv_obj_set_style_text_font(icon_label, &ICON_LARGE_BUTTON_FONT__SEEDSIGNER, LV_PART_MAIN);
    // Strip the icon label's default box padding so its box equals the icon font's
    // line_height (base_line=0, glyph fills the box). The icon ink then anchors the
    // gaps directly instead of a padded box inflating them — the same cap-height
    // technique the status-screen hero icon uses.
    lv_obj_set_style_pad_all(icon_label, 0, LV_PART_MAIN);
    lv_label_set_text(icon_label, icon ? icon : "");
    lv_obj_move_to_index(icon_label, 0);

    // Icon->text gap: target a VISUAL COMPONENT_PADDING (ink-to-ink), like Python.
    // LVGL anchors the text box by the font ascent, which carries empty leading above
    // the caps; subtract that leading from the flex row gap so the visible label sits
    // a true COMPONENT_PADDING below the icon (not COMPONENT_PADDING + leading).
    int32_t leading = text_top_leading(&LARGE_BUTTON_FONT, lv_label_get_text(text_label));
    int32_t row_gap = COMPONENT_PADDING - leading;
    if (row_gap < 0) row_gap = 0;
    lv_obj_set_style_pad_row(lv_button, row_gap, LV_PART_MAIN);

    return lv_button;
}

lv_obj_t* button_list(lv_obj_t* lv_parent, const button_list_item_t *items, size_t item_count) {
    lv_obj_t* last_button = NULL;
    lv_obj_t* first_button = NULL;
    if (!lv_parent || !items || item_count == 0) {
        return last_button;
    }

    for (size_t i = 0; i < item_count; ++i) {
        const char *label = items[i].label ? items[i].label : "";
        last_button = button(lv_parent, label, last_button);
        if (i == 0) {
            first_button = last_button;
        }
    }

    // Default behavior:
    // - no button highlighted when there are multiple buttons
    // - highlight the only button when there is exactly one
    if (item_count == 1 && first_button) {
        button_set_active(first_button, true);
    } else {
        uint32_t child_count = lv_obj_get_child_count(lv_parent);
        for (uint32_t i = 0; i < child_count; ++i) {
            lv_obj_t* child = lv_obj_get_child(lv_parent, i);
            if (child && lv_obj_check_type(child, &lv_button_class)) {
                button_set_active(child, false);
            }
        }
    }

    return last_button;
}

