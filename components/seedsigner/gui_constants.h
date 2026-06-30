#ifndef GUI_CONSTANTS_H
#define GUI_CONSTANTS_H

/*
 * PX_MULTIPLIER: UI scaling factor derived from the target display height.
 *
 * All layout constants (padding, button height, font sizes, etc.) are defined as
 * base values x PX_MULTIPLIER / 100. The base values were originally designed for
 * the Pi Zero's 240px-height display, so PX_MULTIPLIER=100 is a no-op: the UI
 * renders at exactly the dimensions originally defined for that hardware.
 *
 * At 320px height (3.5" display) we use PX_MULTIPLIER=133 — the direct 320/240
 * ratio (4/3). This matches the geometric scaling logic that makes the 480px
 * profile work (below). An earlier revision used 150 ("aesthetic upscale"), but
 * that over-scaled the whole profile ~13% — fonts and layout came out too large
 * (the keyboard font had to be hand-eased to fit its keys, a symptom of the
 * over-scale). 133 is the correct, consistent value.
 *
 * At 480px height (4.3" display) we use PX_MULTIPLIER=200 — the direct 480/240
 * ratio (2x). UI elements come out matched in physical size (measured with a tape
 * measure) between the 3.5" (165 DPI) and 4.3" (217 DPI) displays.
 *
 * The height-to-PX_MULTIPLIER mapping lives here so it is consistent across all
 * build targets (ESP32, Pi Zero, desktop tools). Callers only need to supply
 * DISPLAY_WIDTH and DISPLAY_HEIGHT; the build system passes one or more
 * SUPPORT_DISPLAY_HEIGHT_* flags to control which profiles and fonts are compiled
 * in. Hardware builds set a single flag; desktop tools set all of them so users
 * can switch resolutions at runtime.
 */

#include <string>
#include "lvgl.h"

#if !defined(SUPPORT_DISPLAY_HEIGHT_240) && !defined(SUPPORT_DISPLAY_HEIGHT_320) && !defined(SUPPORT_DISPLAY_HEIGHT_480)
#error "At least one SUPPORT_DISPLAY_HEIGHT_* flag must be defined"
#endif

// SEEDSIGNER_TTF_CACHE_SIZE: glyph/draw cache entries per runtime tiny_ttf font.
// Enabled by default for redraw/scroll speed; every target (desktop, browser,
// and device) is expected to back LVGL with adequate RAM/PSRAM. The cache retains
// rasterized bitmaps — the only hazard is running it against a too-small fixed
// pool, where OOM turns into a CPU spin via LVGL's default assert handler (see
// docs/knowledge/tiny-ttf-cache-spin-root-cause.md). A build that genuinely runs
// on a tiny fixed pool can override this to 0 (rasterize each draw, no cache).
#ifndef SEEDSIGNER_TTF_CACHE_SIZE
#define SEEDSIGNER_TTF_CACHE_SIZE 256
#endif

// Named PX_MULTIPLIER values — one per supported display height.
const int PX_MULTIPLIER_100 = 100;   // 240px height (Pi Zero): no scaling
const int PX_MULTIPLIER_133 = 133;   // 320px height (3.5" ESP32): direct 320/240 ratio
const int PX_MULTIPLIER_200 = 200;   // 480px height (4.3" ESP32): direct 480/240 ratio (2x)

// Auto-scroll feel for an overflowing single-line label (top-nav title; long
// status headline). Hold the line start-justified for an initial beat so the reader
// can absorb the start, then continuously marquee-scroll (circular wrap) at a TRUE
// constant px/sec (the helper sets an explicit per-line duration = distance / speed,
// not LVGL's speed encoding which caps the duration ~10 s and lets long lines run
// fast), holding again each time it wraps back to the start. The speed matches the
// Python (PIL) screens' horizontal_scroll_speed of 40 px/sec (smooth on a Pi Zero);
// the circular wrap is preferred over Python's back-and-forth ping-pong, and the
// per-loop start hold is the part of Python's feel worth keeping. Speed is px/sec at
// the Pi Zero reference (PX_MULTIPLIER=100) and scaled for taller displays so the
// visual speed is constant; the hold is wall-clock and does not scale.
const int LINE_SCROLL_PX_PER_SEC    = 40;    // matches Python horizontal_scroll_speed
const int LINE_SCROLL_BEGIN_HOLD_MS = 1000;  // initial hold + hold on each wrap to start (1 s)
const int LINE_SCROLL_MIN_MS        = 300;   // floor on the resolved scroll duration (tiny overflows)

// ---------------------------------------------------------------------------
// Font declarations for supported display heights
// ---------------------------------------------------------------------------
// Only the baked, never-translated fonts are declared here: the seedsigner icon
// fonts (PUA glyphs) and the fixed-width Inconsolata keyboard/text-entry font
// (ASCII). The five translated text-role fonts come from the compiled-in OpenSans
// Western TTF, rasterized at runtime and installed by set_display() — see
// install_western_baseline() in gui_constants.cpp.
#ifdef SUPPORT_DISPLAY_HEIGHT_240
LV_FONT_DECLARE(seedsigner_icons_48_4bpp);
LV_FONT_DECLARE(seedsigner_icons_24_4bpp);
LV_FONT_DECLARE(seedsigner_icons_36_4bpp);
LV_FONT_DECLARE(seedsigner_icons_26_4bpp);
LV_FONT_DECLARE(inconsolata_semibold_24_4bpp);
#endif

// The "_133x" suffix matches the 320px-height profile's PX_MULTIPLIER_133
// (geometric 320/240); these fonts are sized accordingly (icons 32/48/64,
// keyboard 32).
#ifdef SUPPORT_DISPLAY_HEIGHT_320
LV_FONT_DECLARE(seedsigner_icons_24_4bpp_133x);
LV_FONT_DECLARE(seedsigner_icons_36_4bpp_133x);
LV_FONT_DECLARE(seedsigner_icons_48_4bpp_133x);
LV_FONT_DECLARE(seedsigner_icons_26_4bpp_133x);
LV_FONT_DECLARE(inconsolata_semibold_24_4bpp_133x);
#endif

#ifdef SUPPORT_DISPLAY_HEIGHT_480
LV_FONT_DECLARE(seedsigner_icons_24_4bpp_200x);
LV_FONT_DECLARE(seedsigner_icons_36_4bpp_200x);
LV_FONT_DECLARE(seedsigner_icons_48_4bpp_200x);
LV_FONT_DECLARE(seedsigner_icons_26_4bpp_200x);
LV_FONT_DECLARE(inconsolata_semibold_24_4bpp_200x);
#endif

// ---------------------------------------------------------------------------
// Image declarations for supported display heights
// ---------------------------------------------------------------------------
// Baked RGB565 logo assets, one per display-height profile, scaled by the same
// PX_MULTIPLIER as the fonts (base 100, "_133x" = 133, "_200x" = 200 — the suffix
// is the px_multiplier). The SeedSigner wordmark is
// shared by the screensaver and the opening splash; the HRF partner logo is used
// only by the splash's partner band. Pick the right variant at runtime via
// seedsigner_logo_for_active_profile() / hrf_logo_for_active_profile() so a
// single-height build compiles only its own variant (selectors are #ifdef-gated
// in lockstep with these declarations and the CMake source lists).
#ifdef SUPPORT_DISPLAY_HEIGHT_240
LV_IMAGE_DECLARE(seedsigner_logo_img);
LV_IMAGE_DECLARE(hrf_logo_img);
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_320
LV_IMAGE_DECLARE(seedsigner_logo_img_133x);
LV_IMAGE_DECLARE(hrf_logo_img_133x);
#endif
#ifdef SUPPORT_DISPLAY_HEIGHT_480
LV_IMAGE_DECLARE(seedsigner_logo_img_200x);
LV_IMAGE_DECLARE(hrf_logo_img_200x);
#endif

// ---------------------------------------------------------------------------
// Display profile: all resolution-dependent layout constants and fonts
// ---------------------------------------------------------------------------
struct DisplayProfile {
    int width;
    int height;
    int px_multiplier;

    int edge_padding;
    int component_padding;
    int list_item_padding;

    int icon_font_size;
    int icon_inline_font_size;
    int icon_large_button_size;
    int icon_primary_screen_size;

    int top_nav_title_font_size;
    int top_nav_height;
    int top_nav_button_size;

    int body_font_size;
    int body_font_max_size;
    int body_font_min_size;
    int body_line_spacing;

    int label_font_size;

    int button_font_size;
    int button_height;
    int button_radius;

    const lv_font_t* main_menu_title_font;
    const lv_font_t* top_nav_title_font;
    const lv_font_t* large_button_font;
    const lv_font_t* button_font;
    const lv_font_t* body_font;
    const lv_font_t* icon_font;
    const lv_font_t* icon_large_button_font;

    // 48 px (base) seedsigner icon font, used as the hero icon on
    // status/error screens. Scales 1.333x at 320 height (64px), 2x at 480 (96px).
    const lv_font_t* icon_primary_screen_font;

    // Fixed-width font (Inconsolata SemiBold, 18 px base) for the on-screen
    // keyboard keys and the passphrase text-entry box. Merged with the
    // LV_SYMBOL_* control glyphs so a single font covers letters + controls.
    const lv_font_t* keyboard_font;

    // 26 px (base) seedsigner icon font for the top-nav contextual icon beside the
    // title (Python TopNav uses ICON_FONT_SIZE + 4 = 26). Scales 35px at 320 height,
    // 52px at 480.
    const lv_font_t* top_nav_icon_font;
};

const DisplayProfile& active_profile();
void set_display(int width, int height);
int display_profile_count();
const DisplayProfile& display_profile_at(int index);

// Resolution-keyed baked image selectors — return the variant matching the
// active profile's px_multiplier (mirrors fonts_for_multiplier()). Used by the
// screensaver (logo) and the opening splash (logo + HRF partner band).
const lv_image_dsc_t* seedsigner_logo_for_active_profile();
const lv_image_dsc_t* hrf_logo_for_active_profile();

// Mutable access to the active profile, for the font-registration seam
// (font_registry.cpp) which repoints text-font pointers to install per-locale
// script fonts. Regular call sites use active_profile()/the macros (const).
// The profile structs are process-global singletons, so a repoint persists
// until seedsigner_clear_registered_fonts() restores the compiled-in fonts.
DisplayProfile& active_profile_mutable();

// ---------------------------------------------------------------------------
// Macro accessors -- existing code uses these names unchanged
// ---------------------------------------------------------------------------
#define EDGE_PADDING              (active_profile().edge_padding)
#define COMPONENT_PADDING         (active_profile().component_padding)
#define LIST_ITEM_PADDING         (active_profile().list_item_padding)

#define ICON_FONT_SIZE            (active_profile().icon_font_size)
#define ICON_INLINE_FONT_SIZE     (active_profile().icon_inline_font_size)
#define ICON_LARGE_BUTTON_SIZE    (active_profile().icon_large_button_size)
#define ICON_PRIMARY_SCREEN_SIZE  (active_profile().icon_primary_screen_size)

#define TOP_NAV_TITLE_FONT_SIZE   (active_profile().top_nav_title_font_size)
#define TOP_NAV_HEIGHT            (active_profile().top_nav_height)
#define TOP_NAV_BUTTON_SIZE       (active_profile().top_nav_button_size)

#define BODY_FONT_SIZE            (active_profile().body_font_size)
#define BODY_FONT_MAX_SIZE        (active_profile().body_font_max_size)
#define BODY_FONT_MIN_SIZE        (active_profile().body_font_min_size)
#define BODY_LINE_SPACING         (active_profile().body_line_spacing)

#define LABEL_FONT_SIZE           (active_profile().label_font_size)

#define BUTTON_FONT_SIZE          (active_profile().button_font_size)
#define BUTTON_HEIGHT             (active_profile().button_height)
#define BUTTON_RADIUS             (active_profile().button_radius)

#define MAIN_MENU_TITLE_FONT               (*active_profile().main_menu_title_font)
#define TOP_NAV_TITLE_FONT                 (*active_profile().top_nav_title_font)
#define LARGE_BUTTON_FONT                  (*active_profile().large_button_font)
#define BUTTON_FONT                        (*active_profile().button_font)
#define BODY_FONT                          (*active_profile().body_font)
#define ICON_FONT__SEEDSIGNER              (*active_profile().icon_font)
#define ICON_LARGE_BUTTON_FONT__SEEDSIGNER (*active_profile().icon_large_button_font)
#define ICON_PRIMARY_SCREEN_FONT__SEEDSIGNER (*active_profile().icon_primary_screen_font)
#define KEYBOARD_FONT                      (*active_profile().keyboard_font)
#define TOP_NAV_ICON_FONT__SEEDSIGNER      (*active_profile().top_nav_icon_font)

// ---------------------------------------------------------------------------
// Non-scaling constants (colors, font names, etc.)
// ---------------------------------------------------------------------------
const int BACKGROUND_COLOR = 0x000000;
const int WARNING_COLOR = 0xffd60a;
const int DIRE_WARNING_COLOR = 0xff5700;
const int ERROR_COLOR = 0xff1b0a;
const int SUCCESS_COLOR = 0x30d158;
const int BITCOIN_ORANGE = 0xff9416;
const int ACCENT_COLOR = 0xff9f0a;
const int TESTNET_COLOR = 0x00f100;
const int REGTEST_COLOR = 0x00caf1;

const int BODY_FONT_COLOR = 0xf8f8f8;

static constexpr const char* FIXED_WIDTH_EMPHASIS_FONT_NAME = "Inconsolata-SemiBold";

const int LABEL_FONT_COLOR = 0x707070;

const int BUTTON_FONT_COLOR = 0xe8e8e8;
const int BUTTON_BACKGROUND_COLOR = 0x2c2c2c;
const int BUTTON_SELECTED_FONT_COLOR = 0x000000;

const int NOTIFICATION_COLOR = 0x00f100;

// Inactive/disabled gray (Python GUIConstants.INACTIVE_COLOR) — used for the
// animated-QR progress-bar track and the "repeated frame" status dot.
const int INACTIVE_COLOR = 0x414141;
// Pure-green progress fill (Python GUIConstants.GREEN_INDICATOR_COLOR) — distinct
// from SUCCESS_COLOR; used for the animated-QR progress-bar fill.
const int GREEN_INDICATOR_COLOR = 0x00ff00;


class SeedSignerIconConstants {
    public:
        // Menu icons
        static constexpr const char* SCAN = "\ue900";
        static constexpr const char* SEEDS = "\ue901";
        static constexpr const char* SETTINGS = "\ue902";
        static constexpr const char* TOOLS = "\ue903";

        // Utility icons
        static constexpr const char* BACK = "\ue904";
        static constexpr const char* CHECK = "\ue905";
        static constexpr const char* CHECKBOX = "\ue906";
        static constexpr const char* CHECKBOX_SELECTED = "\ue907";
        static constexpr const char* CHEVRON_DOWN = "\ue908";
        static constexpr const char* CHEVRON_LEFT = "\ue909";
        static constexpr const char* CHEVRON_RIGHT = "\ue90a";
        static constexpr const char* CHEVRON_UP = "\ue90b";
        // static constexpr const char* CLOSE = "\ue90c";  // Unused icons
        // static constexpr const char* PAGE_DOWN = "\ue90d";
        // static constexpr const char* PAGE_UP = "\ue90e";
        static constexpr const char* PLUS = "\ue90f";
        static constexpr const char* POWER = "\ue910";
        static constexpr const char* RESTART = "\ue911";

        // Messaging icons
        static constexpr const char* INFO = "\ue912";
        static constexpr const char* SUCCESS = "\ue913";
        static constexpr const char* WARNING = "\ue914";
        static constexpr const char* ERROR = "\ue915";

        // Informational icons
        // static constexpr const char* ADDRESS = "\ue916";
        static constexpr const char* CHANGE = "\ue917";
        static constexpr const char* DERIVATION = "\ue918";
        // static constexpr const char* FEE = "\ue919";
        static constexpr const char* FINGERPRINT = "\ue91a";
        static constexpr const char* PASSPHRASE = "\ue91b";

        // Misc icons
        // static constexpr const char* BITCOIN = "\ue91c";
        static constexpr const char* BITCOIN_ALT = "\ue91d";
        // static constexpr const char* BRIGHTNESS = "\ue91e";
        static constexpr const char* MICROSD = "\ue91f";
        static constexpr const char* QRCODE = "\ue920";
        static constexpr const char* SIGN = "\ue921";

        // Input icons
        static constexpr const char* DELETE = "\ue922";
        static constexpr const char* SPACE = "\ue923";
};

// FontAwesome (Solid) icons that appear on SeedSigner buttons. These glyphs are NOT in
// seedsigner-icons.otf \u2014 they are extracted from Font_Awesome_6_Free-Solid-900.otf and
// MERGED into the seedsigner icon font at bake time (scripts/bake_icon_fonts.py), so
// they render through ICON_FONT__SEEDSIGNER exactly like the PUA icons above. Only the
// glyphs listed here are baked in; when a new FontAwesome button icon is introduced,
// add its codepoint to the bake script's FontAwesome range too.
class FontAwesomeIconConstants {
    public:
        static constexpr const char* CAMERA     = "\uf030";
        static constexpr const char* KEYBOARD   = "\uf11c";
        static constexpr const char* DICE       = "\uf522";
        static constexpr const char* DICE_ONE   = "\uf525";
        static constexpr const char* DICE_TWO   = "\uf528";
        static constexpr const char* DICE_THREE = "\uf527";
        static constexpr const char* DICE_FOUR  = "\uf524";
        static constexpr const char* DICE_FIVE  = "\uf523";
        static constexpr const char* DICE_SIX   = "\uf526";
};


#endif // GUI_CONSTANTS_H
