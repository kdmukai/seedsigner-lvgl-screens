#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#else
#error "SDL header not found (expected SDL2/SDL.h or SDL.h)"
#endif

#if __has_include(<SDL2/SDL_ttf.h>)
#include <SDL2/SDL_ttf.h>
#elif __has_include(<SDL_ttf.h>)
#include <SDL_ttf.h>
#else
#error "SDL_ttf not found"
#endif

#include <nlohmann/json.hpp>

#include "lvgl.h"
#include "seedsigner.h"
#include "input_profile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::ordered_json;

typedef void (*screen_fn_t)(void *ctx_json);

static const std::unordered_map<std::string, screen_fn_t> k_screen_registry = {
    {"main_menu_screen", main_menu_screen},
    {"button_list_screen", button_list_screen},
    {"screensaver_screen", screensaver_screen},
};

struct scenario_def_t {
    std::string name;
    std::string screen;
    json context = json::object();
};

// ---------------------------------------------------------------------------
// Dimensions (logical pixels; HiDPI textures are rendered at dpi_scale × these)
// ---------------------------------------------------------------------------

static int g_width = 480;
static int g_height = 320;
static int g_viewport_pad  = 12;
static int g_chrome_w      = 220;
static int g_chrome_gap    = 8;
static int g_title_bar_h   = 48;
static int g_title_gap     = 6;
static int g_label_strip_h = 32;
static int g_label_gap     = 4;
static int g_status_bar_h  = 32;
static int g_status_gap    = 4;

static std::vector<lv_color_t> g_fb;

// ---------------------------------------------------------------------------
// Keypad indev state
// ---------------------------------------------------------------------------

static uint32_t g_pending_key = 0;
static bool g_key_ready = false;

// ---------------------------------------------------------------------------
// Pointer indev state (TOUCH/Mouse mode)
// ---------------------------------------------------------------------------

static int  g_pointer_x       = 0;
static int  g_pointer_y       = 0;
static bool g_pointer_pressed = false;

// ---------------------------------------------------------------------------
// Scenario list (global for load_scenario helper)
// ---------------------------------------------------------------------------

static std::vector<scenario_def_t> g_scenarios;
static size_t g_scenario_idx = 0;

// ---------------------------------------------------------------------------
// Chrome sidebar row data
// ---------------------------------------------------------------------------

struct chrome_row_t {
    bool is_base;
    size_t scenario_idx;
    SDL_Texture *label_tex;
    int label_w, label_h;   // physical (HiDPI) pixel dimensions of the texture
    int indent;             // logical indent (0 for base, 12 for variation)
};

static std::vector<chrome_row_t> g_chrome_rows;
static int g_chrome_row_h  = 28;  // logical px per row
static int g_chrome_scroll_y = 0;

// Label strip (above viewport)
static SDL_Texture *g_label_strip_tex = NULL;
static int g_label_strip_tex_w = 0;

// Title bar textures
static SDL_Texture *g_title_brand_tex  = NULL;  // "SeedSigner" in orange (fallback when no logo)
static int g_title_brand_w = 0;
static SDL_Texture *g_title_rest_tex   = NULL;  // " LVGL Screen Runner"
static int g_title_rest_w = 0;
static int g_title_tex_h = 0;

// Title bar logo (loaded from screen_runner_logo.bmp at startup)
static SDL_Texture *g_logo_tex = NULL;
static int g_logo_w = 0, g_logo_h = 0;

// Title-bar mode badge (clickable, right-aligned)
static SDL_Texture *g_mode_label_tex  = NULL;  // "Keyboard" or "Mouse" — rebuilt on toggle
static int          g_mode_label_w    = 0;
static SDL_Texture *g_mode_prefix_tex = NULL;  // static "Input mode:" label
static int          g_mode_prefix_w   = 0;
static SDL_Rect     g_mode_badge_rect = {};    // logical rect of badge; hit-tested on click

// Status bar (below viewport)
static SDL_Texture *g_status_tex = NULL;
static int g_status_tex_w = 0;
static std::string g_status_text;
static uint32_t g_status_set_ms = 0;  // SDL tick when current status was set

// SDL_ttf fonts.  Loaded at physical pixels (dpi_scale * visual size) and
// blitted at visual size so Retina/HiDPI displays get crisp text.
static TTF_Font *g_font_sm     = NULL;  // sidebar variation rows
static TTF_Font *g_font_md     = NULL;  // sidebar base rows
static TTF_Font *g_font_label  = NULL;  // label strip above viewport
static TTF_Font *g_font_status = NULL;  // status bar below viewport
static TTF_Font *g_font_title  = NULL;  // title bar
static float g_dpi_scale = 1.0f;

// SDL rects set in main() after window + renderer creation.
static SDL_Rect g_title_rect;
static SDL_Rect g_chrome_rect;
static SDL_Rect g_label_rect;
static SDL_Rect g_viewport_rect;
static SDL_Rect g_status_rect;


// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

// Convert a snake_case slug to "Title Case With Spaces".
static std::string title_case(const std::string &slug) {
    std::string result;
    bool cap_next = true;
    for (char c : slug) {
        if (c == '_') {
            result += ' ';
            cap_next = true;
        } else if (cap_next) {
            result += (char)toupper((unsigned char)c);
            cap_next = false;
        } else {
            result += c;
        }
    }
    return result;
}

// Format a scenario for display in the label strip:
//   base scenario  → "Button List Screen"
//   variation      → "Button List Screen  /  scroll_many"
static std::string format_scenario_label(const scenario_def_t &sc) {
    std::string screen_display = title_case(sc.screen);
    if (sc.name == sc.screen) return screen_display;
    size_t pos = sc.name.find("__");
    std::string slug = (pos != std::string::npos) ? sc.name.substr(pos + 2) : sc.name;
    return screen_display + "  /  " + slug;
}

// Build one SDL_Texture from a text string.
// Physical dimensions are stored in *out_w / *out_h.
// Caller must SDL_DestroyTexture when done.
static SDL_Texture *make_text_tex(SDL_Renderer *renderer, TTF_Font *font,
                                  const char *text, SDL_Color color,
                                  int *out_w, int *out_h) {
    if (!font || !text || !text[0]) return NULL;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return NULL;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (out_w) *out_w = surf->w;
    if (out_h) *out_h = surf->h;
    SDL_FreeSurface(surf);
    return tex;
}

// Blit a text texture with HiDPI correction: physical dimensions are
// divided by g_dpi_scale to get logical pixel dimensions for the destination rect.
static void blit_text(SDL_Renderer *renderer, SDL_Texture *tex,
                      int logical_x, int logical_y, int phys_w, int phys_h) {
    if (!tex) return;
    SDL_Rect dst = {
        logical_x,
        logical_y,
        (int)(phys_w / g_dpi_scale),
        (int)(phys_h / g_dpi_scale)
    };
    SDL_RenderCopy(renderer, tex, NULL, &dst);
}


// ---------------------------------------------------------------------------
// Button selection callback — updates SDL status bar (no LVGL toast)
// ---------------------------------------------------------------------------

extern "C" void seedsigner_lvgl_on_button_selected(uint32_t index, const char *label) {
    if (index == 0xFFFFFFFFu) {
        SDL_Log("[nav] top-nav: %s", label ? label : "");
        g_status_text = std::string("TOP  |  ") + (label ? label : "");
    } else {
        SDL_Log("[nav] body[%u]: %s", (unsigned)index, label ? label : "");
        g_status_text = std::string("BODY  |  ") + (label ? label : "");
    }
    // g_status_tex is rebuilt in draw_status_bar on next frame.
    if (g_status_tex) {
        SDL_DestroyTexture(g_status_tex);
        g_status_tex = NULL;
    }
    g_status_set_ms = SDL_GetTicks();
}


// ---------------------------------------------------------------------------
// Chrome sidebar
// ---------------------------------------------------------------------------

static void build_chrome_rows(SDL_Renderer *renderer) {
    for (auto &r : g_chrome_rows) {
        if (r.label_tex) SDL_DestroyTexture(r.label_tex);
    }
    g_chrome_rows.clear();
    if (!g_font_sm || !g_font_md) return;

    for (size_t i = 0; i < g_scenarios.size(); ++i) {
        const auto &sc = g_scenarios[i];
        bool is_base = (sc.name == sc.screen);

        std::string label;
        if (is_base) {
            label = title_case(sc.screen);
        } else {
            size_t pos = sc.name.find("__");
            label = (pos != std::string::npos) ? sc.name.substr(pos + 2) : sc.name;
        }

        chrome_row_t row;
        row.is_base      = is_base;
        row.scenario_idx = i;
        row.indent       = is_base ? 0 : 12;

        TTF_Font *font = is_base ? g_font_md : g_font_sm;
        SDL_Color color = is_base
            ? SDL_Color{0xe8, 0xe8, 0xe8, 0xFF}
            : SDL_Color{0xc4, 0xc4, 0xc4, 0xFF};

        row.label_tex = make_text_tex(renderer, font, label.c_str(), color,
                                      &row.label_w, &row.label_h);
        g_chrome_rows.push_back(row);
    }
}

static void update_label_strip(SDL_Renderer *renderer, const scenario_def_t &sc) {
    if (g_label_strip_tex) { SDL_DestroyTexture(g_label_strip_tex); g_label_strip_tex = NULL; }
    g_label_strip_tex_w = 0;
    if (!g_font_label) return;
    std::string display = format_scenario_label(sc);
    SDL_Color color = {0xe8, 0xe8, 0xe8, 0xFF};
    int tex_h = 0;
    g_label_strip_tex = make_text_tex(renderer, g_font_label, display.c_str(), color,
                                      &g_label_strip_tex_w, &tex_h);
    (void)tex_h;
}

static void chrome_scroll_clamp() {
    int total_h = (int)g_chrome_rows.size() * g_chrome_row_h;
    int max_scroll = std::max(0, total_h - g_chrome_rect.h);
    g_chrome_scroll_y = std::max(0, std::min(g_chrome_scroll_y, max_scroll));
}

static int chrome_row_at_y(int y_in_chrome) {
    int adjusted = y_in_chrome + g_chrome_scroll_y;
    if (adjusted < 0) return -1;
    int idx = adjusted / g_chrome_row_h;
    if (idx < 0 || idx >= (int)g_chrome_rows.size()) return -1;
    return idx;
}

static int find_chrome_row(size_t scenario_idx) {
    for (size_t i = 0; i < g_chrome_rows.size(); ++i)
        if (g_chrome_rows[i].scenario_idx == scenario_idx) return (int)i;
    return -1;
}

static void draw_chrome(SDL_Renderer *renderer) {
    SDL_SetRenderDrawColor(renderer, 0x1a, 0x1a, 0x1a, 0xFF);
    SDL_RenderFillRect(renderer, &g_chrome_rect);

    SDL_RenderSetClipRect(renderer, &g_chrome_rect);

    for (int i = 0; i < (int)g_chrome_rows.size(); ++i) {
        const chrome_row_t &row = g_chrome_rows[i];
        int row_y = g_chrome_rect.y + i * g_chrome_row_h - g_chrome_scroll_y;

        if (row_y + g_chrome_row_h <= g_chrome_rect.y) continue;
        if (row_y >= g_chrome_rect.y + g_chrome_rect.h) break;

        bool is_active = (row.scenario_idx == g_scenario_idx);

        SDL_Rect row_rect = {g_chrome_rect.x, row_y, g_chrome_rect.w, g_chrome_row_h};
        if (is_active) {
            // Muted slate-blue selection: visible but not competing with orange viewport buttons.
            SDL_SetRenderDrawColor(renderer, 0x3d, 0x6b, 0x8c, 0xFF);
        } else if (row.is_base) {
            SDL_SetRenderDrawColor(renderer, 0x28, 0x28, 0x28, 0xFF);
        } else {
            SDL_SetRenderDrawColor(renderer, 0x22, 0x22, 0x22, 0xFF);
        }
        SDL_RenderFillRect(renderer, &row_rect);

        if (row.label_tex) {
            // Active rows: modulate to bright white for best contrast on the muted blue bg.
            SDL_SetTextureColorMod(row.label_tex, 0xFF, 0xFF, 0xFF);
            int log_h = (int)(row.label_h / g_dpi_scale);
            int text_y = row_y + (g_chrome_row_h - log_h) / 2;
            blit_text(renderer, row.label_tex,
                      g_chrome_rect.x + row.indent + 6, text_y,
                      row.label_w, row.label_h);
        }
    }

    SDL_RenderSetClipRect(renderer, NULL);
}

static void draw_label_strip(SDL_Renderer *renderer) {
    SDL_SetRenderDrawColor(renderer, 0x14, 0x14, 0x14, 0xFF);
    SDL_RenderFillRect(renderer, &g_label_rect);
    if (!g_label_strip_tex) return;

    int log_h = (int)(g_label_strip_h * g_dpi_scale / g_dpi_scale);  // = g_label_strip_h
    int text_y = g_label_rect.y + (g_label_strip_h - (int)(g_label_strip_h * 0.65f)) / 2;
    // Center vertically: use actual texture logical height
    int tex_log_h = (int)(g_label_strip_h * 0.7f);
    text_y = g_label_rect.y + (g_label_strip_h - tex_log_h) / 2;
    (void)log_h;

    int w = g_label_strip_tex_w;
    int max_w = (int)(g_label_rect.w * g_dpi_scale);
    if (w > max_w) w = max_w;

    // Use the actual texture height for proper vertical centering.
    int tex_phys_h = 0;
    {
        int tw = 0, th = 0;
        SDL_QueryTexture(g_label_strip_tex, NULL, NULL, &tw, &th);
        tex_phys_h = th;
        (void)tw;
    }
    text_y = g_label_rect.y + (g_label_strip_h - (int)(tex_phys_h / g_dpi_scale)) / 2;
    blit_text(renderer, g_label_strip_tex, g_label_rect.x + 6, text_y, w, tex_phys_h);
}

static void rebuild_mode_label(SDL_Renderer *renderer) {
    if (g_mode_label_tex) { SDL_DestroyTexture(g_mode_label_tex); g_mode_label_tex = NULL; }
    if (!g_font_sm) return;
    const char *label = (input_profile_get_mode() == INPUT_MODE_TOUCH) ? "Mouse" : "Keyboard";
    SDL_Color white = {0xe8, 0xe8, 0xe8, 0xFF};
    int h = 0;
    g_mode_label_tex = make_text_tex(renderer, g_font_sm, label, white, &g_mode_label_w, &h);
    (void)h;
}

// Forward declaration (load_scenario defined below).
static void load_scenario(SDL_Renderer *renderer, size_t idx);

static void toggle_input_mode(SDL_Renderer *renderer) {
    input_mode_t next = (input_profile_get_mode() == INPUT_MODE_HARDWARE)
                        ? INPUT_MODE_TOUCH : INPUT_MODE_HARDWARE;
    input_profile_set_mode(next);
    g_pointer_pressed = false;
    g_key_ready       = false;
    load_scenario(renderer, g_scenario_idx);
    rebuild_mode_label(renderer);
    g_status_text = (next == INPUT_MODE_TOUCH) ? "Mouse mode" : "Keyboard mode";
    if (g_status_tex) { SDL_DestroyTexture(g_status_tex); g_status_tex = NULL; }
    g_status_set_ms = SDL_GetTicks();
}

static void draw_title_bar(SDL_Renderer *renderer) {
    SDL_SetRenderDrawColor(renderer, 0x0e, 0x0e, 0x0e, 0xFF);
    SDL_RenderFillRect(renderer, &g_title_rect);

    if (!g_title_brand_tex && !g_title_rest_tex && !g_logo_tex) return;

    int cur_x = g_title_rect.x;

    if (g_logo_tex) {
        // Scale logo to a fixed logical height, preserving aspect ratio.
        // BMP is generated at 2x physical for Retina crispness; we specify
        // logical dimensions directly (do NOT divide by g_dpi_scale here).
        int log_h = g_title_rect.h - 8;  // 40px logical in a 48px title bar
        int log_w = (g_logo_h > 0) ? (int)((float)g_logo_w * log_h / (float)g_logo_h) : log_h;
        int logo_y = g_title_rect.y + (g_title_rect.h - log_h) / 2;
        SDL_Rect logo_dst = {cur_x, logo_y, log_w, log_h};
        SDL_RenderCopy(renderer, g_logo_tex, NULL, &logo_dst);
        cur_x += log_w + 8;
    } else if (g_title_brand_tex) {
        int text_y = g_title_rect.y + (g_title_rect.h - (int)(g_title_tex_h / g_dpi_scale)) / 2;
        blit_text(renderer, g_title_brand_tex, cur_x, text_y, g_title_brand_w, g_title_tex_h);
        cur_x += (int)(g_title_brand_w / g_dpi_scale) + 1;
    }

    if (g_title_rest_tex) {
        int text_y = g_title_rect.y + (g_title_rect.h - (int)(g_title_tex_h / g_dpi_scale)) / 2;
        blit_text(renderer, g_title_rest_tex, cur_x, text_y, g_title_rest_w, g_title_tex_h);
    }

    // Right-aligned mode badge: "Input mode:  [Keyboard]" or "Input mode:  [Mouse]"
    if (g_mode_label_tex) {
        const int PAD_X = 10, PAD_Y = 4;

        int label_phys_h = 0;
        { int tw = 0; SDL_QueryTexture(g_mode_label_tex, NULL, NULL, &tw, &label_phys_h); }
        int log_label_w = (int)(g_mode_label_w / g_dpi_scale);
        int log_label_h = (int)(label_phys_h / g_dpi_scale);
        int badge_w = log_label_w + PAD_X * 2;
        int badge_h = log_label_h + PAD_Y * 2;
        int badge_x = g_title_rect.x + g_title_rect.w - badge_w - 8;
        int badge_y = g_title_rect.y + (g_title_rect.h - badge_h) / 2;
        g_mode_badge_rect = {badge_x, badge_y, badge_w, badge_h};

        // "Input mode:" prefix to the left of the badge
        if (g_mode_prefix_tex) {
            int prefix_phys_h = 0;
            { int tw = 0; SDL_QueryTexture(g_mode_prefix_tex, NULL, NULL, &tw, &prefix_phys_h); }
            int log_prefix_w = (int)(g_mode_prefix_w / g_dpi_scale);
            int log_prefix_h = (int)(prefix_phys_h / g_dpi_scale);
            int prefix_x = badge_x - log_prefix_w - 6;
            int prefix_y = g_title_rect.y + (g_title_rect.h - log_prefix_h) / 2;
            blit_text(renderer, g_mode_prefix_tex, prefix_x, prefix_y,
                      g_mode_prefix_w, prefix_phys_h);
        }

        // Colored badge fill + border
        bool is_touch = (input_profile_get_mode() == INPUT_MODE_TOUCH);
        SDL_SetRenderDrawColor(renderer,
            is_touch ? 0xc8 : 0x3d,
            is_touch ? 0x6f : 0x6b,
            is_touch ? 0x00 : 0x8c, 0xFF);
        SDL_RenderFillRect(renderer, &g_mode_badge_rect);
        SDL_SetRenderDrawColor(renderer, 0x80, 0x80, 0x80, 0xFF);
        SDL_RenderDrawRect(renderer, &g_mode_badge_rect);

        // Mode text centered in badge
        blit_text(renderer, g_mode_label_tex,
                  badge_x + PAD_X, badge_y + PAD_Y,
                  g_mode_label_w, label_phys_h);
    }
}

static void draw_status_bar(SDL_Renderer *renderer) {
    SDL_SetRenderDrawColor(renderer, 0x14, 0x14, 0x14, 0xFF);
    SDL_RenderFillRect(renderer, &g_status_rect);

    // Rebuild texture if text changed since last frame.
    if (!g_status_tex && !g_status_text.empty() && g_font_status) {
        SDL_Color color = {0x80, 0xb0, 0xe0, 0xFF};  // muted blue tint to echo selection color
        int tex_h = 0;
        g_status_tex = make_text_tex(renderer, g_font_status, g_status_text.c_str(), color,
                                     &g_status_tex_w, &tex_h);
        (void)tex_h;
    }
    if (!g_status_tex) return;

    // Fade out over the last 500ms of the 2s display window.
    static const uint32_t SHOW_MS = 1500;
    static const uint32_t FADE_MS = 500;
    uint32_t age = SDL_GetTicks() - g_status_set_ms;
    uint8_t alpha = 255;
    if (age > SHOW_MS) {
        float t = (float)(age - SHOW_MS) / (float)FADE_MS;
        if (t >= 1.0f) { alpha = 0; }
        else            { alpha = (uint8_t)(255.0f * (1.0f - t)); }
    }
    SDL_SetTextureAlphaMod(g_status_tex, alpha);

    int tex_phys_h = 0;
    {
        int tw = 0, th = 0;
        SDL_QueryTexture(g_status_tex, NULL, NULL, &tw, &th);
        tex_phys_h = th;
        (void)tw;
    }
    int text_y = g_status_rect.y + (g_status_bar_h - (int)(tex_phys_h / g_dpi_scale)) / 2;
    blit_text(renderer, g_status_tex, g_status_rect.x + 6, text_y, g_status_tex_w, tex_phys_h);
}


// ---------------------------------------------------------------------------
// Scenario loader
// ---------------------------------------------------------------------------

static void load_scenario(SDL_Renderer *renderer, size_t idx) {
    if (idx >= g_scenarios.size()) return;
    g_scenario_idx = idx;

    const auto &sc = g_scenarios[idx];
    const char *ctx_json = NULL;
    std::string ctx_storage;
    if (sc.context.is_object() && !sc.context.empty()) {
        ctx_storage = sc.context.dump();
        ctx_json = ctx_storage.c_str();
    }
    {
        auto it = k_screen_registry.find(sc.screen);
        if (it != k_screen_registry.end()) {
            it->second((void *)ctx_json);
            lv_timer_handler();
        }
    }

    update_label_strip(renderer, sc);

    int chrome_row_idx = find_chrome_row(idx);
    if (chrome_row_idx >= 0) {
        int row_top = chrome_row_idx * g_chrome_row_h;
        if (row_top < g_chrome_scroll_y)
            g_chrome_scroll_y = row_top;
        else if (row_top + g_chrome_row_h > g_chrome_scroll_y + g_chrome_rect.h)
            g_chrome_scroll_y = row_top + g_chrome_row_h - g_chrome_rect.h;
        chrome_scroll_clamp();
    }
}


// ---------------------------------------------------------------------------
// LVGL plumbing
// ---------------------------------------------------------------------------

static std::string slugify_token(const std::string &in) {
    std::string out;
    for (char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out.push_back(c);
        else if (c == ' ' || c == '-') out.push_back('_');
    }
    if (out.empty()) out = "variation";
    return out;
}

static int load_scenarios_file(const char *path, std::vector<scenario_def_t> &scenarios) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return -1;

    json root;
    try { ifs >> root; } catch (...) { return -1; }
    if (!root.is_object()) return -1;

    scenarios.clear();
    for (auto it = root.begin(); it != root.end(); ++it) {
        const std::string screen_name = it.key();
        const json &screen_def = it.value();
        if (!screen_def.is_object()) return -1;

        json base_ctx = json::object();
        if (screen_def.contains("context")) {
            if (!screen_def["context"].is_object()) return -1;
            base_ctx = screen_def["context"];
        }

        scenario_def_t base;
        base.name = screen_name;
        base.screen = screen_name;
        base.context = base_ctx;
        scenarios.push_back(base);

        if (screen_def.contains("variations")) {
            if (!screen_def["variations"].is_array()) return -1;
            for (const auto &var : screen_def["variations"]) {
                if (!var.is_object()) return -1;
                std::string var_name = var.value("name", std::string("variation"));
                if (var_name.empty()) var_name = "variation";

                json merged = base_ctx;
                if (var.contains("context")) {
                    if (!var["context"].is_object()) return -1;
                    merged.merge_patch(var["context"]);
                }

                scenario_def_t sc;
                sc.screen = screen_name;
                sc.name   = screen_name + "__" + slugify_token(var_name);
                sc.context = std::move(merged);
                scenarios.push_back(std::move(sc));
            }
        }
    }
    return scenarios.empty() ? -1 : 0;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    (void)drv;
    for (int y = area->y1; y <= area->y2; ++y) {
        if (y < 0 || y >= g_height) continue;
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x < 0 || x >= g_width) continue;
            g_fb[(size_t)y * (size_t)g_width + (size_t)x] = *color_p;
            color_p++;
        }
    }
    lv_disp_flush_ready(drv);
}

static void keypad_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    if (input_profile_get_mode() != INPUT_MODE_HARDWARE) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    if (g_key_ready) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key   = g_pending_key;
        g_key_ready = false;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void pointer_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    if (input_profile_get_mode() != INPUT_MODE_TOUCH) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    // Translate SDL logical coords → LVGL viewport-local coords
    data->point.x = (lv_coord_t)(g_pointer_x - g_viewport_rect.x);
    data->point.y = (lv_coord_t)(g_pointer_y - g_viewport_rect.y);
    data->state   = g_pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static uint32_t map_sdl_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP:       return LV_KEY_UP;
        case SDLK_DOWN:     return LV_KEY_DOWN;
        case SDLK_LEFT:     return LV_KEY_LEFT;
        case SDLK_RIGHT:    return LV_KEY_RIGHT;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return LV_KEY_ENTER;
        case SDLK_1: case SDLK_KP_1: case SDLK_EXCLAIM: return LV_KEY_ENTER;
        case SDLK_2: case SDLK_KP_2: case SDLK_AT:      return LV_KEY_ENTER;
        case SDLK_3: case SDLK_KP_3: case SDLK_HASH:    return LV_KEY_ENTER;
        default: return 0;
    }
}

static void blit_to_texture(SDL_Texture *texture) {
    void *pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(texture, NULL, &pixels, &pitch) != 0) return;
    for (int y = 0; y < g_height; ++y) {
        uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
        for (int x = 0; x < g_width; ++x) {
            size_t si = (size_t)y * (size_t)g_width + (size_t)x;
            uint32_t c32 = lv_color_to32(g_fb[si]);
            uint8_t r = (uint8_t)((c32 >> 16) & 0xFF);
            uint8_t g = (uint8_t)((c32 >>  8) & 0xFF);
            uint8_t b = (uint8_t)(c32 & 0xFF);
            row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
    SDL_UnlockTexture(texture);
}


// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv) {
    // Resolve scenarios.json: explicit arg > next to binary > repo-root default.
    std::string scenarios_path;
    if (argc > 1) {
        scenarios_path = argv[1];
    } else {
        char *base = SDL_GetBasePath();
        if (base) {
            scenarios_path = std::string(base) + "scenarios.json";
            SDL_free(base);
        }
        if (scenarios_path.empty() || load_scenarios_file(scenarios_path.c_str(), g_scenarios) != 0) {
            scenarios_path = "tools/scenarios.json";
        }
    }

    if (g_scenarios.empty() && load_scenarios_file(scenarios_path.c_str(), g_scenarios) != 0) {
        SDL_Log("Failed to load scenarios: %s", scenarios_path.c_str());
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    // Window layout (logical pixels):
    //   title bar spans full width at top.
    //   Below: chrome on left, label strip + viewport + status bar on right.
    int content_h = g_label_strip_h + g_label_gap + g_height + g_status_gap + g_status_bar_h;
    int window_w  = g_viewport_pad + g_chrome_w + g_chrome_gap + g_width + g_viewport_pad;
    int window_h  = g_viewport_pad + g_title_bar_h + g_title_gap + content_h + g_viewport_pad;

    SDL_Window *window = SDL_CreateWindow("screen_runner",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        window_w, window_h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) { SDL_Log("Window failed: %s", SDL_GetError()); SDL_Quit(); return 1; }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) { SDL_Log("Renderer failed: %s", SDL_GetError()); SDL_DestroyWindow(window); SDL_Quit(); return 1; }

    // HiDPI: detect DPI scale (2.0 on Retina) and load fonts at physical size.
    // All text textures have physical dimensions; we blit them at logical size
    // so the Retina display renders them at native resolution.
    {
        int drawable_w = window_w;
        SDL_GetRendererOutputSize(renderer, &drawable_w, NULL);
        g_dpi_scale = (float)drawable_w / (float)window_w;
        if (g_dpi_scale < 1.0f) g_dpi_scale = 1.0f;
    }
    // Put the renderer into logical pixel space. Without this, on Retina displays
    // SDL_WINDOW_ALLOW_HIGHDPI makes the renderer use physical pixels (2× the
    // logical window dimensions) and all our rects render at half the intended size.
    SDL_RenderSetLogicalSize(renderer, window_w, window_h);

    // Compute SDL rects (in logical pixels).
    int content_top = g_viewport_pad + g_title_bar_h + g_title_gap;
    int right_col_x = g_viewport_pad + g_chrome_w + g_chrome_gap;

    g_title_rect    = {g_viewport_pad, g_viewport_pad,
                       g_chrome_w + g_chrome_gap + g_width, g_title_bar_h};
    g_chrome_rect   = {g_viewport_pad, content_top, g_chrome_w, content_h};
    g_label_rect    = {right_col_x, content_top, g_width, g_label_strip_h};
    g_viewport_rect = {right_col_x, content_top + g_label_strip_h + g_label_gap,
                       g_width, g_height};
    g_status_rect   = {right_col_x,
                       content_top + g_label_strip_h + g_label_gap + g_height + g_status_gap,
                       g_width, g_status_bar_h};

    SDL_Texture *texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, g_width, g_height);
    if (!texture) { SDL_Log("Texture failed: %s", SDL_GetError()); return 1; }

    // SDL_ttf init + fonts loaded at physical size (dpi_scale × visual size).
    if (TTF_Init() == 0) {
        char *base = SDL_GetBasePath();
        std::string base_path     = base ? base : "./";
        std::string font_regular  = base_path + "screen_runner_font_regular.ttf";
        std::string font_semibold = base_path + "screen_runner_font_semibold.ttf";
        SDL_free(base);

        auto open = [&](const std::string &path, int visual_pt) -> TTF_Font * {
            return TTF_OpenFont(path.c_str(), (int)(visual_pt * g_dpi_scale));
        };
        g_font_sm     = open(font_regular,  15);
        g_font_md     = open(font_semibold, 17);
        g_font_label  = open(font_semibold, 18);
        g_font_status = open(font_regular,  16);
        g_font_title  = open(font_semibold, 20);

        if (!g_font_sm || !g_font_md) {
            SDL_Log("Warning: could not open fonts — chrome text will not render. (%s)",
                    TTF_GetError());
        }
    }

    // Build title bar textures.
    if (g_font_title) {
        SDL_Color orange = {0xff, 0x9f, 0x0a, 0xFF};
        SDL_Color white  = {0xe8, 0xe8, 0xe8, 0xFF};
        int h1 = 0, h2 = 0;
        g_title_brand_tex = make_text_tex(renderer, g_font_title, "SeedSigner",
                                          orange, &g_title_brand_w, &h1);
        g_title_rest_tex  = make_text_tex(renderer, g_font_title, " LVGL Screen Runner",
                                          white,  &g_title_rest_w,  &h2);
        g_title_tex_h = std::max(h1, h2);
    }

    // Load logo BMP (generated at build time by CMakeLists post-build step).
    {
        char *base = SDL_GetBasePath();
        std::string logo_path = std::string(base ? base : "./") + "screen_runner_logo.bmp";
        SDL_free(base);
        SDL_Surface *surf = SDL_LoadBMP(logo_path.c_str());
        if (surf) {
            g_logo_tex = SDL_CreateTextureFromSurface(renderer, surf);
            g_logo_w   = surf->w;
            g_logo_h   = surf->h;
            SDL_FreeSurface(surf);
            if (g_logo_tex)
                SDL_SetTextureBlendMode(g_logo_tex, SDL_BLENDMODE_BLEND);
        } else {
            SDL_Log("Logo not loaded (expected at %s) — falling back to text title.",
                    logo_path.c_str());
        }
    }

    // LVGL init.
    lv_init();
    g_fb.assign((size_t)g_width * (size_t)g_height, lv_color_black());

    static lv_disp_draw_buf_t disp_buf;
    std::vector<lv_color_t> draw_buf((size_t)g_width * 40u);
    lv_disp_draw_buf_init(&disp_buf, draw_buf.data(), NULL, (uint32_t)draw_buf.size());

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = g_width;
    disp_drv.ver_res  = g_height;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    (void)disp;

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_KEYPAD;
    indev_drv.read_cb = keypad_read_cb;
    lv_indev_drv_register(&indev_drv);

    static lv_indev_drv_t pointer_drv;
    lv_indev_drv_init(&pointer_drv);
    pointer_drv.type    = LV_INDEV_TYPE_POINTER;
    pointer_drv.read_cb = pointer_read_cb;
    lv_indev_drv_register(&pointer_drv);

    input_profile_set_mode(INPUT_MODE_HARDWARE);

    build_chrome_rows(renderer);

    // Build mode badge textures (prefix is static; label changes on toggle).
    if (g_font_sm) {
        SDL_Color dim = {0x60, 0x60, 0x60, 0xFF};
        int h = 0;
        g_mode_prefix_tex = make_text_tex(renderer, g_font_sm, "Input mode:",
                                          dim, &g_mode_prefix_w, &h);
        (void)h;
    }
    rebuild_mode_label(renderer);

    load_scenario(renderer, 0);
    SDL_SetWindowTitle(window, "screen_runner");

    bool running = true;
    uint32_t last_tick = SDL_GetTicks();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;

            } else if (ev.type == SDL_MOUSEMOTION) {
                g_pointer_x = ev.motion.x;
                g_pointer_y = ev.motion.y;

            } else if (ev.type == SDL_MOUSEBUTTONUP) {
                if (ev.button.button == SDL_BUTTON_LEFT)
                    g_pointer_pressed = false;

            } else if (ev.type == SDL_MOUSEWHEEL) {
                int mx, my;
                SDL_GetMouseState(&mx, &my);
                bool over_chrome = (mx >= g_chrome_rect.x &&
                                    mx < g_chrome_rect.x + g_chrome_rect.w &&
                                    my >= g_chrome_rect.y &&
                                    my < g_chrome_rect.y + g_chrome_rect.h);
                if (over_chrome) {
                    g_chrome_scroll_y -= ev.wheel.y * g_chrome_row_h * 2;
                    chrome_scroll_clamp();
                }
                bool over_viewport = (mx >= g_viewport_rect.x &&
                                      mx < g_viewport_rect.x + g_viewport_rect.w &&
                                      my >= g_viewport_rect.y &&
                                      my < g_viewport_rect.y + g_viewport_rect.h);
                if (over_viewport && input_profile_get_mode() == INPUT_MODE_TOUCH) {
                    lv_obj_t *scr = lv_scr_act();
                    uint32_t n = lv_obj_get_child_cnt(scr);
                    for (uint32_t i = 0; i < n; ++i) {
                        lv_obj_t *child = lv_obj_get_child(scr, (int32_t)i);
                        // Only scroll containers that actually have overflow content.
                        // This prevents mistakenly scrolling the top_nav (no overflow)
                        // and correctly targets the body content container.
                        if (lv_obj_has_flag(child, LV_OBJ_FLAG_SCROLLABLE) &&
                                lv_obj_get_scroll_dir(child) != LV_DIR_NONE &&
                                (lv_obj_get_scroll_top(child) > 0 ||
                                 lv_obj_get_scroll_bottom(child) > 0)) {
                            // scroll_by(0, +dy): children move down → reveal top (scroll up)
                            // scroll_by(0, -dy): children move up → reveal bottom (scroll down)
                            // ev.wheel.y > 0 = user intends scroll up (same convention as chrome scroll)
                            lv_coord_t dy = (lv_coord_t)(ev.wheel.y * 40);
                            if (dy > 0) {
                                lv_coord_t avail = lv_obj_get_scroll_top(child);
                                if (dy > avail) dy = avail;
                            } else if (dy < 0) {
                                lv_coord_t avail = lv_obj_get_scroll_bottom(child);
                                if (-dy > avail) dy = -avail;
                            }
                            if (dy != 0) lv_obj_scroll_by(child, 0, dy, LV_ANIM_OFF);
                            break;
                        }
                    }
                }

            } else if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
                int bx = ev.button.x, by = ev.button.y;
                bool over_chrome = (bx >= g_chrome_rect.x &&
                                    bx < g_chrome_rect.x + g_chrome_rect.w &&
                                    by >= g_chrome_rect.y &&
                                    by < g_chrome_rect.y + g_chrome_rect.h);
                if (over_chrome) {
                    int row_idx = chrome_row_at_y(by - g_chrome_rect.y);
                    if (row_idx >= 0)
                        load_scenario(renderer, g_chrome_rows[row_idx].scenario_idx);
                }

                // Mode badge click (in title bar)
                bool over_badge = (bx >= g_mode_badge_rect.x &&
                                   bx < g_mode_badge_rect.x + g_mode_badge_rect.w &&
                                   by >= g_mode_badge_rect.y &&
                                   by < g_mode_badge_rect.y + g_mode_badge_rect.h);
                if (over_badge) toggle_input_mode(renderer);

                // Viewport press (touch/mouse mode only)
                bool over_viewport = (bx >= g_viewport_rect.x &&
                                      bx < g_viewport_rect.x + g_viewport_rect.w &&
                                      by >= g_viewport_rect.y &&
                                      by < g_viewport_rect.y + g_viewport_rect.h);
                if (over_viewport && input_profile_get_mode() == INPUT_MODE_TOUCH) {
                    g_pointer_x = bx;
                    g_pointer_y = by;
                    g_pointer_pressed = true;
                }

            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode key = ev.key.keysym.sym;
                bool prev = (key == SDLK_PAGEUP  || key == SDLK_LEFTBRACKET  || key == SDLK_COMMA);
                bool next = (key == SDLK_PAGEDOWN || key == SDLK_RIGHTBRACKET || key == SDLK_PERIOD);

                if (prev) {
                    size_t n = (g_scenario_idx == 0) ? g_scenarios.size() - 1 : g_scenario_idx - 1;
                    load_scenario(renderer, n);
                } else if (next) {
                    load_scenario(renderer, (g_scenario_idx + 1) % g_scenarios.size());
                } else if (key == SDLK_t) {
                    toggle_input_mode(renderer);
                } else {
                    uint32_t mapped = map_sdl_key(key);
                    if (mapped != 0) { g_pending_key = mapped; g_key_ready = true; }
                }
            }
        }

        uint32_t now = SDL_GetTicks();
        lv_tick_inc(now - last_tick);
        last_tick = now;
        lv_timer_handler();

        // Clear status text after fade completes (1.5s show + 0.5s fade = 2s total).
        if (!g_status_text.empty() && (now - g_status_set_ms) >= 2000) {
            g_status_text.clear();
            if (g_status_tex) { SDL_DestroyTexture(g_status_tex); g_status_tex = NULL; }
        }

        blit_to_texture(texture);

        SDL_SetRenderDrawColor(renderer, 18, 18, 18, 255);
        SDL_RenderClear(renderer);

        draw_title_bar(renderer);
        draw_chrome(renderer);
        draw_label_strip(renderer);
        draw_status_bar(renderer);
        SDL_RenderCopy(renderer, texture, NULL, &g_viewport_rect);

        SDL_SetRenderDrawColor(renderer, 48, 48, 48, 255);
        SDL_RenderDrawRect(renderer, &g_chrome_rect);
        SDL_RenderDrawRect(renderer, &g_viewport_rect);

        SDL_RenderPresent(renderer);
    }

    // Cleanup.
    if (g_logo_tex)         SDL_DestroyTexture(g_logo_tex);
    if (g_mode_label_tex)   SDL_DestroyTexture(g_mode_label_tex);
    if (g_mode_prefix_tex)  SDL_DestroyTexture(g_mode_prefix_tex);
    if (g_status_tex)       SDL_DestroyTexture(g_status_tex);
    if (g_label_strip_tex)  SDL_DestroyTexture(g_label_strip_tex);
    if (g_title_brand_tex)  SDL_DestroyTexture(g_title_brand_tex);
    if (g_title_rest_tex)   SDL_DestroyTexture(g_title_rest_tex);
    for (auto &r : g_chrome_rows)
        if (r.label_tex) SDL_DestroyTexture(r.label_tex);

    if (g_font_sm)     TTF_CloseFont(g_font_sm);
    if (g_font_md)     TTF_CloseFont(g_font_md);
    if (g_font_label)  TTF_CloseFont(g_font_label);
    if (g_font_status) TTF_CloseFont(g_font_status);
    if (g_font_title)  TTF_CloseFont(g_font_title);
    TTF_Quit();

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
