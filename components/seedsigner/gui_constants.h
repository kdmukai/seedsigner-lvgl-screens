#ifndef GUI_CONSTANTS_H
#define GUI_CONSTANTS_H

#ifndef PX_MULTIPLIER
#define PX_MULTIPLIER 125
#endif

#include <string>
#include "lvgl.h"

const int EDGE_PADDING = static_cast<int>(8 * PX_MULTIPLIER / 100.0);
const int COMPONENT_PADDING = static_cast<int>(8 * PX_MULTIPLIER / 100.0);
const int LIST_ITEM_PADDING = static_cast<int>(4 * PX_MULTIPLIER / 100.0);

const int BACKGROUND_COLOR = 0x000000;
const int WARNING_COLOR = 0xffd60a;
const int DIRE_WARNING_COLOR = 0xff0000;
const int SUCCESS_COLOR = 0x00dd00;
const int BITCOIN_ORANGE = 0xff9416;
const int ACCENT_COLOR = 0xff9f0a;
const int TESTNET_COLOR = 0x00f100;
const int REGTEST_COLOR = 0x00caf1;

static constexpr const char* ICON_FONT_NAME__FONT_AWESOME = "Font_Awesome_6_Free-Solid-900";
#if PX_MULTIPLIER == 150
    LV_FONT_DECLARE(opensans_semibold_20_4bpp_150x);
    LV_FONT_DECLARE(opensans_semibold_18_4bpp_150x);
    LV_FONT_DECLARE(opensans_regular_17_4bpp_150x);
    LV_FONT_DECLARE(seedsigner_icons_24_4bpp_150x);
    LV_FONT_DECLARE(seedsigner_icons_36_4bpp_150x);

    const lv_font_t TOP_NAV_TITLE_FONT = opensans_semibold_20_4bpp_150x;
    const lv_font_t BUTTON_FONT = opensans_semibold_18_4bpp_150x;
    const lv_font_t BODY_FONT = opensans_regular_17_4bpp_150x;
    const lv_font_t ICON_FONT__SEEDSIGNER = seedsigner_icons_24_4bpp_150x;
    const lv_font_t ICON_LARGE_BUTTON_FONT__SEEDSIGNER = seedsigner_icons_36_4bpp_150x;
#elif PX_MULTIPLIER == 125
    LV_FONT_DECLARE(opensans_semibold_20_4bpp_125x);
    LV_FONT_DECLARE(opensans_semibold_18_4bpp_125x);
    LV_FONT_DECLARE(opensans_regular_17_4bpp_125x);
    LV_FONT_DECLARE(seedsigner_icons_24_4bpp_125x);
    LV_FONT_DECLARE(seedsigner_icons_36_4bpp_125x);
    const lv_font_t TOP_NAV_TITLE_FONT = opensans_semibold_20_4bpp_125x;
    const lv_font_t BUTTON_FONT = opensans_semibold_18_4bpp_125x;
    const lv_font_t BODY_FONT = opensans_regular_17_4bpp_125x;
    const lv_font_t ICON_FONT__SEEDSIGNER = seedsigner_icons_24_4bpp_125x;
    const lv_font_t ICON_LARGE_BUTTON_FONT__SEEDSIGNER = seedsigner_icons_36_4bpp_125x;
#else
    LV_FONT_DECLARE(opensans_semibold_20_4bpp);
    LV_FONT_DECLARE(opensans_semibold_18_4bpp);
    LV_FONT_DECLARE(opensans_regular_17_4bpp);
    LV_FONT_DECLARE(seedsigner_icons_24_4bpp);
    LV_FONT_DECLARE(seedsigner_icons_36_4bpp);

    const lv_font_t TOP_NAV_TITLE_FONT = opensans_semibold_20_4bpp;
    const lv_font_t BUTTON_FONT = opensans_semibold_18_4bpp;
    const lv_font_t BODY_FONT = opensans_regular_17_4bpp;
    const lv_font_t ICON_FONT__SEEDSIGNER = seedsigner_icons_24_4bpp;
    const lv_font_t ICON_LARGE_BUTTON_FONT__SEEDSIGNER = seedsigner_icons_36_4bpp;
#endif
const int ICON_FONT_SIZE = static_cast<int>(22 * PX_MULTIPLIER / 100.0);
const int ICON_INLINE_FONT_SIZE = static_cast<int>(24 * PX_MULTIPLIER / 100.0);
const int ICON_LARGE_BUTTON_SIZE = static_cast<int>(36 * PX_MULTIPLIER / 100.0);
const int ICON_PRIMARY_SCREEN_SIZE = static_cast<int>(50 * PX_MULTIPLIER / 100.0);

const int TOP_NAV_TITLE_FONT_SIZE = static_cast<int>(20 * PX_MULTIPLIER / 100.0);
const int TOP_NAV_HEIGHT = static_cast<int>(48 * PX_MULTIPLIER / 100.0);
const int TOP_NAV_BUTTON_SIZE = static_cast<int>(32 * PX_MULTIPLIER / 100.0);

const int BODY_FONT_SIZE = static_cast<int>(17 * PX_MULTIPLIER / 100.0);
const int BODY_FONT_MAX_SIZE = TOP_NAV_TITLE_FONT_SIZE;
const int BODY_FONT_MIN_SIZE = static_cast<int>(15 * PX_MULTIPLIER / 100.0);
const int BODY_FONT_COLOR = 0xf8f8f8;
const int BODY_LINE_SPACING = COMPONENT_PADDING;

static constexpr const char* FIXED_WIDTH_FONT_NAME = "Inconsolata-Regular";
static constexpr const char* FIXED_WIDTH_EMPHASIS_FONT_NAME = "Inconsolata-SemiBold";

const int LABEL_FONT_SIZE = BODY_FONT_MIN_SIZE;
const int LABEL_FONT_COLOR = 0x707070;

const int BUTTON_FONT_SIZE = static_cast<int>(18 * PX_MULTIPLIER / 100.0);
const int BUTTON_FONT_COLOR = 0xe8e8e8;
const int BUTTON_BACKGROUND_COLOR = 0x2c2c2c;
const int BUTTON_HEIGHT = static_cast<int>(32 * PX_MULTIPLIER / 100.0);
const int BUTTON_SELECTED_FONT_COLOR = 0x000000;
const int BUTTON_RADIUS = static_cast<int>(8 * PX_MULTIPLIER / 100.0);
const int MAIN_MENU_BUTTON_HEIGHT = static_cast<int>(56 * PX_MULTIPLIER / 100.0);

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