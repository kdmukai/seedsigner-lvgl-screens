#ifndef SEEDSIGNER_H
#define SEEDSIGNER_H

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Maximum number of buttons the scaffold tracks for a single screen.
// SeedSigner's longest realistic button list is the Settings menu (~10
// entries) plus headroom for future scrollable lists. 32 keeps the scaffold
// state compact (32 pointers = 256 bytes on a 64-bit host) without imposing
// a practical limit on legitimate UIs.
#define SEEDSIGNER_SCAFFOLD_MAX_BUTTONS 32

// Result of create_top_nav_screen_scaffold().
//
// `screen` and `top_nav` are always populated. `top_back_btn` /
// `top_power_btn` are populated when the corresponding TopNav buttons were
// requested.
//
// `body` is the standard scrollable body container that sits beneath the
// TopNav. When `cfg["button_list"]` is provided, `body` is configured as a
// vertical flex column holding [upper_body, (optional spacer), button(s)].
// Otherwise it is the legacy non-flex container.
//
// `upper_body` is a flex-column container the screen owns: callers add
// content (icon, headline, custom widgets, etc.) here and the scaffold
// guarantees it will be followed by an optional flex-grow spacer and the
// button list. When no button_list is configured, `upper_body == body`.
//
// `button_list_spacer` is non-NULL only when both `cfg["button_list"]` is
// present AND `cfg["is_bottom_list"]` is true: it is a flex-grow=1 child
// inserted between `upper_body` and the first button to pin buttons to the
// viewport bottom when upper content fits.
//
// `button_list[]` / `button_list_count` enumerate the buttons created from
// `cfg["button_list"]`. Both are zero / unset when no button list is
// configured (e.g. main_menu_screen, screensaver_screen).
typedef struct {
    lv_obj_t *screen;
    lv_obj_t *top_nav;
    lv_obj_t *top_back_btn;
    lv_obj_t *top_power_btn;
    lv_obj_t *body;
    lv_obj_t *upper_body;
    lv_obj_t *button_list_spacer;
    size_t    button_list_count;
    lv_obj_t *button_list[SEEDSIGNER_SCAFFOLD_MAX_BUTTONS];
} screen_scaffold_t;

// Screens
void demo_screen(void *ctx);
void button_list_screen(void *ctx_json);
void main_menu_screen(void *ctx);
void screensaver_screen(void *ctx_json);
void large_icon_status_screen(void *ctx_json);

// misc
void lv_seedsigner_screen_close(void);


#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_H
