#ifndef SCREEN_SCAFFOLD_H
#define SCREEN_SCAFFOLD_H

// Internal (C++) declarations for the shared screen-building helpers that are
// DEFINED in seedsigner.cpp. They are exposed here so that per-screen source
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

// Add the pulsing WarningEdges border (color = status_color) on top of `screen`
// (WarningEdgesMixin parity). Reused by warning-class screens split out of
// seedsigner.cpp (e.g. seed_transcribe_whole_qr_screen.cpp) in addition to the
// status screens that define it.
void add_warning_edges_overlay(lv_obj_t *screen, int status_color);

// Switch to a finished screen: applies the RTL + complex-script glyph-run passes,
// loads it as the active screen, deletes the previous root, and reaps retired fonts.
void load_screen_and_cleanup_previous(lv_obj_t *new_screen);

#endif // SCREEN_SCAFFOLD_H
