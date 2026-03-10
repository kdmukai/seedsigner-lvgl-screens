#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#else
#error "SDL header not found (expected SDL2/SDL.h or SDL.h)"
#endif

#include <nlohmann/json.hpp>

#include "lvgl.h"
#include "seedsigner.h"
#include "input_profile.h"

#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

using json = nlohmann::ordered_json;

typedef void (*screen_fn_t)(void *ctx_json);

static const std::unordered_map<std::string, screen_fn_t> k_screen_registry = {
    {"main_menu_screen", main_menu_screen},
    {"button_list_screen", button_list_screen},
};

struct scenario_def_t {
    std::string name;
    std::string screen;
    json context = json::object();
};

static int g_width = 480;
static int g_height = 320;
static std::vector<lv_color_t> g_fb;

static uint32_t g_pending_key = 0;
static bool g_key_ready = false;

static screen_fn_t lookup_screen_fn(const std::string &name) {
    auto it = k_screen_registry.find(name);
    return (it == k_screen_registry.end()) ? NULL : it->second;
}

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
    try {
        ifs >> root;
    } catch (...) {
        return -1;
    }
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
                sc.name = screen_name + "__" + slugify_token(var_name);
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
            size_t di = (size_t)y * (size_t)g_width + (size_t)x;
            g_fb[di] = *color_p;
            color_p++;
        }
    }
    lv_disp_flush_ready(drv);
}

static void keypad_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    if (g_key_ready) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = g_pending_key;
        g_key_ready = false;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t map_sdl_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP: return LV_KEY_UP;
        case SDLK_DOWN: return LV_KEY_DOWN;
        case SDLK_LEFT: return LV_KEY_LEFT;
        case SDLK_RIGHT: return LV_KEY_RIGHT;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: return LV_KEY_ENTER;

        // Compatibility choice (temporary): map AUX test keys to ENTER so desktop
        // runner can exercise KEY1/2/3 default-enter behavior even when distinct
        // aux keycodes are not propagated through the host keypad path.
        case SDLK_1:
        case SDLK_KP_1:
        case SDLK_EXCLAIM:
        case SDLK_F1: return LV_KEY_ENTER;

        case SDLK_2:
        case SDLK_KP_2:
        case SDLK_AT:
        case SDLK_F2: return LV_KEY_ENTER;

        case SDLK_3:
        case SDLK_KP_3:
        case SDLK_HASH:
        case SDLK_F3: return LV_KEY_ENTER;

        default: return 0;
    }
}

static bool render_scenario(const scenario_def_t &scenario) {
    screen_fn_t fn = lookup_screen_fn(scenario.screen);
    if (!fn) return false;

    std::string ctx_storage;
    const char *ctx_json = NULL;
    if (scenario.context.is_object() && !scenario.context.empty()) {
        ctx_storage = scenario.context.dump();
        ctx_json = ctx_storage.c_str();
    }

    fn((void *)ctx_json);
    lv_timer_handler();
    return true;
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
            uint8_t g = (uint8_t)((c32 >> 8) & 0xFF);
            uint8_t b = (uint8_t)(c32 & 0xFF);
            row[x] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    SDL_UnlockTexture(texture);
}

int main(int argc, char **argv) {
    const char *scenarios_file = "tools/scenarios.json";
    if (argc > 1) {
        scenarios_file = argv[1];
    }

    std::vector<scenario_def_t> scenarios;
    if (load_scenarios_file(scenarios_file, scenarios) != 0) {
        SDL_Log("Failed to load scenarios: %s", scenarios_file);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("screen_runner", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_width, g_height, SDL_WINDOW_SHOWN);
    if (!window) {
        SDL_Log("Window create failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_Log("Renderer create failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, g_width, g_height);
    if (!texture) {
        SDL_Log("Texture create failed: %s", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    lv_init();
    g_fb.assign((size_t)g_width * (size_t)g_height, lv_color_black());

    static lv_disp_draw_buf_t disp_buf;
    std::vector<lv_color_t> draw_buf((size_t)g_width * 40u);
    lv_disp_draw_buf_init(&disp_buf, draw_buf.data(), NULL, (uint32_t)draw_buf.size());

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = g_width;
    disp_drv.ver_res = g_height;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    (void)disp;

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_KEYPAD;
    indev_drv.read_cb = keypad_read_cb;
    lv_indev_drv_register(&indev_drv);

    // Design decision: default runner mode is hardware so scenario loads always start
    // with keypad-style focus behavior without per-screen branching.
    input_profile_set_mode(INPUT_MODE_HARDWARE);

    size_t scenario_idx = 0;
    if (!render_scenario(scenarios[scenario_idx])) {
        SDL_Log("Failed to render initial scenario");
    }
    SDL_SetWindowTitle(window, ("screen_runner: " + scenarios[scenario_idx].name).c_str());

    bool running = true;
    uint32_t last_tick = SDL_GetTicks();

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = false;
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode key = ev.key.keysym.sym;
                bool prev_scenario = (key == SDLK_PAGEUP || key == SDLK_LEFTBRACKET || key == SDLK_COMMA);
                bool next_scenario = (key == SDLK_PAGEDOWN || key == SDLK_RIGHTBRACKET || key == SDLK_PERIOD);

                if (prev_scenario) {
                    if (scenario_idx == 0) scenario_idx = scenarios.size() - 1;
                    else scenario_idx--;
                    render_scenario(scenarios[scenario_idx]);
                    SDL_SetWindowTitle(window, ("screen_runner: " + scenarios[scenario_idx].name).c_str());
                } else if (next_scenario) {
                    scenario_idx = (scenario_idx + 1) % scenarios.size();
                    render_scenario(scenarios[scenario_idx]);
                    SDL_SetWindowTitle(window, ("screen_runner: " + scenarios[scenario_idx].name).c_str());
                } else {
                    uint32_t mapped = map_sdl_key(key);
                    if (mapped != 0) {
                        g_pending_key = mapped;
                        g_key_ready = true;
                    }
                }
            }
        }

        uint32_t now = SDL_GetTicks();
        uint32_t elapsed = now - last_tick;
        last_tick = now;
        lv_tick_inc(elapsed);
        lv_timer_handler();

        blit_to_texture(texture);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
