#ifndef SCREEN_SCAFFOLD_H
#define SCREEN_SCAFFOLD_H

// Internal (C++) declarations for the shared screen-building helpers that are
// DEFINED in screen_scaffold.cpp. They are exposed here so that per-screen source
// files (the "one screen per .cpp" pattern — e.g. multisig_wallet_descriptor_screen.cpp)
// can reuse the standard scaffold instead of duplicating it:
//   - JSON context parse/validate,
//   - the TopNav + body (+ optional bottom-pinned button list) tree,
//   - joystick/touch navigation wiring,
//   - the RTL / glyph-run / previous-screen-cleanup load path.
//
// These take nlohmann::json and C++ references, so they cannot live in the C-linkage
// public API header (seedsigner.h). This header is C++-only and intended for the
// screen implementation files, not external consumers.

#include "seedsigner.h"       // screen_scaffold_t (and the public screen entry points)
#include "navigation.h"       // nav_body_layout_t

#include <nlohmann/json.hpp>

#include <cstddef>

using json = nlohmann::json;

// Parse + validate a screen's JSON context string (throws std::runtime_error on
// malformed input) and normalize the allow_screensaver default.
void parse_screen_json_ctx(const char *ctx_json, json &cfg_out);

// Optional-context variant of the parse above, for the boot/overlay tier whose
// context is legitimately optional (main_menu, opening_splash, loading_spinner,
// the two camera overlay screens). NULL or empty ctx_json yields an empty config
// object carrying the same allow_screensaver normalization parse_screen_json_ctx
// applies to every config; any other input is delegated to parse_screen_json_ctx
// unchanged (identical validation, identical error strings).
void parse_optional_screen_json_ctx(const char *ctx_json, json &cfg_out);

// Build the root screen: a TopNav plus the standard body container. When
// cfg["button_list"] is present the body becomes a flex column of
// [upper_body, (optional flex-grow spacer when is_bottom_list), button(s)].
// Screens populate scaffold.upper_body and finish with load_screen_and_cleanup_previous().
screen_scaffold_t create_top_nav_screen_scaffold(const json &cfg, bool scrollable,
                                                 const lv_font_t *title_font = nullptr);

// Wire joystick/touch navigation for a built scaffold (top-nav back/power, aux-key
// policy, input-mode override, scroll-then-buttons auto-detect).
void bind_screen_navigation(const json &cfg, const screen_scaffold_t &screen,
                            lv_obj_t **body_items, size_t body_item_count,
                            nav_body_layout_t body_layout, size_t default_initial_index);

// Scaffold-buttons convenience: vertical body list discovered by the scaffold.
// Forwards to the full overload above with the scaffold's own button_list /
// button_list_count and NAV_BODY_VERTICAL — scaffold-built button screens are
// always vertical lists, so this is the canonical form for them.
//
// default_initial_index is deliberately REQUIRED (no default value): it is the one
// real per-screen policy — 0 means "a menu/list always has a selection";
// NAV_INDEX_NONE means "read-first status screen: nothing is focused while
// scrolling is required to reach the buttons" (see nav_config_t.initial_body_index).
//
// Screens with custom item arrays, grid layouts (main_menu), or a contract of
// ignoring any scaffold buttons (donate / reset / power_off_not_required pass a
// literal NULL, 0) stay on the full overload.
void bind_screen_navigation(const json &cfg, const screen_scaffold_t &screen,
                            size_t default_initial_index);

// Add the pulsing WarningEdges border (color = status_color) on top of `screen`
// (WarningEdgesMixin parity). Reused by warning-class screens (e.g.
// seed_transcribe_whole_qr_screen.cpp) in addition to the status screens.
void add_warning_edges_overlay(lv_obj_t *screen, int status_color);

// Post-layout scaffold-geometry query: the laid-out TOP edge (absolute y1) of the
// scaffold's bottom button — i.e. the bottom of the free band a screen centers
// body content into. Returns button_list[0]'s y1 when the scaffold has a valid
// button, else the display-derived fallback every call site used
// (display height - BUTTON_HEIGHT).
// PRECONDITION: the caller has already run lv_obj_update_layout() on the screen,
// so button_list[0]'s coordinates are final.
int32_t bottom_button_top_y(const screen_scaffold_t &screen);

// Switch to a finished screen: applies the RTL + complex-script glyph-run passes,
// loads it as the active screen, deletes the previous root, and reaps retired fonts.
void load_screen_and_cleanup_previous(lv_obj_t *new_screen);

// Register `screen` so its teardown counts as user activity: on the screen's LV_EVENT_DELETE
// (fired when the successor's load_screen_and_cleanup_previous swaps it out) it resets LVGL's
// idle clock (lv_display_trigger_activity). Use it on screens that can stay up a long time with
// NO user input — a camera scan/entropy screen while the user lines up a QR — so the stale idle
// clock doesn't make the overlay dispatcher fire the screensaver over the freshly-loaded
// successor instead of showing it. The loading spinner applies the same fix inline (its
// LV_EVENT_DELETE cleanup also frees its timer/ctx there); see PR #69 for the rationale.
void reset_idle_clock_on_teardown(lv_obj_t *screen);

#endif // SCREEN_SCAFFOLD_H
