#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <exception>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <png.h>

#include "lvgl.h"
#include "seedsigner.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define DEFAULT_WIDTH 480
#define DEFAULT_HEIGHT 320
#define DEFAULT_OUT_DIR "tools/screenshot_generator/screenshots"
#define DEFAULT_SCENARIOS_FILE "tools/scenarios.json"

static int g_width = DEFAULT_WIDTH;
static int g_height = DEFAULT_HEIGHT;
static std::vector<uint16_t> g_fb;


// External dependency alias.
// nlohmann/json is pulled in automatically by CMake (FetchContent) at build time.
using json = nlohmann::ordered_json;


typedef void (*screen_fn_t)(void *ctx_json);

static const std::unordered_map<std::string, screen_fn_t> k_screen_registry = {
    {"main_menu_screen", main_menu_screen},
    {"button_list_screen", button_list_screen},
    {"screensaver_screen", screensaver_screen},
};

static screen_fn_t lookup_screen_fn(const std::string &name) {
    auto it = k_screen_registry.find(name);
    return (it == k_screen_registry.end()) ? NULL : it->second;
}

struct scenario_def_t {
    std::string name;
    std::string screen;
    json context = json::object();
};

static std::string slugify_token(const std::string &in) {
    std::string out;
    for (char c : in) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') out.push_back(c);
        else if (c == ' ' || c == '-') out.push_back('_');
    }
    if (out.empty()) out = "variation";
    return out;
}

// Load JSON scenario definitions from disk.
//
// JSON schema (high level):
// {
//   "screen_function": {
//     "context": { ...base args... },
//     "variations": [
//       {"name": "...", "context": { ...overrides... }}
//     ]
//   }
// }
//
// Each variation becomes its own concrete scenario by merge-patching
// variation.context onto the base context.
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

                // Start from base context, then patch in per-variation changes.
                json merged = base_ctx;
                if (var.contains("context")) {
                    if (!var["context"].is_object()) return -1;
                    // RFC 7396 merge-patch semantics: object keys in variation
                    // override/add keys in base context.
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

static std::string ctx_get_string(const json &ctx, const char *key, const char *defv) {
    if (!ctx.is_object() || !ctx.contains(key) || !ctx[key].is_string()) return defv ? std::string(defv) : std::string();
    return ctx[key].get<std::string>();
}

static bool ctx_get_bool(const json &ctx, const char *key, bool defv) {
    if (!ctx.is_object() || !ctx.contains(key) || !ctx[key].is_boolean()) return defv;
    return ctx[key].get<bool>();
}

static std::vector<std::string> ctx_get_string_array(const json &ctx, const char *key) {
    std::vector<std::string> out;
    if (!ctx.is_object() || !ctx.contains(key) || !ctx[key].is_array()) return out;
    for (const auto &item : ctx[key]) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

// Render one resolved scenario (already expanded from base + variation).
//
// - scenario.screen: Chooses which C screen function to call.
// - scenario.context: Data that drives the screen content. 
static bool render_scenario_def(const scenario_def_t &scenario) {
    screen_fn_t fn = lookup_screen_fn(scenario.screen);
    if (!fn) {
        return false;
    }

    // Unified contract: screens receive JSON context pointer or NULL.
    const char *ctx_json = NULL;
    std::string ctx_storage;
    if (scenario.context.is_object() && !scenario.context.empty()) {
        ctx_storage = scenario.context.dump();
        ctx_json = ctx_storage.c_str();
    }

    fn((void *)ctx_json);
    return true;
}


static int write_png_rgb24(const char *path, const uint8_t *rgb, int width, int height) {
    if (!path || !rgb || width <= 0 || height <= 0) return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        fclose(fp);
        return -1;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        fclose(fp);
        return -1;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return -1;
    }

    png_init_io(png_ptr, fp);
    png_set_IHDR(png_ptr, info_ptr,
                 (png_uint_32)width, (png_uint_32)height,
                 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);

    png_write_info(png_ptr, info_ptr);

    std::vector<png_bytep> rows((size_t)height);
    const size_t stride = (size_t)width * 3u;
    for (int y = 0; y < height; ++y) {
        rows[(size_t)y] = (png_bytep)(rgb + (size_t)y * stride);
    }

    png_write_image(png_ptr, rows.data());
    png_write_end(png_ptr, NULL);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);
    return 0;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) return -1;
    return 0;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    uint16_t *color_p = (uint16_t *)px_map;
    for (int y = area->y1; y <= area->y2; ++y) {
        if (y < 0 || y >= g_height) continue;
        for (int x = area->x1; x <= area->x2; ++x) {
            if (x < 0 || x >= g_width) continue;
            size_t di = (size_t)y * (size_t)g_width + (size_t)x;
            g_fb[di] = *color_p;
            color_p++;
        }
    }
    lv_display_flush_ready(disp);
}

static int framebuffer_to_rgb24(std::vector<uint8_t> &rgb) {
    rgb.resize((size_t)g_width * (size_t)g_height * 3u);
    for (int y = 0; y < g_height; ++y) {
        for (int x = 0; x < g_width; ++x) {
            size_t si = (size_t)y * (size_t)g_width + (size_t)x;
            size_t di = si * 3u;
            // Convert RGB565 to RGB888
            uint16_t c = g_fb[si];
            rgb[di + 0] = (uint8_t)((c >> 11) << 3);          // R
            rgb[di + 1] = (uint8_t)(((c >> 5) & 0x3F) << 2);  // G
            rgb[di + 2] = (uint8_t)((c & 0x1F) << 3);         // B
        }
    }
    return 0;
}

static std::string html_escape(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (char ch : in) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += ch; break;
        }
    }
    return out;
}


static std::string scenario_display_name(const std::string &scenario) {
    const std::string marker = "_screen_";
    size_t pos = scenario.find(marker);
    if (pos == std::string::npos) return scenario;

    std::string base = scenario.substr(0, pos + marker.size() - 1); // include "_screen"
    std::string variation = scenario.substr(pos + marker.size());
    if (variation.empty()) return scenario;
    return base + " (" + variation + ")";
}

static const char* imagemagick_binary() {
    if (system("command -v magick >/dev/null 2>&1") == 0) return "magick";
    if (system("command -v convert >/dev/null 2>&1") == 0) return "convert";
    return NULL;
}


static void cleanup_frames_dir(const char *frames_dir) {
    DIR *d = opendir(frames_dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", frames_dir, name);
        unlink(path);
    }
    closedir(d);
    rmdir(frames_dir);
}

// Optionally export an animated GIF for scrolling-title scenarios.
//
// Behavior:
// 1) Capture frame sequence by advancing LVGL ticks/timers.
// 2) Assemble GIF using ImageMagick (magick/convert).
// 3) Delete intermediate frame PNGs to keep output directories clean.
static int maybe_write_scroll_gif(const char *run_dir, const std::string &scenario, lv_display_t *disp, const char *im_bin) {
    if (!im_bin) {
        return 0;
    }

    char frames_dir[PATH_MAX];
    snprintf(frames_dir, sizeof(frames_dir), "%s/img/%s.frames", run_dir, scenario.c_str());
    if (mkdir_p(frames_dir) != 0) {
        return -1;
    }

    // ~10 seconds at ~18 FPS (56ms tick step).
    const int frame_count = 180;
    const int frame_step_ms = 56;  // ~18 FPS over ~10s

    for (int i = 0; i < frame_count; ++i) {
        lv_tick_inc(frame_step_ms);
        lv_timer_handler();
        lv_refr_now(disp);

        std::vector<uint8_t> rgb;
        framebuffer_to_rgb24(rgb);

        char frame_png[PATH_MAX];
        snprintf(frame_png, sizeof(frame_png), "%s/frame_%03d.png", frames_dir, i);
        if (write_png_rgb24(frame_png, rgb.data(), g_width, g_height) != 0) {
            return -1;
        }
    }

    char out_gif[PATH_MAX];
    snprintf(out_gif, sizeof(out_gif), "%s/img/%s.gif", run_dir, scenario.c_str());

    char cmd[PATH_MAX * 3];
    snprintf(cmd, sizeof(cmd), "%s -delay 8 -loop 0 '%s'/frame_*.png '%s'", im_bin, frames_dir, out_gif);
    int rc = system(cmd);
    if (rc != 0) {
        cleanup_frames_dir(frames_dir);
        return -1;
    }

    cleanup_frames_dir(frames_dir);
    return 0;
}

static int remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, name);

        struct stat st;
        if (lstat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            remove_tree(child);
            rmdir(child);
        } else {
            unlink(child);
        }
    }
    closedir(d);
    return 0;
}

static int write_manifest_json(const char *run_dir, const char *generated_at, const std::vector<scenario_def_t> &scenarios, int width, int height) {
    json manifest = json::object();
    manifest["generated_at"] = generated_at;
    manifest["width"] = width;
    manifest["height"] = height;
    manifest["screenshots"] = json::array();

    for (const scenario_def_t &scenario : scenarios) {
        char gif_path[PATH_MAX];
        snprintf(gif_path, sizeof(gif_path), "%s/img/%s.gif", run_dir, scenario.name.c_str());
        struct stat gif_st;
        bool has_gif = (stat(gif_path, &gif_st) == 0) && S_ISREG(gif_st.st_mode);
        const char *ext = has_gif ? "gif" : "png";

        json item = json::object();
        item["name"] = scenario.name;
        item["display_name"] = scenario_display_name(scenario.name) + (has_gif ? " [animated]" : "");
        item["path"] = std::string("img/") + scenario.name + "." + ext;
        manifest["screenshots"].push_back(item);
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/manifest.json", run_dir);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    std::string text = manifest.dump(2);
    fwrite(text.data(), 1, text.size(), f);
    fputc('\n', f);
    fclose(f);
    return 0;
}

static void usage(const char *argv0) {
    printf("Usage: %s [options]\n", argv0);
    printf("  --out-dir <path>      Output dir (default: %s)\n", DEFAULT_OUT_DIR);
    printf("  --width <px>          Width (default: %d)\n", DEFAULT_WIDTH);
    printf("  --height <px>         Height (default: %d)\n", DEFAULT_HEIGHT);
    printf("  --scenarios-file <path>  Scenario config file (default: %s)\n", DEFAULT_SCENARIOS_FILE);
}

int main(int argc, char **argv) {
    const char *out_dir = DEFAULT_OUT_DIR;
    int width = DEFAULT_WIDTH;
    int height = DEFAULT_HEIGHT;
    const char *scenarios_file = DEFAULT_SCENARIOS_FILE;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--scenarios-file") == 0 && i + 1 < argc) {
            scenarios_file = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "invalid width/height\n");
        return 1;
    }

    g_width = width;
    g_height = height;
    g_fb.assign((size_t)g_width * (size_t)g_height, 0);

    if (mkdir_p(out_dir) != 0) {
        fprintf(stderr, "Failed creating out dir: %s\n", out_dir);
        return 1;
    }

    // Single-output mode: preserve static files (e.g. index.html),
    // clear only image artifacts under img/, and overwrite manifest.json.

    time_t t = time(NULL);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%SZ", &tm_utc);

    const char *run_dir = out_dir;

    char img_dir[PATH_MAX];
    snprintf(img_dir, sizeof(img_dir), "%s/img", run_dir);
    remove_tree(img_dir);
    if (mkdir_p(img_dir) != 0) {
        fprintf(stderr, "Failed creating image dir: %s\n", img_dir);
        return 1;
    }

    lv_init();

    lv_display_t *disp = lv_display_create(g_width, g_height);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    static std::vector<uint8_t> draw_buf((size_t)g_width * (size_t)g_height * 2u);
    lv_display_set_buffers(disp, draw_buf.data(), NULL, draw_buf.size(),
                           LV_DISPLAY_RENDER_MODE_FULL);

    const char *im_bin = imagemagick_binary();
    if (im_bin) {
        printf("imagemagick detected (%s): animated GIF generation enabled for scrolling-title scenarios\n", im_bin);
    } else {
        printf("imagemagick not detected (magick/convert): animated GIF generation disabled\n");
    }

    std::vector<scenario_def_t> scenarios;
    if (load_scenarios_file(scenarios_file, scenarios) != 0) {
        const char *fallback = "tools/scenarios.json";
        if (strcmp(scenarios_file, fallback) != 0 && load_scenarios_file(fallback, scenarios) == 0) {
            fprintf(stderr, "note: using fallback scenarios file: %s\n", fallback);
        } else {
            fprintf(stderr, "Failed to load scenarios file: %s\n", scenarios_file);
            return 1;
        }
    }

    for (const scenario_def_t &scenario : scenarios) {
        try {
            if (!render_scenario_def(scenario)) {
                fprintf(stderr, "Unknown/invalid scenario: %s\n", scenario.name.c_str());
                return 2;
            }

            lv_timer_handler();
            lv_refr_now(disp);

            std::vector<uint8_t> rgb;
            framebuffer_to_rgb24(rgb);

            char out_png[PATH_MAX];
            snprintf(out_png, sizeof(out_png), "%s/img/%s.png", run_dir, scenario.name.c_str());
            if (write_png_rgb24(out_png, rgb.data(), g_width, g_height) != 0) {
                fprintf(stderr, "Failed writing PNG: %s\n", out_png);
                return 1;
            }
            printf("wrote %s\n", out_png);

            if (maybe_write_scroll_gif(run_dir, scenario.name, disp, (im_bin && ctx_get_bool(scenario.context, "animated", false)) ? im_bin : NULL) != 0) {
                fprintf(stderr, "Failed writing animated GIF for scenario: %s\n", scenario.name.c_str());
                return 1;
            }
            if (im_bin && ctx_get_bool(scenario.context, "animated", false)) {
                char out_gif[PATH_MAX];
                snprintf(out_gif, sizeof(out_gif), "%s/img/%s.gif", run_dir, scenario.name.c_str());
                printf("wrote %s\n", out_gif);
            }
        } catch (const std::exception &e) {
            fprintf(stderr, "Scenario %s threw exception: %s\n", scenario.name.c_str(), e.what());
            return 3;
        } catch (...) {
            fprintf(stderr, "Scenario %s threw unknown exception\n", scenario.name.c_str());
            return 3;
        }
    }

    if (write_manifest_json(run_dir, ts, scenarios, g_width, g_height) != 0) {
        fprintf(stderr, "Failed writing manifest.json in %s\n", run_dir);
        return 1;
    }

    printf("done: %zu scenario(s), %dx%d\n", scenarios.size(), g_width, g_height);
    return 0;
}
