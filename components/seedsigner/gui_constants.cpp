#include "gui_constants.h"
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Helper: scale a base pixel value by a PX_MULTIPLIER
// ---------------------------------------------------------------------------
static int px_scale(int base, int multiplier) {
    return static_cast<int>(base * multiplier / 100.0);
}

// ---------------------------------------------------------------------------
// Font selection by multiplier. Font files are named by their multiplier value:
// unsuffixed = PX_MULTIPLIER_100, _150x = PX_MULTIPLIER_150.
// ---------------------------------------------------------------------------
struct FontSet {
    const lv_font_t* main_menu_title;
    const lv_font_t* title;
    const lv_font_t* large_button;
    const lv_font_t* button;
    const lv_font_t* body;
    const lv_font_t* icon;
    const lv_font_t* icon_large;
};

static FontSet fonts_for_multiplier(int px_mult) {
#ifdef SUPPORT_DISPLAY_HEIGHT_480
    if (px_mult == PX_MULTIPLIER_200) {
        return {
            &opensans_semibold_26_4bpp_200x,
            &opensans_semibold_20_4bpp_200x,
            &opensans_semibold_18_4bpp_200x,  // large_button = button font (unchanged)
            &opensans_semibold_18_4bpp_200x,
            &opensans_regular_17_4bpp_200x,
            &seedsigner_icons_24_4bpp_200x,
            &seedsigner_icons_36_4bpp_200x,   // icon_large unchanged
        };
    }
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_320
    if (px_mult == PX_MULTIPLIER_150) {
        return {
            &opensans_semibold_26_4bpp_150x,  // main_menu_title = 39px
            &opensans_semibold_20_4bpp_150x,
            &opensans_semibold_18_4bpp_150x,  // large_button = button font (unchanged)
            &opensans_semibold_18_4bpp_150x,
            &opensans_regular_17_4bpp_150x,
            &seedsigner_icons_24_4bpp_150x,
            &seedsigner_icons_36_4bpp_150x,   // icon_large unchanged
        };
    }
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_240
    if (px_mult == PX_MULTIPLIER_100) {
        return {
            &opensans_semibold_26_4bpp,        // main_menu_title = 26px
            &opensans_semibold_20_4bpp,
            &opensans_semibold_20_4bpp,        // large_button = title font (20px)
            &opensans_semibold_18_4bpp,
            &opensans_regular_17_4bpp,
            &seedsigner_icons_24_4bpp,
            &seedsigner_icons_48_4bpp,         // icon_large = 48px
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
        case 320: px_mult = PX_MULTIPLIER_150; break;
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
        fonts.main_menu_title, fonts.title, fonts.large_button, fonts.button, fonts.body, fonts.icon, fonts.icon_large,
    };
}

// ---------------------------------------------------------------------------
// Profile instances for each supported resolution
// ---------------------------------------------------------------------------
#ifdef SUPPORT_DISPLAY_HEIGHT_240
static const DisplayProfile profile_240x240 = make_profile(240, 240);
static const DisplayProfile profile_320x240 = make_profile(320, 240);
#endif

#ifdef SUPPORT_DISPLAY_HEIGHT_320
static const DisplayProfile profile_480x320 = make_profile(480, 320);
#endif

#ifdef SUPPORT_DISPLAY_HEIGHT_480
static const DisplayProfile profile_800x480 = make_profile(800, 480);
#endif

// ---------------------------------------------------------------------------
// Profile lookup table
// ---------------------------------------------------------------------------
struct ProfileEntry {
    int width;
    int height;
    const DisplayProfile* profile;
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

static const DisplayProfile* find_profile(int width, int height) {
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
static const DisplayProfile* g_active_profile = nullptr;

const DisplayProfile& active_profile() {
    if (!g_active_profile) {
        fprintf(stderr, "FATAL: active_profile() called before set_display()\n");
        abort();
    }
    return *g_active_profile;
}

void set_display(int width, int height) {
    const DisplayProfile* p = find_profile(width, height);
    if (!p) {
        fprintf(stderr, "FATAL: no display profile for %dx%d\n", width, height);
        abort();
    }
    g_active_profile = p;
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
