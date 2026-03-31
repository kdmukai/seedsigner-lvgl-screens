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
 * At 320px height we use PX_MULTIPLIER=150, which scales up slightly more than
 * the direct 320/240 ratio (~1.33) would suggest. This is an opinionated choice
 * for aesthetic reasons -- elements feel better slightly larger on the higher-res
 * screen rather than just linearly scaled.
 *
 * It remains to be seen whether displays beyond 320px height will need their own
 * PX_MULTIPLIER values or whether 150 will hold as screen sizes scale up.
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

#if !defined(SUPPORT_DISPLAY_HEIGHT_240) && !defined(SUPPORT_DISPLAY_HEIGHT_320)
#error "At least one SUPPORT_DISPLAY_HEIGHT_* flag must be defined"
#endif

// Named PX_MULTIPLIER values — one per supported display height.
const int PX_MULTIPLIER_100 = 100;   // 240px height (Pi Zero): no scaling
const int PX_MULTIPLIER_150 = 150;   // 320px height: aesthetic upscale

// ---------------------------------------------------------------------------
// Font declarations for supported display heights
// ---------------------------------------------------------------------------
#ifdef SUPPORT_DISPLAY_HEIGHT_240
LV_FONT_DECLARE(opensans_semibold_20_4bpp);
LV_FONT_DECLARE(opensans_semibold_18_4bpp);
LV_FONT_DECLARE(opensans_regular_17_4bpp);
LV_FONT_DECLARE(seedsigner_icons_24_4bpp);
LV_FONT_DECLARE(seedsigner_icons_36_4bpp);
#endif

#ifdef SUPPORT_DISPLAY_HEIGHT_320
LV_FONT_DECLARE(opensans_semibold_20_4bpp_150x);
LV_FONT_DECLARE(opensans_semibold_18_4bpp_150x);
LV_FONT_DECLARE(opensans_regular_17_4bpp_150x);
LV_FONT_DECLARE(seedsigner_icons_24_4bpp_150x);
LV_FONT_DECLARE(seedsigner_icons_36_4bpp_150x);
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
    int main_menu_button_height;

    const lv_font_t* top_nav_title_font;
    const lv_font_t* button_font;
    const lv_font_t* body_font;
    const lv_font_t* icon_font;
    const lv_font_t* icon_large_button_font;
};

const DisplayProfile& active_profile();
void set_display(int width, int height);
int display_profile_count();
const DisplayProfile& display_profile_at(int index);

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
#define MAIN_MENU_BUTTON_HEIGHT   (active_profile().main_menu_button_height)

#define TOP_NAV_TITLE_FONT                 (*active_profile().top_nav_title_font)
#define BUTTON_FONT                        (*active_profile().button_font)
#define BODY_FONT                          (*active_profile().body_font)
#define ICON_FONT__SEEDSIGNER              (*active_profile().icon_font)
#define ICON_LARGE_BUTTON_FONT__SEEDSIGNER (*active_profile().icon_large_button_font)

// ---------------------------------------------------------------------------
// Non-scaling constants (colors, font names, etc.)
// ---------------------------------------------------------------------------
const int BACKGROUND_COLOR = 0x000000;
const int WARNING_COLOR = 0xffd60a;
const int DIRE_WARNING_COLOR = 0xff0000;
const int SUCCESS_COLOR = 0x00dd00;
const int BITCOIN_ORANGE = 0xff9416;
const int ACCENT_COLOR = 0xff9f0a;
const int TESTNET_COLOR = 0x00f100;
const int REGTEST_COLOR = 0x00caf1;

static constexpr const char* ICON_FONT_NAME__FONT_AWESOME = "Font_Awesome_6_Free-Solid-900";

const int BODY_FONT_COLOR = 0xf8f8f8;

static constexpr const char* FIXED_WIDTH_FONT_NAME = "Inconsolata-Regular";
static constexpr const char* FIXED_WIDTH_EMPHASIS_FONT_NAME = "Inconsolata-SemiBold";

const int LABEL_FONT_COLOR = 0x707070;

const int BUTTON_FONT_COLOR = 0xe8e8e8;
const int BUTTON_BACKGROUND_COLOR = 0x2c2c2c;
const int BUTTON_SELECTED_FONT_COLOR = 0x000000;

const int NOTIFICATION_COLOR = 0x00f100;


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

        // Must be updated whenever new icons are added. See usage in `Icon` class below.
        static constexpr const char* MIN_VALUE = SCAN;
        static constexpr const char* MAX_VALUE = SPACE;
};


#endif // GUI_CONSTANTS_H
