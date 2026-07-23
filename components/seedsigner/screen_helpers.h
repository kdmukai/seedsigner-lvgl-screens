#pragma once

// ── Cross-cutting screen render/layout helpers ──
//
// These small helpers (text measurement, tight line spacing, body-text labels,
// button-list config readers, status-type defaults, hex-color parsing, RTL
// post-pass) are used by many screens across domains. They are declared here so
// each per-screen translation unit under screens/ can call them; the definitions
// live in screen_helpers.cpp.
//
// This header is the companion to screen_scaffold.h: scaffold.h owns the screen
// skeleton (top-nav + body + nav binding); this owns the shared render helpers.

#include <cstdint>
#include <string>
#include <vector>

#include "lvgl.h"
#include "components.h"      // SEEDSIGNER_ICON_COLOR_DEFAULT (button_item_cfg_t default)
#include "input_profile.h"   // input_mode_t (nav_mode_override_from_cfg)

#include <nlohmann/json.hpp>

// One row of a button-list screen, parsed from cfg by read_button_list_items().
struct button_item_cfg_t {
    std::string label;
    std::string icon;        // empty = none
    std::string right_icon;  // empty = none
    uint32_t    icon_color = SEEDSIGNER_ICON_COLOR_DEFAULT;
    uint32_t    label_color = SEEDSIGNER_ICON_COLOR_DEFAULT;
    bool        is_checked = false;  // set from cfg["checked_buttons"] by the scaffold
};

// Status-screen family (large_icon_status_screen + the psbt detail screens): a
// status_type selects an icon, color, and default titles/labels.
enum class status_type_t { SUCCESS, WARNING, DIRE_WARNING, ERROR, CUSTOM };

struct status_type_defaults_t {
    const char *icon;
    int color;
    const char *default_top_nav_title;
    const char *default_button_label;
    bool warning_edges_default;
    int text_edge_padding_multiplier;  // 1 for success, 2 for warning variants
};

// RTL post-pass: recursively set base_dir on a label subtree (sibling to the
// glyph-run RTL path). Idempotent.
void apply_rtl_text_to_labels(lv_obj_t *obj);

// Build the standard scrollable/static body container under a top-nav.
lv_obj_t *create_standard_body_content(lv_obj_t *screen, lv_obj_t *top_nav_obj, bool scrollable);

// Button-list config readers: labels-only, and the richer per-item form.
bool read_button_list_labels(const nlohmann::json &cfg, std::vector<std::string> &out);
bool read_button_list_items(const nlohmann::json &cfg, std::vector<button_item_cfg_t> &out);

// "#rrggbb" (or 0xAARRGGBB-ish) -> packed color.
uint32_t parse_hex_color(const std::string &s);

// Standard body-text label (wrapped, themed) parented under `parent`.
lv_obj_t *make_body_text_label(lv_obj_t *parent, const char *text, int32_t width);

// Status-type table lookup + JSON parse + apply-defaults onto a screen cfg.
status_type_defaults_t defaults_for_status_type(status_type_t st);
status_type_t parse_status_type(const nlohmann::json &cfg);
void apply_status_type_defaults(nlohmann::json &cfg, const status_type_defaults_t &defaults);

// --- top_nav chrome normalization (structural defaults + content validation) ---
// Content policy (spec: docs/screen-conformance-spec.md): every user-visible
// string arrives LOCALIZED from the host view layer via cfg. Screens never
// inject English content defaults — a missing required content key is a
// developer error and throws. Structural defaults (booleans / layout flags,
// never rendered as text) remain write-if-absent.

// Ensure cfg["top_nav"] is an object (an absent OR non-object value is replaced
// with an empty object, exactly like the inline blocks this replaces), then
// write-if-absent the two structural chrome flags. A host-provided value always
// wins. Forced (non-defaulted) overrides — e.g. a screen that unconditionally
// hides the back button — stay explicit assignments at the call site AFTER this
// call, annotated with their Python constant, so each screen's guarded-vs-forced
// host-override contract remains visible.
void ensure_top_nav_structure(nlohmann::json &cfg,
                              bool default_show_back_button, bool default_show_power_button);

// Validate that the host supplied the (localized) top-nav title: throws a
// screen-name-prefixed std::runtime_error when cfg["top_nav"]["title"] is
// missing or not a string. Call after ensure_top_nav_structure().
void require_top_nav_title(const nlohmann::json &cfg, const char *screen_name);

// Per-glyph ink extents over a UTF-8 string (max ascent/descent above/below the
// baseline). out_max_descent may be null.
void measure_text_ink_extents(const lv_font_t *font, const char *text,
                              int32_t *out_max_ascent, int32_t *out_max_descent);

// Tight inter-line advance for a body string (ink-hugging, < font line_height).
int32_t tight_line_space(const lv_font_t *font, const char *text, int32_t gap);

// Apply the tight body line spacing to an existing (possibly multi-line) label.
void apply_body_tight_line_spacing(lv_obj_t *label);

// ── Nav-policy cfg readers (shared by scaffold + most screens) ──
// Read an optional input-mode override ("touch"/"hardware") from cfg.
void nav_mode_override_from_cfg(const nlohmann::json &cfg, bool &has_override, input_mode_t &mode_override);
// Read the initial focused-button index from cfg (else default_index).
size_t nav_initial_index_from_cfg(const nlohmann::json &cfg, size_t default_index);

// Even out a wrapped label's lines (balanced ragged column).
void balance_wrapped_label_column(lv_obj_t *label);

// ── PSBT / amount detail-screen shared helpers ──
// Shared by the psbt_overview / psbt_address_details / psbt_change_details screen
// files (definitions in screen_helpers.cpp).

// Map a network code ("M"/"T"/"R", the standardized host contract; long names also
// tolerated for legacy scenarios) to its accent color. Per D-6 the host supplies the
// network; screens never infer it from an address.
uint32_t network_color(const std::string &net);

// Build the send-amount headline (coin icon + amount) from cfg["btc_amount"].
lv_obj_t *btc_amount_from_cfg(lv_obj_t *parent, const nlohmann::json &j);
