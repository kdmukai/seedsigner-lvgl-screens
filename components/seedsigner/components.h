#ifndef SEEDSIGNER_COMPONENTS_H
#define SEEDSIGNER_COMPONENTS_H

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

lv_obj_t* top_nav(lv_obj_t* lv_parent, const char *title, bool show_back_button, bool show_power_button, lv_obj_t **out_back_btn, lv_obj_t **out_power_btn, const lv_font_t *title_font = nullptr);

// Configure an already start-justified, width-constrained single-line label to
// continuously marquee-scroll (circular wrap) an overflowing line at a steady
// ~40 px/sec, with two independently-tunable holds:
//   - `begin_hold_ms`: the initial pause before scrolling starts.
//   - `loop_hold_ms`:  the pause each time the line wraps back to the start.
// The title + status headline pass LINE_SCROLL_BEGIN_HOLD_MS for both (a ~1 s beat to
// read the start, then a beat each loop). The touch long-press-to-scroll gesture passes
// begin_hold=0 (immediate — the long-press itself is the pause, and the label clips back
// on release, so an initial hold would hide the motion behind a quick release) but keeps
// loop_hold=LINE_SCROLL_BEGIN_HOLD_MS so it still pauses each time it returns to the
// start. Shaped (hi/th) labels ride the same scroll offset via the glyph-run draw
// (Task 0); RTL (ur) is excluded there for now.
void label_set_line_autoscroll(lv_obj_t* label, uint32_t begin_hold_ms, uint32_t loop_hold_ms);

typedef struct {
    const char *label;
    void *value;
} button_list_item_t;

lv_obj_t* button(lv_obj_t* lv_parent, const char* text, lv_obj_t* align_to);
lv_obj_t* large_icon_button(lv_obj_t* lv_parent, const char* icon, const char* text, lv_obj_t* align_to);
lv_obj_t* button_list(lv_obj_t* lv_parent, const button_list_item_t *items, size_t item_count);

void button_set_active(lv_obj_t* lv_button, bool active);

// Hardware/joystick focus: promote a focused button's too-wide text label to a
// marquee scroll (true) or clip it back to its start edge (false). Driven from the
// nav layer because body buttons are kept out of the LVGL focus group, so LVGL
// never emits the FOCUSED/DEFOCUSED events that would otherwise drive this.
void button_set_label_marquee(lv_obj_t* lv_button, bool marquee);


#endif // SEEDSIGNER_COMPONENTS_H
