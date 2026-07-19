// Headless smoke test for runner_core. Because runner_core is SDL-free it can
// render screens into its RGB565 framebuffer with no display — so this exercises
// the exact path both the native and WASM runners use, and asserts that screens
// actually produce pixels, resolution switching works, malformed JSON is
// rejected (not fatal), and the scenario catalog parses.
//
// Returns 0 on success, non-zero on any failed assertion. Intended for CI.

#include "runner_core.h"
#include "overlay_manager.h"
#include "locale_loader.h"   // ss_load_locale / ss_unload_locale / ss_reap_retired
#include "components.h"      // button_text_label / button_set_label_marquee (address-list click test)

#include "lvgl.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Filesystem pack provider for the locale loader: reads lang-packs/<locale>/<file>
// into a reusable scratch buffer (the loader copies what it keeps before the next
// call). Mirrors the screenshot generator's provider; lets the headless test drive
// the real per-locale font registration path. Resolves relative to the test's CWD,
// which CI runs from the repo root (where lang-packs/ lives).
static std::vector<uint8_t> g_pack_scratch;
static bool fs_pack_provider(const char* locale, const char* file,
                             const uint8_t** bytes, size_t* len, void* /*user*/) {
    std::string path = std::string("lang-packs/") + locale + "/" + file;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fprintf(stderr, "test: missing pack file: %s\n", path.c_str());
        return false;
    }
    g_pack_scratch.assign((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
    *bytes = g_pack_scratch.data();
    *len = g_pack_scratch.size();
    return true;
}

// Register a pack from its on-disk manifest.json so the render layer learns the locale's
// policy (chain/rtl/shaping/roles). Screens bakes no locale table — every non-English
// locale is self-described by its pack — so a host must register it before ss_load_locale.
// Mirrors what each production host does at pack discovery.
static bool register_pack(const char* locale) {
    std::string path = std::string("lang-packs/") + locale + "/manifest.json";
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string bytes((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    return ss_register_pack_manifest(bytes.data(), bytes.size());
}

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

// Capture the most recent on_button_selected(index, label) the screen layer reported.
// A STRONG definition here overrides the weak no-op default in components.cpp for this
// test binary, standing in for the host — the address-list click test below asserts what
// the host actually receives.
static uint32_t    g_last_selected_index = 0;
static std::string g_last_selected_label;
extern "C" void seedsigner_lvgl_on_button_selected(uint32_t index, const char* label) {
    g_last_selected_index = index;
    g_last_selected_label = label ? label : "";
}

// Recursively find the address-explorer list's row-0 button: the first lv_button whose
// tagged text label begins with "0:" ("0:{address}"). The top-nav back/power buttons carry
// icon glyphs, never a "0:" prefix, so they are skipped naturally.
static lv_obj_t* find_address_row_button(lv_obj_t* obj) {
    if (!obj) return nullptr;
    if (lv_obj_check_type(obj, &lv_button_class)) {
        lv_obj_t* lbl = button_text_label(obj);
        if (lbl) {
            const char* t = lv_label_get_text(lbl);
            if (t && t[0] == '0' && t[1] == ':') return obj;
        }
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* found = find_address_row_button(lv_obj_get_child(obj, i));
        if (found) return found;
    }
    return nullptr;
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

    // -----------------------------------------------------------------------
    // Address-explorer list: a row click reports the row's CANONICAL FULL text,
    // identical whether the row is AT REST (touch mode: abbreviated on screen) or
    // REVEALED (hardware focus: the full address swapped in + marquee-scrolled).
    // Regression for the bug where the click handed the host the abbreviated string
    // in touch mode but the full one under hardware focus, because it read the LIVE
    // label (which differs by input mode). It now reads the registered focus-reveal
    // "full" form, so both report one consistent string. A directly-sent CLICKED has
    // no pointer indev, so it reaches on_button_selected exactly like the nav layer's
    // hardware/touch click paths do.
    // -----------------------------------------------------------------------
    printf("\n-- address-explorer list: click return is input-mode-independent --\n");
    {
        runner_core::resize(240, 240);
        std::string list_ctx = "{}";
        for (const auto& g : groups)
            if (g.screen == "tools_address_explorer_address_list_screen" && !g.scenarios.empty())
                list_ctx = g.scenarios.front().context_json;

        runner_core::load_screen("tools_address_explorer_address_list_screen", list_ctx);
        for (int i = 0; i < 3; ++i) runner_core::tick(16);

        lv_obj_t* row0 = find_address_row_button(lv_scr_act());
        check(row0 != nullptr, "address list: found row 0 button");
        if (row0) {
            // At rest (touch analog): force the row to its abbreviated "{i}:{head}…{tail}"
            // form. (On load in hardware mode bind_screen_navigation reveals row 0 via
            // initial_selected_index=0, so un-reveal it first to reproduce the touch state
            // where no row is focused and the on-screen label is truncated.)
            button_set_label_marquee(row0, false);
            g_last_selected_label.clear();
            lv_obj_send_event(row0, LV_EVENT_CLICKED, NULL);
            std::string at_rest = g_last_selected_label;

            // Revealed (hardware-focus analog): swap the full address in, then click.
            button_set_label_marquee(row0, true);
            g_last_selected_label.clear();
            lv_obj_send_event(row0, LV_EVENT_CLICKED, NULL);
            std::string revealed = g_last_selected_label;

            printf("     at-rest click  -> \"%s\"\n", at_rest.c_str());
            printf("     revealed click -> \"%s\"\n", revealed.c_str());

            check(!at_rest.empty(), "address list: at-rest click reported a label");
            check(at_rest == revealed,
                  "address list: click return identical at rest vs revealed (input-mode-independent)");
            check(at_rest.find("...") == std::string::npos,
                  "address list: click return is the FULL address (no abbreviation ellipsis)");
            check(at_rest.rfind("0:", 0) == 0,
                  "address list: click return keeps the row index prefix");
        }
    }

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

    // -----------------------------------------------------------------------
    // Locale font registration: dedup + retire/reap lifecycle (font-memory
    // Task A). Loads real packs from lang-packs/<locale>/, renders, then unloads
    // and reaps across a screen swap. Exercises the paths where roles SHARE a
    // tiny_ttf instance and the reap must destroy each one exactly once:
    //   - CJK Primary (zh): large_button == top_nav_title share a 23px script @240
    //   - shaping Primary (hi): glyph-run path + Primary share, reaped on switch
    //   - Fallback pack (ru): button == large_button share a 23px Cyrillic leaf @320
    // A per-registration (double) free of a shared script would crash here — loudly
    // under ASan, and corrupts the heap otherwise. Reaching the asserts means the
    // shared-instance bookkeeping (and the baseline restore after reap) held.
    // -----------------------------------------------------------------------
    printf("\n-- locale font dedup + retire/reap lifecycle --\n");
    {
        const std::string loc_ctx =
            "{\"top_nav\":{\"title\":\"Settings\",\"show_back_button\":true},"
            "\"button_list\":[\"Language\",\"Camera\",\"Network\"]}";

        // Load a locale, render it, then move OFF it the way production does:
        // retire its fonts (unload), build a NEW screen so the old one (holding raw
        // font pointers) is deleted, then reap. This is the exact sequence that
        // double-frees a shared script if reap destroys per-registration.
        auto load_render_reap = [&](const char* loc, int w, int h) -> bool {
            runner_core::resize(w, h);
            register_pack(loc);  // learn the locale's policy from its manifest (no baked table)
            bool loaded = ss_load_locale(loc, fs_pack_provider, nullptr);
            runner_core::load_screen("button_list_screen", loc_ctx);
            for (int i = 0; i < 3; ++i) runner_core::tick(16);
            int px = count_nonzero(runner_core::framebuffer(), w * h);
            ss_unload_locale();                                  // retire fonts
            runner_core::load_screen("button_list_screen", loc_ctx);  // old screen deleted
            for (int i = 0; i < 3; ++i) runner_core::tick(16);
            ss_reap_retired();                                   // destroy retired (once each)
            return loaded && px > 200;
        };

        check(load_render_reap("zh_Hans_CN", 240, 240),
              "zh_Hans_CN load/render/reap (CJK Primary share @240)");
        check(load_render_reap("hi", 240, 240),
              "hi load/render/reap (shaping Primary + glyph runs)");
        check(load_render_reap("ru", 480, 320),
              "ru load/render/reap (Fallback pack share @480x320)");

        // Back on the English baseline: the restore must repoint every role at a
        // live compiled-in font — no dangling pointer into the just-reaped locale.
        runner_core::resize(240, 240);
        runner_core::load_screen("button_list_screen", loc_ctx);
        for (int i = 0; i < 3; ++i) runner_core::tick(16);
        check(count_nonzero(runner_core::framebuffer(), 240 * 240) > 200,
              "english baseline renders after locale reap (no dangling font)");
    }

    // -----------------------------------------------------------------------
    // Runtime pack registration: screens bakes NO locale table, so a language pack
    // becomes loadable purely from its own manifest.json — the "add a language by
    // copying it onto the SD card / into lang-packs, no firmware rebuild" path.
    // We assert the DISCOVERY half here (which files a locale needs is driven entirely
    // by the registered manifest, so ss_load_locale would then fetch them); the
    // byte-load half is the same provider path the real-pack tests above already
    // exercise. Also asserts fail-closed parsing (hostile input on a user-writable
    // partition) and that clearing the registry leaves a locale unknown (no baked fallback).
    // -----------------------------------------------------------------------
    printf("\n-- runtime SD-pack manifest registration --\n");
    {
        ss_clear_pack_manifests();
        auto reg = [](const std::string& j) {
            return ss_register_pack_manifest(j.data(), j.size());
        };
        auto files = [](const char* loc) {
            return std::string(ss_locale_pack_files(loc));  // copy: static buffer
        };

        // A code absent from the runtime set: no files (there is no compiled table).
        check(files("zz_Test") == "[]",
              "unknown locale needs no pack files before registration");

        // Primary (CJK-style) pack: one <code>.ttf serves every role.
        check(reg(R"({"locale":"zz_Test","source_family":"NotoSansSC","chain":"primary"})"),
              "register primary runtime pack");
        check(files("zz_Test") == "[\"zz_Test.ttf\"]",
              "runtime primary pack drives file discovery (no recompile)");

        // Fallback (block-range) pack: Regular + SemiBold weights.
        check(reg(R"({"locale":"yy","source_family":"OpenSans","chain":"fallback","unicode_range":"U+0500-052F"})"),
              "register fallback runtime pack");
        check(files("yy") == "[\"yy_regular.ttf\",\"yy_semibold.ttf\"]",
              "runtime fallback pack yields two weight files");

        // Complex-script pack: also needs runs.bin.
        check(reg(R"({"locale":"xx","source_family":"NotoSansDevanagari","chain":"primary","shaping":true,"script":"Deva"})"),
              "register shaping runtime pack");
        {
            std::string f = files("xx");
            check(f.find("xx.ttf") != std::string::npos &&
                  f.find("runs.bin") != std::string::npos,
                  "runtime shaping pack requests runs.bin");
        }

        // Fail closed: malformed / incomplete manifests register nothing.
        check(!reg("{ this is not json"), "malformed JSON rejected");
        check(!reg(R"({"source_family":"NotoSansSC","chain":"primary"})"),
              "manifest without locale rejected");
        check(!reg(R"({"locale":"","source_family":"X"})"), "empty locale rejected");
        check(files("nope") == "[]", "a rejected manifest leaves no entry");

        // Re-registering a code REPLACES the prior runtime entry (idempotent by code) —
        // e.g. an SD rescan picking up an updated pack. zz_Test was a Primary above; make
        // it a shaping pack and confirm discovery now reflects the new manifest.
        check(reg(R"({"locale":"zz_Test","source_family":"NotoSansDevanagari","chain":"primary","shaping":true,"script":"Deva"})"),
              "re-register zz_Test as a shaping pack");
        check(files("zz_Test").find("runs.bin") != std::string::npos,
              "re-registered zz_Test now requests runs.bin (replace by code)");

        // Clear: every runtime entry vanishes. Screens has no baked fallback, so the
        // locale is simply unknown again (no compiled descriptor to fall back to).
        ss_clear_pack_manifests();
        check(files("zz_Test") == "[]", "clear drops runtime packs (no baked fallback)");
        check(files("ru") == "[]",
              "a real locale like 'ru' is unknown until its pack is registered");
    }

    // -----------------------------------------------------------------------
    // Camera scan/entropy screens reset the idle clock on teardown, so a long
    // input-less scan (the user lining up a QR) doesn't leave a stale idle clock
    // that makes the overlay dispatcher fire the screensaver over the freshly-loaded
    // successor screen (e.g. Select Signer) instead of showing it. These screens opt
    // out of the screensaver (apply_screensaver_policy, default_allow=false), so they
    // carry SS_OBJ_FLAG_NO_SCREENSAVER and load_screen_and_cleanup_previous wires the
    // teardown reset off that flag automatically. Timeout is set to 0 here so the saver
    // never activates and we can measure the idle clock directly.
    // -----------------------------------------------------------------------
    printf("\n-- camera screens reset the idle clock on teardown --\n");
    {
        overlay_manager_set_screensaver_timeout(0);
        runner_core::load_screen("camera_preview_overlay_screen", "{}");
        for (int i = 0; i < 3; ++i) runner_core::tick(16);

        // Grow the idle clock with NO input (a scan runs input-less while the QR is aligned).
        idle_tick(1500);
        check(lv_display_get_inactive_time(NULL) >= 1000,
              "camera screen: idle clock went stale with no input");

        // Host dismisses the scan by loading the next screen -> the camera screen is deleted
        // -> its teardown callback resets the idle clock.
        runner_core::load_screen("button_list_screen", button_list);
        for (int i = 0; i < 2; ++i) runner_core::tick(16);
        check(lv_display_get_inactive_time(NULL) < 500,
              "camera teardown reset the idle clock (successor gets a full screensaver window)");
    }

    printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL OK" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
