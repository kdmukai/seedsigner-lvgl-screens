// toast_overlay_screen
//
// Python provenance: no direct screen class — desktop-tooling host for the
// toast_overlay module, whose banner ports gui/toast.py ToastOverlay.render().
//
// On device the toast (toast_overlay.{h,cpp}) composites over the LIVE screen from
// the display's top layer, raised by the host through overlay_manager_show_toast()
// (or toast_overlay_show()). This wrapper lets the screenshot generator and runners
// exercise the SAME banner WITHOUT a live app: it builds the real MAIN MENU as the
// underlying screen — matching Python's MainMenuView_*Toast screenshots, which render
// each toast over the home menu — then raises the toast on the top layer above it. So
// the shot shows the toast COMPOSITING over a real screen, not over black (the very
// Pi-Zero bug this module fixes).
//
// It stays a SEPARATE entry in the screen picker (its own registry key + scenario);
// the main menu is only borrowed as a backdrop. Folding these into main_menu_screen's
// own variations would bury them among the menu's scenarios.
//
// The banner itself (rounded outline, icon, wrapped message, colors) belongs entirely
// to the toast_overlay module. This file owns only the severity -> (icon, color)
// convenience mapping that mirrors Python's Toast subclasses (InfoToast / SuccessToast
// / WarningToast / DireWarningToast / ErrorToast, and the green MICROSD notification
// toasts). A real host applies the same policy in its view layer and passes the
// resolved icon + colors straight to the toast API.
//
// cfg:
//   label_text (string, REQUIRED)  the toast message, already localized by the host.
//            May contain '\n'. This is user-visible CONTENT, so — per the content
//            policy — it is never defaulted here; a missing value throws.
//   severity  (string, default "default")  selects the icon + colors:
//            default | info | success | warning | dire_warning | error | sd_card.
//   icon      (string, optional)  explicit seedsigner-icon glyph override; when
//            absent the severity's icon is used ("" forces no icon).
//   duration_ms (int, default 3000)  auto-dismiss delay for the interactive runners;
//            ignored under static render (the screenshot keeps the banner up).
//   background (object, REQUIRED)  the main_menu_screen config used as the backdrop
//            (top_nav.title + the four button_list labels). Passed through verbatim to
//            main_menu_screen, so its own content rules (localized title + exactly four
//            labels) apply; its labels localize via the recursive scenario localizer.

#include "screen_scaffold.h"   // parse_optional_screen_json_ctx
#include "seedsigner.h"        // toast_overlay_screen + main_menu_screen entry-point declarations
#include "toast_overlay.h"     // toast_overlay_spec_t, toast_overlay_show / _dismiss
#include "gui_constants.h"     // colors, SeedSignerIconConstants

#include "lvgl.h"              // lv_screen_active, lv_obj_add_event_cb

#include <nlohmann/json.hpp>   // json (cfg read)

#include <stdexcept>           // std::runtime_error (required-field validation)
#include <string>              // std::string

using json = nlohmann::json;


namespace {

// Severity -> (icon glyph, outline color, text color). Mirrors Python's Toast
// subclasses: the messaging toasts use a colored outline + icon over WHITE text; the
// SD-card notification uses green for BOTH outline and text (Python leaves color ==
// font_color == NOTIFICATION_COLOR). A NULL icon means a text-only toast (DefaultToast).
struct toast_severity_t {
    const char *icon;
    uint32_t    outline_color;
    uint32_t    font_color;
};

toast_severity_t severity_for(const std::string &name) {
    if (name == "info")
        return {SeedSignerIconConstants::INFO,    (uint32_t)INFO_COLOR,         (uint32_t)BODY_FONT_COLOR};
    if (name == "success")
        return {SeedSignerIconConstants::SUCCESS, (uint32_t)SUCCESS_COLOR,      (uint32_t)BODY_FONT_COLOR};
    if (name == "warning")
        return {SeedSignerIconConstants::WARNING, (uint32_t)WARNING_COLOR,      (uint32_t)BODY_FONT_COLOR};
    if (name == "dire_warning")
        return {SeedSignerIconConstants::WARNING, (uint32_t)DIRE_WARNING_COLOR, (uint32_t)BODY_FONT_COLOR};
    if (name == "error")
        return {SeedSignerIconConstants::ERROR,   (uint32_t)ERROR_COLOR,        (uint32_t)BODY_FONT_COLOR};
    if (name == "sd_card")
        return {SeedSignerIconConstants::MICROSD, (uint32_t)NOTIFICATION_COLOR, (uint32_t)NOTIFICATION_COLOR};
    // "default": text-only, white outline + white text (Python DefaultToast).
    return {nullptr, (uint32_t)BODY_FONT_COLOR, (uint32_t)BODY_FONT_COLOR};
}

// The backdrop main menu is torn down (on the next screen load) while its toast still
// floats on the shared top layer — dismiss it so it can't linger over the successor
// screen in the interactive runners.
void dismiss_toast_on_teardown_cb(lv_event_t * /*e*/) {
    toast_overlay_dismiss();
}

}  // namespace


void toast_overlay_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_optional_screen_json_ctx(json_str, cfg);

    // The message is user-visible CONTENT — always host-supplied, never defaulted.
    if (!cfg.contains("label_text") || !cfg["label_text"].is_string()) {
        throw std::runtime_error("toast_overlay_screen: label_text is required and must be a string");
    }
    std::string label_text = cfg["label_text"].get<std::string>();

    std::string severity = cfg.value("severity", std::string("default"));
    toast_severity_t sev = severity_for(severity);

    // Optional explicit icon override ("" forces a text-only toast).
    std::string icon_override;
    bool have_icon_override = false;
    if (cfg.contains("icon") && cfg["icon"].is_string()) {
        icon_override = cfg["icon"].get<std::string>();
        have_icon_override = true;
    }
    const char *icon_glyph = have_icon_override
                                 ? (icon_override.empty() ? nullptr : icon_override.c_str())
                                 : sev.icon;

    uint32_t duration_ms = (uint32_t)cfg.value("duration_ms", 3000);

    // The backdrop is the real main menu (Python renders each toast over MainMenuView).
    if (!cfg.contains("background") || !cfg["background"].is_object()) {
        throw std::runtime_error(
            "toast_overlay_screen: background is required and must be the main_menu_screen config object");
    }
    std::string background_json = cfg["background"].dump();

    // --- Build the main menu backdrop, then raise the toast on the top layer ---

    // main_menu_screen builds + loads the menu as the active screen (and cleans up the
    // previous scenario — whose own teardown handler dismisses any lingering toast). It
    // enforces its own content rules (localized title + exactly four labels).
    main_menu_screen((void *)background_json.c_str());

    // Dismiss the toast when this backdrop is torn down (runner navigation): the toast
    // lives on the shared top layer, not on the menu screen, so it needs an explicit
    // hook off the now-active menu.
    lv_obj_t *backdrop = lv_screen_active();
    if (backdrop) {
        lv_obj_add_event_cb(backdrop, dismiss_toast_on_teardown_cb, LV_EVENT_DELETE, NULL);
    }

    toast_overlay_spec_t spec;
    spec.label_text    = label_text.c_str();
    spec.icon_glyph    = icon_glyph;
    spec.outline_color = sev.outline_color;
    spec.font_color    = sev.font_color;
    spec.duration_ms   = duration_ms;
    toast_overlay_show(&spec);
}
