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
#include "gui_constants.h"
#include "locale_fonts.h"
#include "locale_loader.h"
#include "locale_picker.h"
#include "seedsigner.h"
#include "input_profile.h"
#include "shape_spike.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef DISPLAY_WIDTH
#error "DISPLAY_WIDTH must be defined by the build system"
#endif
#ifndef DISPLAY_HEIGHT
#error "DISPLAY_HEIGHT must be defined by the build system"
#endif

#define DEFAULT_OUT_DIR "tools/apps/screenshot_generator/screenshots"
#define DEFAULT_SCENARIOS_FILE "tools/scenarios/scenarios.json"

// Animated-GIF capture/playback timing. The GIF format stores each frame's delay
// as an integer number of CENTISECONDS, so a true 15 FPS (66.7ms) is not
// representable; we snap to the nearest 10ms grid point, 70ms (~14.3 FPS), and
// use it for BOTH the capture tick step and the GIF frame delay so playback runs
// at real speed. Default length 2s, overridable per scenario via the
// "animation_seconds" context key. (At 70ms/frame the 1.4s warning-edge pulse is
// exactly 20 frames — one seamless loop.)
static const int    GIF_FRAME_MS = 70;
static const double GIF_DEFAULT_SECONDS = 2.0;

static int g_width = DISPLAY_WIDTH;
static int g_height = DISPLAY_HEIGHT;
static std::vector<uint16_t> g_fb;


// External dependency alias.
// nlohmann/json is pulled in automatically by CMake (FetchContent) at build time.
using json = nlohmann::ordered_json;


typedef void (*screen_fn_t)(void *ctx_json);

static const std::unordered_map<std::string, screen_fn_t> k_screen_registry = {
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

static screen_fn_t lookup_screen_fn(const std::string &name) {
    auto it = k_screen_registry.find(name);
    return (it == k_screen_registry.end()) ? NULL : it->second;
}

struct scenario_def_t {
    std::string name;
    std::string screen;
    json context = json::object();
    // Generator-only directives (the `screenshot` namespace) — GIF capture params, etc.
    // A sibling of `context`, so it is NEVER passed to the screen and the interactive
    // runners (which read only `context`/`variations`) ignore it automatically.
    json shot = json::object();
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

        // Generator-only directives live in a `screenshot` sibling of `context` (screen-
        // level here; a variation may override it below).
        json base_shot = json::object();
        if (screen_def.contains("screenshot")) {
            if (!screen_def["screenshot"].is_object()) return -1;
            base_shot = screen_def["screenshot"];
        }

        scenario_def_t base;
        base.name = screen_name;
        base.screen = screen_name;
        base.context = base_ctx;
        base.shot = base_shot;
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

                // Per-variation screenshot directives override the screen-level ones
                // (e.g. only one variation of an otherwise-static screen is animated).
                json shot = base_shot;
                if (var.contains("screenshot")) {
                    if (!var["screenshot"].is_object()) return -1;
                    shot.merge_patch(var["screenshot"]);
                }

                scenario_def_t sc;
                sc.screen = screen_name;
                sc.name = screen_name + "__" + slugify_token(var_name);
                sc.context = std::move(merged);
                sc.shot = std::move(shot);
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

static double ctx_get_double(const json &ctx, const char *key, double defv) {
    if (!ctx.is_object() || !ctx.contains(key) || !ctx[key].is_number()) return defv;
    return ctx[key].get<double>();
}

// GIF capture params come from the `screenshot` namespace; for backward compatibility we
// fall back to the legacy in-`context` keys (animated / animation_seconds) when absent.
static bool scenario_is_animated(const scenario_def_t &sc) {
    if (sc.shot.is_object() && sc.shot.contains("animated") && sc.shot["animated"].is_boolean())
        return sc.shot["animated"].get<bool>();
    return ctx_get_bool(sc.context, "animated", false);
}
static double scenario_gif_seconds(const scenario_def_t &sc) {
    if (sc.shot.is_object() && sc.shot.contains("seconds") && sc.shot["seconds"].is_number())
        return sc.shot["seconds"].get<double>();
    return ctx_get_double(sc.context, "animation_seconds", GIF_DEFAULT_SECONDS);
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
//
// img_dir: directory for output images (e.g. "screenshots/img/480x320")
// file_base: base filename without extension (e.g. "main_menu_screen_480x320")
static int maybe_write_scroll_gif(const char *img_dir, const std::string &file_base, lv_display_t *disp,
                                  const char *im_bin, double anim_seconds) {
    if (!im_bin) {
        return 0;
    }

    char frames_dir[PATH_MAX];
    snprintf(frames_dir, sizeof(frames_dir), "%s/%s.frames", img_dir, file_base.c_str());
    if (mkdir_p(frames_dir) != 0) {
        return -1;
    }

    // Capture and play back at a fixed 15 FPS. There is no "perfect" length: the
    // scroll loop period depends on the title text and the resolution width, so a
    // fixed count can't capture exactly one loop. Default to GIF_DEFAULT_SECONDS;
    // a scenario can override via "animation_seconds" (e.g. a long title whose
    // marquee needs longer than the default to scroll through once).
    const int frame_step_ms = GIF_FRAME_MS;
    int frame_count = (int)(anim_seconds * 1000.0 / GIF_FRAME_MS + 0.5);
    if (frame_count < 1) frame_count = 1;

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
    snprintf(out_gif, sizeof(out_gif), "%s/%s.gif", img_dir, file_base.c_str());

    char cmd[PATH_MAX * 3];
    // GIF delay is in centiseconds; GIF_FRAME_MS is a multiple of 10 so it maps
    // exactly, keeping playback in lockstep with the capture tick step above.
    snprintf(cmd, sizeof(cmd), "%s -delay %d -loop 0 '%s'/frame_*.png '%s'",
             im_bin, GIF_FRAME_MS / 10, frames_dir, out_gif);
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

static int write_manifest_json(const char *run_dir, const char *generated_at, const json &resolutions) {
    json manifest = json::object();
    manifest["generated_at"] = generated_at;
    manifest["resolutions"] = resolutions;

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
    printf("  --scenarios-file <path>  Scenario config file (default: %s)\n", DEFAULT_SCENARIOS_FILE);
    printf("\nSupported resolutions (%d):\n", display_profile_count());
    for (int i = 0; i < display_profile_count(); ++i) {
        const DisplayProfile &p = display_profile_at(i);
        printf("  %dx%d\n", p.width, p.height);
    }
}

// Filesystem byte provider for the shared locale loader (ss_load_locale). Reads
// pack file <font_dir>/<locale>/<file> into a reusable scratch buffer; the loader
// copies whatever it must keep before the next call, so one scratch is safe to
// reuse across a locale's files. (The "how to get bytes" seam — see
// locale_loader.h; the device equivalent reads + verifies a flash partition.)
struct FsPackCtx {
    std::string font_dir;
    std::vector<uint8_t> scratch;
};

static bool fs_pack_provider(const char *locale, const char *file,
                             const uint8_t **bytes, size_t *len, void *user) {
    FsPackCtx *ctx = static_cast<FsPackCtx *>(user);
    std::string path = ctx->font_dir + "/" + locale + "/" + file;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "missing pack file: %s\n", path.c_str());
        return false;
    }
    ctx->scratch.assign((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    *bytes = ctx->scratch.data();
    *len = ctx->scratch.size();
    return true;
}

// Register every language pack found under <font_dir> from its own manifest.json, so
// the render layer learns each locale's policy (chain / rtl / shaping / role sizes) at
// runtime. Screens bakes only the English floor; non-English locales are self-described
// by their pack, so a pack simply appearing under lang-packs/ makes its locale render —
// no compiled-in locale table, no recompile. Returns the number registered. (Host-side
// file I/O: the render layer deliberately never opens files — see locale_loader.h.)
static int register_packs_from_dir(const std::string &font_dir) {
    DIR *d = opendir(font_dir.c_str());
    if (!d) return 0;
    // Collect + sort the locale dirs so registration order (and thus the
    // supported_locales_json ordering) is deterministic regardless of the
    // filesystem's readdir order — CI screenshot diffs depend on it.
    std::vector<std::string> locales;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        locales.push_back(name);
    }
    closedir(d);
    std::sort(locales.begin(), locales.end());

    int n = 0;
    for (const std::string &name : locales) {
        std::string manifest = font_dir + "/" + name + "/manifest.json";
        std::ifstream in(manifest, std::ios::binary);
        if (!in) continue;  // not a pack dir (English + catalog-only locales carry no manifest)
        std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
        if (ss_register_pack_manifest(bytes.data(), bytes.size())) ++n;
    }
    return n;
}

int main(int argc, char **argv) {
    const char *out_dir = DEFAULT_OUT_DIR;
    const char *scenarios_file = DEFAULT_SCENARIOS_FILE;
    bool dump_locales = false;
    const char *shape_spike_dir = NULL;  // --shape-spike: run the throwaway shaping spike and exit
    std::string locale;     // --locale: register per-locale fonts + (caller picks scenarios file)
    std::string font_dir = "lang-packs";  // --font-dir (repo-root production packs)

    // Need an active profile before usage() can call display_profile_count()
    set_display(DISPLAY_WIDTH, DISPLAY_HEIGHT);

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (strcmp(argv[i], "--scenarios-file") == 0 && i + 1 < argc) {
            scenarios_file = argv[++i];
        } else if (strcmp(argv[i], "--dump-locales") == 0) {
            dump_locales = true;
        } else if (strcmp(argv[i], "--shape-spike") == 0 && i + 1 < argc) {
            shape_spike_dir = argv[++i];
        } else if (strcmp(argv[i], "--locale") == 0 && i + 1 < argc) {
            locale = argv[++i];
        } else if (strcmp(argv[i], "--font-dir") == 0 && i + 1 < argc) {
            font_dir = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    // Learn every available locale's policy from its pack manifest. There is no
    // compiled-in locale table anymore — screens bakes only the English floor and
    // discovers non-English locales from the packs under --font-dir. Both --dump-locales
    // and the per-locale render path below read this registered runtime set.
    register_packs_from_dir(font_dir);

    // Emit the canonical per-profile font manifest (the render layer's sole
    // outward interface) and exit. The offline font-pack tooling consumes this
    // instead of duplicating the locale->{font,size,chain} table in Python.
    if (dump_locales) {
        json all = json::object();
        for (int i = 0; i < display_profile_count(); ++i) {
            const DisplayProfile &p = display_profile_at(i);
            set_display(p.width, p.height);
            char key[32];
            snprintf(key, sizeof(key), "%dx%d", p.width, p.height);
            all[key] = json::parse(supported_locales_json());
        }
        printf("%s\n", all.dump(2).c_str());
        return 0;
    }

    // THROWAWAY de-risking spike: load pre-shaped glyph runs + subset fonts from
    // <dir> and render them by glyph-id through the existing tiny_ttf engine.
    // Bypasses the whole scenario/display-profile pipeline.
    if (shape_spike_dir) {
        lv_init();
        return run_shape_spike(shape_spike_dir);
    }

    if (mkdir_p(out_dir) != 0) {
        fprintf(stderr, "Failed creating out dir: %s\n", out_dir);
        return 1;
    }

    // Preserve static files (e.g. index.html), clear image artifacts, overwrite manifest.
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

    const char *im_bin = imagemagick_binary();
    if (im_bin) {
        printf("imagemagick detected (%s): animated GIF generation enabled for scrolling-title scenarios\n", im_bin);
    } else {
        printf("imagemagick not detected (magick/convert): animated GIF generation disabled\n");
    }

    std::vector<scenario_def_t> scenarios;
    if (load_scenarios_file(scenarios_file, scenarios) != 0) {
        const char *fallback = "tools/scenarios/scenarios.json";
        if (strcmp(scenarios_file, fallback) != 0 && load_scenarios_file(fallback, scenarios) == 0) {
            fprintf(stderr, "note: using fallback scenarios file: %s\n", fallback);
        } else {
            fprintf(stderr, "Failed to load scenarios file: %s\n", scenarios_file);
            return 1;
        }
    }

    lv_init();

    // Static screenshots: disable animations (e.g. the text-entry cursor blink)
    // so each frame is deterministic and the cursor is always captured.
    seedsigner_lvgl_set_static_render(true);

    // The locale picker fetches endonym images (endonym_<h>.bin) through the same
    // byte-provider seam as the font loader — reuse the filesystem provider so a
    // settings_locale_picker_screen scenario renders its native-script rows from lang-packs/.
    static FsPackCtx picker_fs_ctx;
    picker_fs_ctx.font_dir = font_dir;
    locale_picker_set_image_provider(fs_pack_provider, &picker_fs_ctx);

    json manifest_resolutions = json::array();

    // Generate screenshots for every compiled-in display profile.
    for (int pi = 0; pi < display_profile_count(); ++pi) {
        const DisplayProfile &profile = display_profile_at(pi);

        set_display(profile.width, profile.height);
        g_width  = profile.width;
        g_height = profile.height;
        g_fb.assign((size_t)g_width * (size_t)g_height, 0);

        // The five translated text roles now render from the compiled-in OpenSans
        // Western TTF baseline, installed by set_display() above (keyboard + icons
        // stay baked). Per-locale script fonts chain on top via the seam below.

        // Render each resolution in its native input mode: the 240px Pi Zero is
        // joystick; larger ESP32 screens are touch. Screens that branch on input
        // mode (e.g. the passphrase keyboard) then show the right experience for
        // that device without per-scenario input overrides.
        input_profile_set_mode(profile.height == 240 ? INPUT_MODE_HARDWARE : INPUT_MODE_TOUCH);

        // Per-locale fonts: restore the previous profile's fonts (destroying the
        // tiny_ttf fonts that reference the old buffers), free those buffers, then
        // register this profile's script font at its per-role sizes. English
        // (empty locale) leaves the compiled-in fonts untouched.
        if (!locale.empty()) {
            // The shared loader clears the previous profile's fonts + glyph runs,
            // then registers this profile's pack (and runs.bin, for shaping
            // locales) via the filesystem provider — one call, all hosts identical.
            FsPackCtx fs_ctx{font_dir, {}};
            if (!ss_load_locale(locale.c_str(), fs_pack_provider, &fs_ctx)) {
                fprintf(stderr, "Failed loading locale '%s' at %dx%d\n",
                        locale.c_str(), g_width, g_height);
                return 1;
            }
        }

        char res_label[32];
        snprintf(res_label, sizeof(res_label), "%dx%d", g_width, g_height);

        printf("\n--- %s ---\n", res_label);

        char res_img_dir[PATH_MAX];
        snprintf(res_img_dir, sizeof(res_img_dir), "%s/img/%s", run_dir, res_label);
        if (mkdir_p(res_img_dir) != 0) {
            fprintf(stderr, "Failed creating image dir: %s\n", res_img_dir);
            return 1;
        }

        // Create LVGL display for this resolution.
        std::vector<uint8_t> draw_buf((size_t)g_width * (size_t)g_height * 2u);
        lv_display_t *disp = lv_display_create(g_width, g_height);
        lv_display_set_flush_cb(disp, lvgl_flush_cb);
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(disp, draw_buf.data(), NULL, draw_buf.size(),
                               LV_DISPLAY_RENDER_MODE_FULL);

        json res_entry = json::object();
        res_entry["width"] = g_width;
        res_entry["height"] = g_height;
        res_entry["label"] = std::string(res_label);
        res_entry["screenshots"] = json::array();

        for (const scenario_def_t &scenario : scenarios) {
            try {
                // The dedicated digits page only exists at 240px (larger screens
                // keep digits in the number row), so a "digits" variation above
                // 240px would just duplicate the lowercase base — skip it.
                if (profile.height > 240 &&
                    scenario.context.value("initial_mode", std::string()) == "digits") {
                    continue;
                }

                if (!render_scenario_def(scenario)) {
                    fprintf(stderr, "Unknown/invalid scenario: %s\n", scenario.name.c_str());
                    return 2;
                }

                lv_timer_handler();
                lv_refr_now(disp);

                std::vector<uint8_t> rgb;
                framebuffer_to_rgb24(rgb);

                std::string file_base = scenario.name + "_" + res_label;

                char out_png[PATH_MAX];
                snprintf(out_png, sizeof(out_png), "%s/%s.png", res_img_dir, file_base.c_str());
                if (write_png_rgb24(out_png, rgb.data(), g_width, g_height) != 0) {
                    fprintf(stderr, "Failed writing PNG: %s\n", out_png);
                    return 1;
                }
                printf("wrote %s\n", out_png);

                bool want_gif = im_bin && scenario_is_animated(scenario);
                double anim_seconds = scenario_gif_seconds(scenario);
                if (maybe_write_scroll_gif(res_img_dir, file_base, disp, want_gif ? im_bin : NULL, anim_seconds) != 0) {
                    fprintf(stderr, "Failed writing animated GIF for scenario: %s\n", scenario.name.c_str());
                    return 1;
                }

                // Check if GIF was actually produced.
                char gif_path[PATH_MAX];
                snprintf(gif_path, sizeof(gif_path), "%s/%s.gif", res_img_dir, file_base.c_str());
                struct stat gif_st;
                bool has_gif = (stat(gif_path, &gif_st) == 0) && S_ISREG(gif_st.st_mode);
                const char *ext = has_gif ? "gif" : "png";

                if (has_gif) {
                    printf("wrote %s/%s.gif\n", res_img_dir, file_base.c_str());
                }

                json item = json::object();
                item["name"] = scenario.name;
                item["display_name"] = scenario_display_name(scenario.name) + (has_gif ? " [animated]" : "");
                item["path"] = std::string("img/") + res_label + "/" + file_base + "." + ext;
                res_entry["screenshots"].push_back(item);

            } catch (const std::exception &e) {
                fprintf(stderr, "Scenario %s threw exception: %s\n", scenario.name.c_str(), e.what());
                return 3;
            } catch (...) {
                fprintf(stderr, "Scenario %s threw unknown exception\n", scenario.name.c_str());
                return 3;
            }
        }

        lv_display_delete(disp);
        manifest_resolutions.push_back(res_entry);

        printf("done: %zu scenario(s), %s\n", scenarios.size(), res_label);
    }

    if (write_manifest_json(run_dir, ts, manifest_resolutions) != 0) {
        fprintf(stderr, "Failed writing manifest.json in %s\n", run_dir);
        return 1;
    }

    printf("\ndone: %d resolution(s), %zu scenario(s) each\n",
           display_profile_count(), scenarios.size());
    return 0;
}
