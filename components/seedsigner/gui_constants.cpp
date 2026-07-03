#include "gui_constants.h"
#include "fonts/opensans_western.h"
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Helper: scale a base pixel value by a PX_MULTIPLIER
// ---------------------------------------------------------------------------
static int px_scale(int base, int multiplier) {
    return static_cast<int>(base * multiplier / 100.0);
}

// ---------------------------------------------------------------------------
// Font selection by multiplier. Font files are named by a px_multiplier suffix:
// unsuffixed = PX_MULTIPLIER_100, _133x = PX_MULTIPLIER_133 (320px-height, sized
// at the geometric 320/240 ratio), _200x = PX_MULTIPLIER_200.
// ---------------------------------------------------------------------------
struct FontSet {
    const lv_font_t* main_menu_title;
    const lv_font_t* title;
    const lv_font_t* large_button;
    const lv_font_t* button;
    const lv_font_t* body;
    const lv_font_t* icon;
    const lv_font_t* icon_large;
    const lv_font_t* icon_primary_screen;  // 48 px base; scales with multiplier
    const lv_font_t* keyboard;             // fixed-width keyboard/text-entry font
    const lv_font_t* candidate;            // fixed-width 22px base; mnemonic candidate list
    const lv_font_t* top_nav_icon;         // 26 px base (ICON_FONT_SIZE+4); top-nav title icon
};

// The five translated text-role fonts (main_menu_title, title, large_button,
// button, body) are NOT baked here: they are rasterized at runtime from the
// compiled-in OpenSans Western TTF and installed by set_display() (see
// install_western_baseline() below). fonts_for_multiplier() leaves those five
// slots null and supplies only the baked, never-translated fonts: the seedsigner
// icons (PUA) and the fixed-width Inconsolata keyboard/text-entry font (ASCII).
static FontSet fonts_for_multiplier(int px_mult) {
#ifdef SUPPORT_DISPLAY_HEIGHT_480
    if (px_mult == PX_MULTIPLIER_200) {
        return {
            nullptr,  // main_menu_title  } OpenSans Western TTF,
            nullptr,  // title            } installed per-role by
            nullptr,  // large_button     } set_display() once LVGL
            nullptr,  // button           } is initialized.
            nullptr,  // body             }
            &seedsigner_icons_24_4bpp_200x,
            &seedsigner_icons_36_4bpp_200x,   // icon_large unchanged
            &seedsigner_icons_48_4bpp_200x,   // icon_primary_screen = 96px
            &inconsolata_semibold_24_4bpp_200x,  // keyboard = 48px
            &inconsolata_semibold_22_4bpp_200x,  // candidate = 44px (22 x 2)
            &seedsigner_icons_26_4bpp_200x,   // top_nav_icon = 52px (26 x 2)
        };
    }
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_320
    if (px_mult == PX_MULTIPLIER_133) {
        // _133x fonts = 133-scaled (= base x 320/240). See gui_constants.h.
        return {
            nullptr,  // main_menu_title  } OpenSans Western TTF,
            nullptr,  // title            } installed per-role by
            nullptr,  // large_button     } set_display() once LVGL
            nullptr,  // button           } is initialized.
            nullptr,  // body             }
            &seedsigner_icons_24_4bpp_133x,      // icon          = 32px (24 x 1.333)
            &seedsigner_icons_36_4bpp_133x,      // icon_large    = 48px (36 x 1.333)
            &seedsigner_icons_48_4bpp_133x,      // icon_primary  = 64px (48 x 1.333)
            &inconsolata_semibold_24_4bpp_133x,  // keyboard      = 32px (24 x 1.333)
            &inconsolata_semibold_22_4bpp_133x,  // candidate     = 29px (22 x 1.333)
            &seedsigner_icons_26_4bpp_133x,      // top_nav_icon  = 35px (26 x 1.333)
        };
    }
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_240
    if (px_mult == PX_MULTIPLIER_100) {
        return {
            nullptr,  // main_menu_title  } OpenSans Western TTF,
            nullptr,  // title            } installed per-role by
            nullptr,  // large_button     } set_display() once LVGL
            nullptr,  // button           } is initialized.
            nullptr,  // body             }
            &seedsigner_icons_24_4bpp,
            &seedsigner_icons_48_4bpp,         // icon_large = 48px
            &seedsigner_icons_48_4bpp,         // icon_primary_screen = 48px
            &inconsolata_semibold_24_4bpp,     // keyboard = 24px (matches Python passphrase keyboard)
            &inconsolata_semibold_22_4bpp,     // candidate = 22px (button_font_size + 4)
            &seedsigner_icons_26_4bpp,         // top_nav_icon = 26px (ICON_FONT_SIZE+4)
        };
    }
#endif
    fprintf(stderr, "FATAL: no fonts for PX_MULTIPLIER=%d\n", px_mult);
    abort();
}

static DisplayProfile make_profile(int width, int height) {
    int px_mult;
    switch (height) {
        case 240: px_mult = PX_MULTIPLIER_100; break;
        case 320: px_mult = PX_MULTIPLIER_133; break;
        case 480: px_mult = PX_MULTIPLIER_200; break;
        default:
            fprintf(stderr, "FATAL: no PX_MULTIPLIER for height=%d\n", height);
            abort();
    }
    int comp_pad   = px_scale(8, px_mult);
    int title_size = px_scale(20, px_mult);
    int min_size   = px_scale(15, px_mult);
    FontSet fonts  = fonts_for_multiplier(px_mult);

    return {
        width, height, px_mult,
        px_scale(8, px_mult),    // edge_padding
        comp_pad,                // component_padding
        px_scale(4, px_mult),    // list_item_padding
        px_scale(22, px_mult),   // icon_font_size
        px_scale(24, px_mult),   // icon_inline_font_size
        px_scale(36, px_mult),   // icon_large_button_size
        px_scale(50, px_mult),   // icon_primary_screen_size
        title_size,              // top_nav_title_font_size
        px_scale(48, px_mult),   // top_nav_height
        px_scale(32, px_mult),   // top_nav_button_size
        px_scale(17, px_mult),   // body_font_size
        title_size,              // body_font_max_size (= top_nav_title_font_size)
        min_size,                // body_font_min_size
        comp_pad,                // body_line_spacing (= component_padding)
        min_size,                // label_font_size (= body_font_min_size)
        px_scale(18, px_mult),   // button_font_size
        px_scale(32, px_mult),   // button_height
        px_scale(8, px_mult),    // button_radius
        fonts.main_menu_title, fonts.title, fonts.large_button, fonts.button, fonts.body, fonts.icon, fonts.icon_large, fonts.icon_primary_screen, fonts.keyboard, fonts.candidate, fonts.top_nav_icon,
    };
}

// ---------------------------------------------------------------------------
// Profile instances for each supported resolution
// ---------------------------------------------------------------------------
// Profiles are non-const so the font-registration seam (font_registry.cpp) can
// repoint their text-font pointers to per-locale script fonts at runtime. All
// other access is through active_profile()/the macros, which expose them as const.
#ifdef SUPPORT_DISPLAY_HEIGHT_240
static DisplayProfile profile_240x240 = make_profile(240, 240);
static DisplayProfile profile_320x240 = make_profile(320, 240);
#endif

#ifdef SUPPORT_DISPLAY_HEIGHT_320
static DisplayProfile profile_480x320 = make_profile(480, 320);
#endif

#ifdef SUPPORT_DISPLAY_HEIGHT_480
static DisplayProfile profile_800x480 = make_profile(800, 480);
#endif

// ---------------------------------------------------------------------------
// Profile lookup table
// ---------------------------------------------------------------------------
struct ProfileEntry {
    int width;
    int height;
    DisplayProfile* profile;
};

static const ProfileEntry profile_table[] = {
#ifdef SUPPORT_DISPLAY_HEIGHT_240
    {240, 240, &profile_240x240},
    {320, 240, &profile_320x240},
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_320
    {480, 320, &profile_480x320},
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_480
    {800, 480, &profile_800x480},
#endif
};

static const int profile_count = sizeof(profile_table) / sizeof(profile_table[0]);

static DisplayProfile* find_profile(int width, int height) {
    for (int i = 0; i < profile_count; ++i) {
        if (profile_table[i].width == width && profile_table[i].height == height) {
            return profile_table[i].profile;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Active profile state
// ---------------------------------------------------------------------------
static DisplayProfile* g_active_profile = nullptr;

const DisplayProfile& active_profile() {
    if (!g_active_profile) {
        fprintf(stderr, "FATAL: active_profile() called before set_display()\n");
        abort();
    }
    return *g_active_profile;
}

DisplayProfile& active_profile_mutable() {
    if (!g_active_profile) {
        fprintf(stderr, "FATAL: active_profile_mutable() called before set_display()\n");
        abort();
    }
    return *g_active_profile;
}

// ---------------------------------------------------------------------------
// Baked image selection by multiplier — same shape as fonts_for_multiplier().
// Each branch is #ifdef-gated in lockstep with the LV_IMAGE_DECLAREs in
// gui_constants.h and the CMake source lists, so a single-height build contains
// (and references) only its own variant.
// ---------------------------------------------------------------------------
const lv_image_dsc_t* seedsigner_logo_for_active_profile() {
    const int px_mult = active_profile().px_multiplier;
#ifdef SUPPORT_DISPLAY_HEIGHT_480
    if (px_mult == PX_MULTIPLIER_200) return &seedsigner_logo_img_200x;
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_320
    if (px_mult == PX_MULTIPLIER_133) return &seedsigner_logo_img_133x;  // 320px-height profile (base x 320/240)
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_240
    if (px_mult == PX_MULTIPLIER_100) return &seedsigner_logo_img;
#endif
    fprintf(stderr, "FATAL: no seedsigner logo for PX_MULTIPLIER=%d\n", px_mult);
    abort();
}

const lv_image_dsc_t* hrf_logo_for_active_profile() {
    const int px_mult = active_profile().px_multiplier;
#ifdef SUPPORT_DISPLAY_HEIGHT_480
    if (px_mult == PX_MULTIPLIER_200) return &hrf_logo_img_200x;
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_320
    if (px_mult == PX_MULTIPLIER_133) return &hrf_logo_img_133x;  // 320px-height profile (base x 320/240)
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_240
    if (px_mult == PX_MULTIPLIER_100) return &hrf_logo_img;
#endif
    fprintf(stderr, "FATAL: no HRF logo for PX_MULTIPLIER=%d\n", px_mult);
    abort();
}

const lv_image_dsc_t* btc_logo_for_active_profile() {
    const int px_mult = active_profile().px_multiplier;
#ifdef SUPPORT_DISPLAY_HEIGHT_480
    if (px_mult == PX_MULTIPLIER_200) return &btc_logo_img_200x;  // 120px (60 x 2)
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_320
    if (px_mult == PX_MULTIPLIER_133) return &btc_logo_img_133x;  // 80px (60 x 1.333)
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_240
    if (px_mult == PX_MULTIPLIER_100) return &btc_logo_img;       // 60px (Python btc_logo_60x60)
#endif
    fprintf(stderr, "FATAL: no BTC logo for PX_MULTIPLIER=%d\n", px_mult);
    abort();
}

// ---------------------------------------------------------------------------
// Compiled-in OpenSans Western baseline for the five translated text roles
// ---------------------------------------------------------------------------
// The five role fonts (main_menu_title, top_nav_title, large_button, button,
// body) render *translated* UI labels, so they are rasterized at runtime from
// the compiled-in OpenSans Western-Latin TTF via lv_tiny_ttf — covering accented
// Latin (á / ñ / ¿ / ¡) that the old ASCII-only baked bitmaps rendered as boxes.
// The keyboard and icon fonts stay baked: they render only ASCII / PUA glyphs,
// never translated text, and carry the security-sensitive / user-controlled
// input, so they stay off the rasterizer.
//
// fonts_for_multiplier() leaves these five slots null at static-init time (when
// LVGL is not yet initialized). The fonts are created the first time set_display()
// runs after lv_init(), one tiny_ttf per role at that profile's per-role px. They
// are cached per profile and NEVER destroyed: the locale font seam
// (font_registry.cpp) captures these pointers as the "original" font to restore
// on locale clear, so they must stay valid for the whole process lifetime.
//
// Per-role px replicates the old baked bitmap sizes EXACTLY for parity (modulo
// bitmap -> rasterized anti-aliasing). The one quirk: large_button was 20 px base
// at the 240-height profile but 18 px base at 320 / 480 — keyed off px_multiplier
// below. Weights: body = Regular; the other four = SemiBold. The sizes are
// hardcoded here (rather than pulled from locale_fonts) to keep gui_constants
// free of any dependency on the locale layer, which itself includes this header.
#if LV_USE_TINY_TTF

// Profiles whose baseline fonts are already installed. The install is idempotent
// and must never recreate a profile's fonts (stable pointers; never destroyed),
// so re-entering set_display() for an already-installed profile is a no-op. At
// most one entry per compiled-in profile.
static const DisplayProfile* g_baseline_installed[8] = {nullptr};
static int g_baseline_installed_count = 0;

static bool baseline_installed(const DisplayProfile* p) {
    for (int i = 0; i < g_baseline_installed_count; ++i) {
        if (g_baseline_installed[i] == p) return true;
    }
    return false;
}

static void install_western_baseline(DisplayProfile& p) {
    if (baseline_installed(&p)) return;

    const int mult = p.px_multiplier;

    // large_button: 20 px base at 240 height (PX_MULTIPLIER_100), 18 px at 320/480.
    const int large_button_base = (mult == PX_MULTIPLIER_100) ? 20 : 18;

    struct RoleSpec {
        const lv_font_t* DisplayProfile::* field;
        int  base_size;
        bool semibold;
    };
    const RoleSpec roles[] = {
        { &DisplayProfile::main_menu_title_font, 26,                true  },
        { &DisplayProfile::top_nav_title_font,   20,                true  },
        { &DisplayProfile::large_button_font,    large_button_base, true  },
        { &DisplayProfile::button_font,          18,                true  },
        { &DisplayProfile::body_font,            17,                false },
    };

    // Multiple roles can resolve to the same (weight, px) after scaling — e.g.
    // top_nav_title and large_button both land on 20 px SemiBold at the 240-height
    // profile; button and large_button on 23 px at 320. tiny_ttf would rasterize a
    // byte-identical font for each, and every instance carries its own set of glyph
    // caches on the constrained internal pool. So create one instance per distinct
    // (semibold, px) and point colliding roles at the shared font. These baseline
    // fonts are process-lifetime and never destroyed, so sharing here is purely
    // "don't build the duplicate" — no lifecycle bookkeeping needed.
    struct Created { int px; bool semibold; lv_font_t* font; };
    Created created[sizeof(roles) / sizeof(roles[0])];
    int created_count = 0;

    for (const RoleSpec& r : roles) {
        const int px = px_scale(r.base_size, mult);

        // Reuse an instance already built for this (semibold, px) in this profile.
        lv_font_t* f = nullptr;
        for (int i = 0; i < created_count; ++i) {
            if (created[i].px == px && created[i].semibold == r.semibold) {
                f = created[i].font;
                break;
            }
        }

        if (!f) {
            const uint8_t* data = r.semibold ? opensans_western_semibold_ttf
                                             : opensans_western_regular_ttf;
            const size_t   len  = r.semibold ? opensans_western_semibold_ttf_len
                                             : opensans_western_regular_ttf_len;

            // KERNING_NONE: our closed label corpus doesn't need pair kerning.
            // Cache size = SEEDSIGNER_TTF_CACHE_SIZE (gui_constants.h): enabled by
            // default. The cache retains bitmaps, so every target must back LVGL with
            // adequate RAM/PSRAM; against a too-small fixed pool it OOMs (→ assert-
            // handler spin). See docs/knowledge/tiny-ttf-cache-spin-root-cause.md.
            f = lv_tiny_ttf_create_data_ex(data, len, px,
                                           LV_FONT_KERNING_NONE,
                                           SEEDSIGNER_TTF_CACHE_SIZE);
            if (!f) {
                fprintf(stderr, "FATAL: failed to rasterize OpenSans Western baseline "
                                "(role px=%d)\n", px);
                abort();
            }
            created[created_count++] = {px, r.semibold, f};
        }

        p.*(r.field) = f;
    }

    if (g_baseline_installed_count <
        (int)(sizeof(g_baseline_installed) / sizeof(g_baseline_installed[0]))) {
        g_baseline_installed[g_baseline_installed_count++] = &p;
    }
}

// Memoized regular-weight OpenSans font at an arbitrary (already-scaled) px, for
// body-relative sizes the profile roles don't cover. Never destroyed — the cache
// is process-lifetime, matching install_western_baseline's stable-pointer contract.
const lv_font_t* seedsigner_latin_font(int base_px_size) {
    const int px = px_scale(base_px_size, active_profile().px_multiplier);

    struct Entry { int px; lv_font_t* font; };
    static Entry cache[16];
    static int   cache_count = 0;

    for (int i = 0; i < cache_count; ++i) {
        if (cache[i].px == px) return cache[i].font;
    }

    // KERNING_NONE + the shared cache size, exactly like the baseline install.
    lv_font_t* f = lv_tiny_ttf_create_data_ex(opensans_western_regular_ttf,
                                              opensans_western_regular_ttf_len,
                                              px, LV_FONT_KERNING_NONE,
                                              SEEDSIGNER_TTF_CACHE_SIZE);
    if (!f) {
        // Rasterization failed (should not happen); degrade to the body font so the
        // caller still renders something rather than crashing.
        return active_profile().body_font;
    }
    if (cache_count < (int)(sizeof(cache) / sizeof(cache[0]))) {
        cache[cache_count++] = { px, f };
    }
    return f;
}

#else  // !LV_USE_TINY_TTF

const lv_font_t* seedsigner_latin_font(int /*base_px_size*/) {
    return active_profile().body_font;
}

#endif  // LV_USE_TINY_TTF

void set_display(int width, int height) {
    DisplayProfile* p = find_profile(width, height);
    if (!p) {
        fprintf(stderr, "FATAL: no display profile for %dx%d\n", width, height);
        abort();
    }
    g_active_profile = p;

#if LV_USE_TINY_TTF
    // Install the compiled-in OpenSans Western baseline for the five translated
    // text roles, the first time we're called after lv_init(). Pre-lv_init
    // callers (e.g. the generator's usage() / --dump-locales path) skip this —
    // they never render, and the install runs on the next, post-lv_init
    // set_display() before any screen is drawn.
    if (lv_is_initialized()) {
        install_western_baseline(*p);
    }
#endif
}

int display_profile_count() {
    return profile_count;
}

const DisplayProfile& display_profile_at(int index) {
    if (index < 0 || index >= profile_count) {
        fprintf(stderr, "FATAL: display_profile_at(%d) out of range [0, %d)\n", index, profile_count);
        abort();
    }
    return *profile_table[index].profile;
}
