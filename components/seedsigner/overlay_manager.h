#ifndef SEEDSIGNER_OVERLAY_MANAGER_H
#define SEEDSIGNER_OVERLAY_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "toast_overlay.h"   // toast_overlay_spec_t

#ifdef __cplusplus
extern "C" {
#endif

// Overlay manager — a shared, platform-agnostic native layer that owns the
// cross-cutting overlays which composite over (or interrupt) the active screen:
// the screensaver and, later, a general toast queue. It runs entirely on the
// LVGL loop via a dispatcher lv_timer, so it behaves identically on every
// platform that pumps lv_timer_handler() — the Pi CPython runtime, the ESP32
// MicroPython LVGL task, and the desktop runner.
//
// Producers on other threads (e.g. the Pi's SD-card detector threads) reach the
// manager through thread-safe entry points and never touch LVGL directly (LVGL
// is not thread-safe). State mutated from a producer thread is guarded by
// overlay_manager_lock()/overlay_manager_unlock(), which default to no-ops for
// single-threaded hosts (desktop, the ESP32 task, web) and are overridden by a
// host that enqueues cross-thread (the Pi .so provides a real mutex). This
// mirrors the existing seedsigner_lvgl_on_button_selected() weak-symbol pattern.

// Initialize the manager and start its dispatcher lv_timer. Call exactly once,
// AFTER the LVGL display + input devices exist. Idempotent.
void overlay_manager_init(void);

// --- Screensaver ----------------------------------------------------------

// Idle time before the screensaver activates, in milliseconds. 0 disables it
// (and dismisses it if currently active). Runtime-updatable (toward a user
// setting). The idle clock is LVGL's lv_display_get_inactive_time(), so it
// resets on ANY input — fixing the old "fires mid-navigation" bug.
void overlay_manager_set_screensaver_timeout(uint32_t ms);
uint32_t overlay_manager_get_screensaver_timeout(void);

// Per-screen screensaver opt-out, carried as an LVGL object flag stamped on the
// screen's root object. A view that set allow_screensaver=false has this flag
// applied to its screen (in the shared scaffold, screen_scaffold.cpp); the dispatcher skips
// activation while the active screen carries it. The flag rides on the object,
// so it auto-clears on every screen swap — no global bookkeeping.
//
// LVGL reserves LV_OBJ_FLAG_USER_1..4 as free app-defined bits; this is the
// registry for the one we claim. A flag bit fits a single boolean with no
// allocation and without spending the object's lone user_data slot; if a screen
// later needs to carry MORE per-screen overlay data, promote to a struct on
// user_data (allow_screensaver becomes a field) and retire this flag.
// The macro expands to an lvgl.h symbol, so include lvgl.h at the use site.
#define SS_OBJ_FLAG_NO_SCREENSAVER LV_OBJ_FLAG_USER_1

bool overlay_manager_is_screensaver_active(void);

// Programmatically dismiss the screensaver (no-op if not active). LVGL-thread
// only (it loads/deletes screens).
void overlay_manager_dismiss_screensaver(void);

// --- Toast queue ----------------------------------------------------------

// Thread-safe producer entry point: request a toast from ANY thread. The spec's
// strings are deep-copied under overlay_manager_lock() here, and the request is
// realized on the LVGL loop by the dispatcher — so a producer thread (e.g. the Pi's
// SD-card detector) never touches LVGL directly (LVGL is not thread-safe). On the
// LVGL thread you may instead call toast_overlay_show() directly for an immediate
// build (the desktop-tools screen wrapper does).
//
// One toast at a time: a newer request replaces an older un-drained one, and a shown
// toast replaces whatever is already on screen (Python runs a single toast). Showing
// a toast dismisses the screensaver if it is up ("new toasts break out of the
// screensaver", Python parity) and suppresses screensaver activation while the toast
// is displayed, so the two overlays never fight over the screen.
void overlay_manager_show_toast(const toast_overlay_spec_t *spec);

// --- Lock hooks (weak; a host overrides them iff it enqueues cross-thread) -
void overlay_manager_lock(void);
void overlay_manager_unlock(void);

#ifdef __cplusplus
}
#endif

#endif // SEEDSIGNER_OVERLAY_MANAGER_H
