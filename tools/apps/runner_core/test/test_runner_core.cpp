// Headless smoke test for runner_core. Because runner_core is SDL-free it can
// render screens into its RGB565 framebuffer with no display — so this exercises
// the exact path both the native and WASM runners use, and asserts that screens
// actually produce pixels, resolution switching works, malformed JSON is
// rejected (not fatal), and the scenario catalog parses.
//
// Returns 0 on success, non-zero on any failed assertion. Intended for CI.

#include "runner_core.h"
#include "overlay_manager.h"

#include "lvgl.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static int count_nonzero(const uint16_t* fb, int n) {
    int c = 0;
    for (int i = 0; i < n; ++i)
        if (fb[i] != 0) ++c;
    return c;
}

static int failures = 0;
static void check(bool cond, const char* msg) {
    printf("[%s] %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) ++failures;
}

// Advance LVGL time by total_ms WITHOUT any input, in steps small enough that
// the overlay dispatcher (200ms) fires several times. No push_key/set_pointer
// => indev reads stay RELEASED => the idle clock grows (lv_indev only resets
// last_activity_time on a press), so this simulates a genuine idle period.
static void idle_tick(uint32_t total_ms) {
    const uint32_t step = 100;
    for (uint32_t t = 0; t < total_ms; t += step) runner_core::tick(step);
}

int main(int argc, char** argv) {
    const char* scenarios_path = (argc > 1) ? argv[1] : "tools/scenarios/scenarios.json";

    runner_core::init(240, 240);
    check(runner_core::width() == 240 && runner_core::height() == 240, "init 240x240");

    // Start the overlay manager's dispatcher (inert until a timeout is set).
    overlay_manager_init();

    const std::string button_list =
        "{\"top_nav\":{\"title\":\"Settings\",\"show_back_button\":true},"
        "\"button_list\":[\"Language\",\"Camera\",\"Network\"]}";

    check(runner_core::load_screen("button_list_screen", button_list),
          "load button_list_screen");
    for (int i = 0; i < 4; ++i) runner_core::tick(16);
    int nz = count_nonzero(runner_core::framebuffer(), 240 * 240);
    printf("     button_list non-zero pixels: %d\n", nz);
    check(nz > 200, "button_list rendered visible pixels");

    // Resolution switch + re-render.
    runner_core::resize(480, 320);
    check(runner_core::width() == 480 && runner_core::height() == 320, "resize to 480x320");
    runner_core::load_screen("button_list_screen", button_list);
    for (int i = 0; i < 4; ++i) runner_core::tick(16);
    int nz2 = count_nonzero(runner_core::framebuffer(), 480 * 320);
    printf("     after-resize non-zero pixels: %d\n", nz2);
    check(nz2 > 200, "button_list rendered after resize");

    // Reset to the base resolution before the per-screen sweep.
    runner_core::resize(240, 240);

    // Scenario catalog parses and groups by screen.
    std::vector<runner_core::ScreenScenarios> groups;
    bool sc_ok = runner_core::load_scenarios_grouped(scenarios_path, groups);
    printf("     scenario groups: %zu\n", groups.size());
    check(sc_ok && groups.size() >= 3, "scenarios.json parsed into groups");

    // Render every registered screen using its real default scenario context
    // (guaranteed valid), exercising the whole catalog with no crash.
    for (const auto& g : groups) {
        const std::string& ctx = g.scenarios.empty() ? std::string("{}")
                                                      : g.scenarios.front().context_json;
        bool ok = runner_core::load_screen(g.screen, ctx);
        for (int i = 0; i < 3; ++i) runner_core::tick(16);
        std::string m = std::string("rendered ") + g.screen + " (default scenario)";
        check(ok, m.c_str());
    }

    bool found_variations = false;
    for (const auto& g : groups)
        if (g.scenarios.size() > 1) found_variations = true;
    check(found_variations, "at least one screen has merged variations");

    // Malformed JSON must be rejected via a thrown exception, not a crash.
    bool threw = false;
    try {
        runner_core::load_screen("button_list_screen", "{ not valid json ");
    } catch (...) {
        threw = true;
    }
    check(threw, "malformed JSON throws (caller can keep last good render)");

    // Unknown screen reported, not invoked.
    check(!runner_core::has_screen("does_not_exist"), "unknown screen detected");
    check(!runner_core::load_screen("does_not_exist", "{}"), "unknown screen not loaded");

    // -----------------------------------------------------------------------
    // Overlay manager — screensaver idle-watch (headless, deterministic).
    // lv_display_trigger_activity(NULL) simulates input: it resets the idle
    // clock exactly as a real keypress does (lv_indev sets last_activity_time
    // only on LV_INDEV_STATE_PRESSED), so we can drive activate/dismiss without
    // the input-mode plumbing.
    // -----------------------------------------------------------------------
    printf("\n-- overlay manager: screensaver idle-watch --\n");
    runner_core::load_screen("button_list_screen", button_list);
    for (int i = 0; i < 4; ++i) runner_core::tick(16);
    lv_obj_t* home = lv_scr_act();

    // Disabled by default (timeout 0): never activates, even when idle.
    overlay_manager_set_screensaver_timeout(0);
    lv_display_trigger_activity(NULL);
    idle_tick(1500);
    check(!overlay_manager_is_screensaver_active(), "timeout=0 => screensaver stays disabled");

    // Activates only once the idle timeout elapses.
    overlay_manager_set_screensaver_timeout(1000);
    lv_display_trigger_activity(NULL);
    idle_tick(700);
    check(!overlay_manager_is_screensaver_active(), "below timeout => not yet active");
    idle_tick(600);  // ~1300ms idle total > 1000ms
    check(overlay_manager_is_screensaver_active(), "idle past timeout => screensaver active");
    check(lv_scr_act() != home, "screensaver is the active screen");

    // Any input wakes it and restores the prior screen.
    lv_display_trigger_activity(NULL);
    idle_tick(400);  // < timeout, so it dismisses and stays dismissed
    check(!overlay_manager_is_screensaver_active(), "input => screensaver dismissed");
    check(lv_scr_act() == home, "prior screen restored after dismiss");

    // Per-screen opt-out: a screen carrying SS_OBJ_FLAG_NO_SCREENSAVER never
    // triggers the saver. The flag rides on the screen object (here `home`, the
    // active screen), exactly as the view-stamped policy works in production.
    lv_obj_add_flag(home, SS_OBJ_FLAG_NO_SCREENSAVER);
    lv_display_trigger_activity(NULL);
    idle_tick(1500);
    check(!overlay_manager_is_screensaver_active(), "no-screensaver flag => no activation");
    lv_obj_remove_flag(home, SS_OBJ_FLAG_NO_SCREENSAVER);

    // Re-activates once the flag is cleared and idle again (timeout still 1000ms).
    lv_display_trigger_activity(NULL);
    idle_tick(1300);
    check(overlay_manager_is_screensaver_active(), "flag cleared + idle => active again");

    // Disabling (timeout 0) while active dismisses it and restores the screen.
    overlay_manager_set_screensaver_timeout(0);
    idle_tick(400);
    check(!overlay_manager_is_screensaver_active(), "timeout=0 while active => dismissed");
    check(lv_scr_act() == home, "prior screen restored after disable");

    printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
