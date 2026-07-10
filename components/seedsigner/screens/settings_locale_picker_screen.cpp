// settings_locale_picker_screen
//
// Python provenance: no Python screen class — an LVGL-native screen driven by
// LocaleSelectionView (settings_views.py) through View.run_locale_picker_screen
// (view.py). It supersedes the PIL path's generic
// SettingsEntryUpdateSelectionScreen entry for SETTING__LOCALE, whose per-button
// script fonts the LVGL runner cannot forward.
//
// The language-selection screen: one radio-style (CHECKED_SELECTION) row per
// onboard language, each showing the English name and the native name (endonym)
// on a single line; the active locale's row is check-marked and initially
// focused. Unlike a generic button_list_screen it must show every language's
// name in its OWN native script on one screen — which the native path cannot do
// as live text (one active locale, one set of role fonts, and keeping N script
// fonts resident would blow the ESP32 glyph-cache pool). So each row is EITHER:
//   - live text (the always-resident baseline font) — for Latin-script endonyms
//     covered by the baked floor (English, Español, …); OR
//   - a pre-rendered A8 endonym image (native script, zero runtime font) — for
//     every language that ships a font pack (CJK, Arabic/Persian, Devanagari, …).
// A row is an image row iff its cfg entry carries "image"; the host fetches
// that blob through the endonym image provider (locale_picker_set_image_provider,
// the same seam as ss_load_locale). Otherwise it is a live-text row.
//
// Row format: the English name, a "|" separator, then the native name — live
// text for a Latin-script native (e.g. "Spanish | Español"), or the
// pre-rendered image drawn right after the separator for a non-Latin script
// (e.g. "Hindi | <हिन्दी image>"); the button label keeps its live English text
// either way. English itself (native == english) shows once, with no separator.
// A row that overflows scrolls like any button-list row. The host orders the
// rows (English pinned first, the rest alphabetical by English name).
//
// Selection uses the standard body-button result path: clicking / entering a
// row fires seedsigner_lvgl_on_button_selected(row_index, ...); the host maps
// the index back to the locale it placed at that position (and persists /
// re-renders).
//
// SANCTIONED FAIL-SOFT (content policy, spec §5): the per-row endonym image is
// deliberately NOT required to render — locale_picker_attach_endonym returns
// false on a missing provider / failed fetch / malformed blob, leaving the row
// showing its live English text. A bad or half-copied SD pack must degrade one
// row's rendering, never crash the language picker — this is the screen a user
// needs working to recover from a bad pack.
//
// Lifecycle: Tier 1 (stateless) — no statics, timers, or heap ctx; each row's
// endonym image data is owned by locale_picker_attach_endonym's own delete
// callback on the row button (see locale_picker.h).
//
// Layout note: cfg carries NO "button_list" key by contract — its absence
// selects scaffold Mode 1 (top_nav + plain scrollable body, upper_body ==
// body), and this file builds the row buttons itself, chained with the same
// geometry button_list() produces, so each row can host an endonym image
// overlay. Any change to button_list() spacing must be mirrored here by hand.
//
// cfg:
//   rows                      (array, required)      picker rows in display order;
//            at most SEEDSIGNER_SCAFFOLD_MAX_BUTTONS entries. Per row:
//   rows[].locale             (string, required)     locale code ("en",
//            "zh_Hans_CN", …); keys the endonym-image fetch and the
//            active_locale match.
//   rows[].english            (string, default "")   the language's English name;
//            when empty, the native name alone becomes the row text.
//   rows[].native             (string, default "")   the endonym (native-script
//            name); appended as live text when it differs from the primary name
//            and the row has no image.
//   rows[].image              (string | bool, optional)  endonym image row: a
//            string names the pack file explicitly ("endonym_480.bin"); boolean
//            true derives "endonym_<active profile height>.bin" so one scenario
//            renders at every resolution. Absent/false => live-text row.
//   active_locale             (string, default "")   currently-persisted locale
//            code: its row is check-marked and initially focused.
//   top_nav.title             (string, required)     localized screen title (read
//            by the scaffold; enforced here via require_top_nav_title).
//   top_nav.show_back_button  (bool, default true)   scaffold chrome flag (the
//            same value the scaffold falls back to when absent).
//   top_nav.show_power_button (bool, default false)  scaffold chrome flag (the
//            same value the scaffold falls back to when absent).
//   top_nav.icon              (string, optional)     icon glyph beside the title
//            (read by the scaffold).
//   top_nav.icon_color        (hex string, optional) title icon color (scaffold).
//   initial_selected_index    (int, optional)        overrides the default initial
//            focus of the active locale's row (navigation layer).
//   input.mode                (string, optional)     "touch" | "hardware"
//            input-mode override (navigation layer).
//   input.keys.key1/key2/key3 (string, optional)     per-aux-key policy "enter" |
//            "noop" | "emit" (navigation layer).
//   allow_screensaver         (bool, default true)   per-screen screensaver policy
//            (normalized by parse_screen_json_ctx, stamped by the scaffold).

#include "screen_scaffold.h"  // parse_screen_json_ctx / create_top_nav_screen_scaffold / bind_screen_navigation / load_screen_and_cleanup_previous
#include "seedsigner.h"       // settings_locale_picker_screen decl, screen_scaffold_t, SEEDSIGNER_SCAFFOLD_MAX_BUTTONS
#include "components.h"       // button_ex, button_opts_t, BUTTON_STYLE_CHECKED_SELECTION, SEEDSIGNER_ICON_COLOR_DEFAULT
#include "gui_constants.h"    // active_profile (endonym image filename derivation)
#include "locale_picker.h"    // locale_picker_attach_endonym (endonym image rows)
#include "navigation.h"       // NAV_BODY_VERTICAL
#include "screen_helpers.h"   // ensure_top_nav_structure, require_top_nav_title

#include "lvgl.h"             // lv_obj_t (row-button handles)

#include <nlohmann/json.hpp>  // json (cfg reads + structural-default writes)

#include <stdexcept>          // std::runtime_error (required-field validation)
#include <string>             // std::string, std::to_string
#include <vector>             // std::vector (row-button handles for navigation)

using json = nlohmann::json;


void settings_locale_picker_screen(void *ctx_json) {
    // --- Config ---

    const char *json_str = (const char *)ctx_json;

    json cfg;
    parse_screen_json_ctx(json_str, cfg);

    // Required field: rows is the screen's whole reason to exist — one radio row
    // per entry. One throw per defect, before the scaffold exists, so no throw
    // path can leak LVGL objects.
    if (!cfg.contains("rows") || !cfg["rows"].is_array()) {
        throw std::runtime_error("settings_locale_picker_screen: \"rows\" is required and must be an array");
    }
    const json &rows = cfg["rows"];

    // SEEDSIGNER_SCAFFOLD_MAX_BUTTONS is owned by the scaffold layer today (it
    // sizes screen_scaffold_t.button_list and create_top_nav_screen_scaffold
    // enforces it for cfg["button_list"]); these rows bypass that fixed array
    // (they ride a local vector into bind_screen_navigation, which heap-sizes to
    // count), so this screen re-applies the scaffold's list bound on itself.
    if (rows.size() > SEEDSIGNER_SCAFFOLD_MAX_BUTTONS) {
        throw std::runtime_error("settings_locale_picker_screen: rows exceed SEEDSIGNER_SCAFFOLD_MAX_BUTTONS");
    }

    // Per-row required field: locale keys the endonym-image fetch and the
    // active-row match. Every row is validated up front (still before the
    // scaffold) so the build loop below cannot throw mid-tree.
    for (const json &row : rows) {
        if (!row.is_object() || !row.contains("locale") || !row["locale"].is_string()) {
            throw std::runtime_error("settings_locale_picker_screen: each row needs a string \"locale\"");
        }
    }

    const std::string active_locale = cfg.value("active_locale", std::string());

    // Structural defaults (write-if-absent, never user-visible text): the same
    // values the scaffold falls back to when the flags are absent, so these
    // writes are representation-only. The localized title itself is content and
    // must come from the host.
    ensure_top_nav_structure(cfg, /*default_show_back_button=*/true,
                                  /*default_show_power_button=*/false);
    require_top_nav_title(cfg, "settings_locale_picker_screen");

    // --- Scaffold ---

    // No "button_list" key ⇒ scaffold Mode 1: top_nav + a plain scrollable body
    // (upper_body == body). We build the row buttons into that body ourselves so
    // each can host live text OR an endonym image. A pure list, so navigation uses
    // item-focus scrolling (no scroll-then-buttons hand-off).
    screen_scaffold_t screen = create_top_nav_screen_scaffold(cfg, /*scrollable=*/true);

    // --- Body ---

    // 1. One radio row per locale, chained top-down into the scrollable body.
    std::vector<lv_obj_t *> items;
    items.reserve(rows.size());
    lv_obj_t *previous_row_button = nullptr;
    size_t active_index = 0;

    for (size_t i = 0; i < rows.size(); ++i) {
        const json &row = rows[i];
        const std::string locale  = row["locale"].get<std::string>();
        const std::string english = row.value("english", std::string());
        const std::string native  = row.value("native",  std::string());
        const bool is_active = (!active_locale.empty() && locale == active_locale);

        // Non-Latin natives ship a pre-rendered image; name it explicitly
        // ("image":"endonym_480.bin") or let the screen derive it for the active
        // profile height ("image":true → endonym_<height>.bin, so one scenario
        // renders at every resolution).
        std::string image_file;
        if (row.contains("image")) {
            if (row["image"].is_string()) {
                image_file = row["image"].get<std::string>();
            } else if (row["image"].is_boolean() && row["image"].get<bool>()) {
                image_file = "endonym_" + std::to_string(active_profile().height) + ".bin";
            }
        }

        // Row text = English name, a "|" separator, then the native name on the same
        // line. A non-Latin native is drawn as its image just after the separator, so
        // its label ends at "English |". A Latin native is appended as live text.
        // English itself (native == English) shows once, with no separator.
        const std::string primary = !english.empty() ? english : native;
        std::string label_text = primary;
        if (!image_file.empty()) {
            label_text = primary + " |";                 // native image drawn after the pipe
        } else if (!native.empty() && native != primary) {
            label_text = primary + " | " + native;       // Latin native as live text
        }

        // Radio row: the current locale is CHECK-marked; every row left-aligned and
        // chained below the previous (the same geometry button_list() produces).
        button_opts_t row_button_opts = {};
        row_button_opts.text             = label_text.c_str();
        row_button_opts.align_to         = previous_row_button;
        row_button_opts.is_text_centered = false;
        row_button_opts.icon_color       = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BUTTON_FONT_COLOR at rest
        row_button_opts.label_color      = SEEDSIGNER_ICON_COLOR_DEFAULT;   // -> BUTTON_FONT_COLOR at rest
        row_button_opts.style            = BUTTON_STYLE_CHECKED_SELECTION;
        row_button_opts.is_checked       = is_active;
        lv_obj_t *row_button = button_ex(screen.body, &row_button_opts);

        // Image row (native-script name, no runtime font). The label keeps its live
        // English text; on any failure the attach returns false and the row simply
        // shows that text — the sanctioned fail-soft stated in the banner, never a
        // crash on a bad pack image.
        if (!image_file.empty()) {
            locale_picker_attach_endonym(row_button, locale.c_str(), image_file.c_str());
        }

        items.push_back(row_button);
        previous_row_button = row_button;
        if (is_active) active_index = i;
    }

    // --- Navigation + load ---

    // Active-row default index: focus (and scroll to) the current locale's row by
    // default; an explicit cfg initial_selected_index still overrides via
    // nav_initial_index_from_cfg.
    bind_screen_navigation(
        cfg, screen,
        items.empty() ? nullptr : items.data(), items.size(),
        NAV_BODY_VERTICAL, active_index);

    load_screen_and_cleanup_previous(screen.screen);
}
