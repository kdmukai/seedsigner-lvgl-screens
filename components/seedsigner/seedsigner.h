#ifndef SEEDSIGNER_H
#define SEEDSIGNER_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title;
    bool show_back_button;
    bool show_power_button;
} top_nav_ctx_t;

extern const top_nav_ctx_t TOP_NAV_CTX_DEFAULTS;

typedef struct {
    const char *label;   // tuple[0]: str
    void *value;         // tuple[1]: any (opaque pointer)
} button_list_item_t;

typedef struct {
    top_nav_ctx_t top_nav;
    const button_list_item_t *button_list;
    size_t button_list_len;
} button_list_screen_ctx_t;

void demo_screen(void *ctx);
void button_list_screen(void *ctx);
void lv_seedsigner_screen_close(void);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_H
