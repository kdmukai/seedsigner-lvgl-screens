# Screen conformance — pilot run report (2026-07-10)

Overnight autonomous run: full-corpus review → conformance spec → safe helper
extractions → 5 pilot screens conformed. Companion to
`docs/screen-conformance-spec.md` (the authoritative rules). Branch:
`refactor/screen-conformance`. **No PR opened yet — pending review of this
demonstration.**

## What happened

1. **Digest** (39 agents): profiled all 52 in-scope translation units, cataloged
   approach drift on 8 dimensions, adversarially verified 16 duplication
   clusters, selected the exemplar, flagged 78 suspected bugs (16 medium,
   62 low, none high — none fixed; ledger below).
2. **Spec** written and committed, then amended with two user-directed policies:
   **no English content defaults** (all user-visible text host-supplied +
   localized; missing required content throws) and **Python provenance without
   line numbers** (transitional linkage).
3. **Extractions** (spec ledger #1-#8), three commits, each pixel-gated:
   `parse_optional_screen_json_ctx`, `ensure_top_nav_structure` +
   `require_top_nav_title`, `bind_screen_navigation(cfg, screen, index)`
   overload (18 sites), aux-key surface (`nav_aux_key_index` + the missing
   `seedsigner_lvgl_on_aux_key` prototype + weak-default relocation),
   `monospace_char_width` (6 sites), `kb_side_panel_geometry` (3 sites),
   glyph-ink clone deletion, `bottom_button_top_y` (4 sites).
4. **Pilots** — the demonstration of what full conformance looks like:
   - `tools_calc_final_word_done_screen` — the exemplar, polished into the
     literal reference implementation (d41274e)
   - `button_list_screen` — the workhorse (910e04b)
   - `loading_spinner_screen` — chrome-free stateful tier (c4de91f)
   - `large_icon_status_screen` — monolith-era custom body + 4 named geometry
     passes (e2b2921)
   - `psbt_change_details_screen` — PSBT detail tier + content-policy
     conversions (11665fc)

## Verification

- **Method:** every wave rebuilt the screenshot generator and regenerated the
  six representative locales (one per glyph-rendering type: en=baked Latin,
  ru=Cyrillic pack, ja=CJK pack, fa=LVGL-RTL, hi=shaped runs LTR, ur=shaped
  runs RTL), then byte-compared all 3,180 images against the pre-change
  baseline (11,270 PNGs, all 23 locales, rendered before any edit), with
  ImageMagick pixel comparison as the tie-breaker on byte mismatch.
- **Result: zero pixel diffs at every gate** (6 gate runs: smoke, extractions
  A/B/C, exemplar, pilots).
- Additional checks: runner_core test target built clean; `nm` verified the
  relocated weak host callback still loses to the screen_runner's strong
  override (the MinGW cross-TU weak-linkage class of 7f89913).

## Behavior deltas (malformed-cfg paths only — never hit by scenarios)

The content policy converts silent English fallbacks into screen-named throws.
Well-formed (host/view-layer) configs render pixel-identically; these deltas
fire only on developer-error configs:

- `tools_calc_final_word_done_screen`: missing `top_nav.title` /
  `button_list` / `final_word` / `fingerprint` / `fingerprint_label` now throw
  (previously English defaults / derivation). `mnemonic_word_length` no longer
  read.
- `button_list_screen`: missing/malformed `top_nav.title` now throws cleanly
  (previously nlohmann const `operator[]` assert/UB — the flagged crash path).
- `psbt_change_details_screen`: missing `top_nav.title` /
  `address_type_label` / `button_list` / `btc_amount` now throw; `verified_text`
  required when `is_verified` is true; empty `button_list` array now throws.
- Exception-text wording normalized to `"<screen_name>: <key> is required..."`
  in conformed files (never rendered; hosts treat any throw as fatal).

## Deferred by design (documented, not done)

- **Status-family content defaults** (`apply_status_type_defaults` injects
  English title/button label): shared by the whole status family — converts to
  require-and-throw when that family goes through rollout (spec ledger #7 note).
- **DOCUMENTED extraction clusters** (#10-#16): simple-text skeleton,
  keypad-sink/indev-attach promotion, upper-body grow trio, compose-and-overlay
  decision, ink-anchored label, UTF-8 decoder unification, keyboard-family
  wiring — each needs a design or pixel-policy decision (spec §10, §11).
- **Consolidation ledger** (spec §12): input_profile→navigation merge, locale
  subsystem reorganization, camera-overlay twins, header hygiene, dead-code
  deletions.
- **Tier-2 lifecycle deviations** found during pilots (report-only):
  `loading_spinner` allocates a pure-POD ctx with `new`/`delete` (spec says
  `lv_malloc` family) and its cleanup callback lacks the event-code re-check.
  Likely more of the same in the other Tier-2 screens — audit during rollout.

## Rollout: 31 screens remaining

Each gets the full spec §13 checklist (banner, curated includes, dividers,
naming, content policy, helper adoption). Known mechanical work per screen
beyond the checklist (from the verified clusters):

| Screen | Exemplar score | Known mechanical work (beyond full checklist) |
|---|---|---|
| power_off_not_required_screen | 9 | top_nav defaults -> structure+require (content policy) |
| donate_screen | 8 | top_nav defaults -> structure+require (content policy) |
| reset_screen | 8 | top_nav defaults -> structure+require (content policy) |
| settings_locale_picker_screen | 8 | kitchen-sink includes |
| psbt_address_details_screen | 8 | top_nav defaults -> structure+require (content policy) |
| seed_mnemonic_entry_screen | 8 | kitchen-sink includes |
| seed_finalize_screen | 8 | kitchen-sink includes, top_nav defaults -> structure+require (content policy) |
| seed_export_xpub_details_screen | 8 | kitchen-sink includes, top_nav defaults -> structure+require (content policy) |
| seed_address_verification_screen | 8 | top_nav defaults -> structure+require (content policy) |
| multisig_wallet_descriptor_screen | 8 | top_nav defaults -> structure+require (content policy) |
| io_test_screen | 7 | top_nav defaults -> structure+require (content policy) |
| psbt_op_return_screen | 7 | top_nav defaults -> structure+require (content policy) |
| qr_display_screen | 7 | kitchen-sink includes |
| seed_transcribe_zoomed_qr_screen | 7 | kitchen-sink includes |
| keyboard_screen | 7 | kitchen-sink includes |
| seed_words_screen | 7 | kitchen-sink includes, top_nav defaults -> structure+require (content policy) |
| seed_sign_message_confirm_address_screen | 7 | top_nav defaults -> structure+require (content policy) |
| tools_address_explorer_address_list_screen | 7 | top_nav defaults -> structure+require (content policy) |
| opening_splash_screen | 6 | kitchen-sink includes |
| psbt_math_screen | 6 | top_nav defaults -> structure+require (content policy) |
| seed_transcribe_whole_qr_screen | 6 | top_nav defaults -> structure+require (content policy) |
| seed_add_passphrase_screen | 6 | kitchen-sink includes, top_nav defaults -> structure+require (content policy) |
| seed_sign_message_confirm_message_screen | 6 | top_nav defaults -> structure+require (content policy) |
| screensaver_screen | 5 | kitchen-sink includes |
| psbt_overview_screen | 5 | kitchen-sink includes, top_nav defaults -> structure+require (content policy) |
| tools_calc_final_word_screen | 5 | top_nav defaults -> structure+require (content policy) |
| main_menu_screen | 4 | kitchen-sink includes |
| settings_qr_confirmation_screen | 4 | top_nav defaults -> structure+require (content policy) |
| seed_review_passphrase_screen | 4 | kitchen-sink includes, top_nav defaults -> structure+require (content policy) |
| camera_preview_overlay_screen | 4 | kitchen-sink includes |
| camera_entropy_overlay_screen | 3 | kitchen-sink includes |

Suggested wave order: (1) the simple/high-score screens (fast, low risk, build
momentum and consistency mass); (2) the PSBT + seed detail tiers (share the
psbt_change_details precedent); (3) keyboard family together with extraction
cluster #16; (4) chrome-free/QR tier; (5) status family + `apply_status_type_defaults`
content-policy conversion; (6) compose-and-overlay pair after the §8 decision.

## Cost (measured)

- Digest workflow: 39 agents, 3.19M subagent tokens, ~57 min.
- Extraction agents (3): ~455k subagent tokens.
- Exemplar + 4 pilot agents: ~467k subagent tokens.
- Total subagent spend: **~4.1M tokens**; plus main-session orchestration.
  Wall clock ~5 hours including gates (each gate ≈ 4 min: rebuild + 6-locale
  regeneration + 3,180 comparisons).

## Suspected-bug ledger (78 flags — none fixed)

Digest-reported, profile-agent confidence (not adversarially verified except
where noted; treat as leads, not confirmed defects). The five pilot files'
comment-only defects (stale docs) were fixed as comment conformance; every
behavioral flag below is untouched.

### Medium severity

- **screens/io_test_screen.cpp** — io_test_screen.cpp:187 — input_profile_set_mode(INPUT_MODE_HARDWARE)
  Forces the GLOBAL input mode to hardware and never restores it when the screen is torn down (io_test_cleanup_cb deletes only the group/ctx). On any device that was in INPUT_MODE_TOUCH, every screen after exiting the I/O test inherits hardware mode (touch scroll-vs-focus behavior flips) until something else calls set_mode. The comment's defense ('gated off on touch-only builds by the app') is an out-of-repo promise, and the primary ESP32 target board is a touch panel.

- **screens/loading_spinner_screen.cpp** — loading_spinner_screen.cpp:138-209 (whole entry function; contrast screen_scaffold.cpp:271-273)
  The spinner screen never stamps SS_OBJ_FLAG_NO_SCREENSAVER (and ignores cfg allow_screensaver entirely). overlay_manager.cpp:113 suppresses the screensaver only when the ACTIVE screen carries that flag; during a long blocking host task the LVGL timer keeps ticking with zero input, so the overlay-manager idle-watch can replace the loading spinner with the screensaver mid-task — precisely the screen where prolonged input-idle is guaranteed.

- **screens/settings_qr_confirmation_screen.cpp** — settings_qr_confirmation_screen() step 2 overlay (lines 191-208) — labels created after button_list_screen has already run load_screen_and_cleanup_previous
  The overlaid config_name/status_message labels never receive the RTL (apply_rtl_text_to_labels) or complex-script glyph-run post-passes, which run only inside load_screen_and_cleanup_previous at load time. status_message is host-localized, so on RTL (ur/fa) or shaped (hi/th) locales the status paragraph renders with wrong base direction / unshaped codepoints while the rest of the screen is processed correctly.

- **screens/seed_transcribe_whole_qr_screen.cpp** — seed_transcribe_whole_qr_screen() line 339 + encode_transcription_qr() lines 183-190
  qr_mode is never validated: an unrecognized string (e.g. a host typo like 'bytes') silently takes the auto branch instead of throwing like the sibling QR screens do. For a hand-transcription screen a silently different encode mode yields a module matrix that no longer matches the Pi Zero's render, defeating the screen's pixel-parity purpose without any error.

- **screens/seed_mnemonic_entry_screen.cpp** — mnemonic_lock_in lines 331-337 interacting with the initial_letters==1 seeding (lines 583-585) in TOUCH mode
  lock_in's first branch (`letters.back() != " "` -> just append a fresh slot) assumes the live slot already equals the pressed letter — true only under joystick float. In touch mode with exactly ONE initial letter (e.g. initial_letters:"a"), letters==["a"] with no float ever occurring; the first tap on any letter 'p' takes that branch, locks in the stale 'a' and silently drops the tapped 'p' (user must tap twice, and the first tap can lock a letter they never chose). Hardware mode is immune because mnemonic_float keeps back()==hovered==clicked.

- **screens/tools_calc_final_word_screen.cpp** — Overlay creation after button_list_screen() has already run load_screen_and_cleanup_previous (lines 237-300)
  The RTL and glyph-run passes (apply_rtl_text_to_labels / apply_glyph_runs_to_labels in screen_scaffold.cpp lines 113-126) run once at screen load — the overlay captions (host-LOCALIZED strings: your_input_text, checksum_label, final_word_text) are added after and never receive them, so shaped locales (hi/th) render raw codepoints and RTL (ur) keeps LTR base direction on this screen. Same latent gap in settings_qr_confirmation_screen's status_message overlay.

- **screens/seed_review_passphrase_screen.cpp** — wrap_passphrase, num_lines clamp at lines 73-75 interacting with the lo-clamp at line 91
  When the passphrase needs more than max_lines(3) lines (len > 3*max_cpl, ~54+ chars at 240px), num_lines is clamped but the text is NOT truncated or font-reduced: the lo = len - rem_lines*max_cpl clamp then forces cuts that make early lines exceed max_cpl (e.g. len=100, max_cpl=10 yields an 80-char first line), so the label renders wider than the screen. Python auto-fits the font size downward instead; this port fixed the font and has no fallback.

- **screens/seed_address_verification_screen.cpp** — lines 126-129 (inline network -> net_color mapping)
  Only the long names "testnet"/"regtest" are recognized; the shared network_color() contract (screen_helpers.cpp:575-583) explicitly documents that the JSON contract uses the Python SettingsConstants codes "M"/"T"/"R". A host passing cfg["network"]="T" (accepted everywhere else, e.g. btc_amount_from_cfg and the PSBT detail screens) silently gets mainnet-orange highlights here. It also skips resolve_address_network()'s address-prefix inference, so an unmistakably-testnet tb1 address with no cfg network renders mainnet orange, unlike psbt_address_details.

- **screens/tools_address_explorer_address_list_screen.cpp** — ae_abbreviate/ae_format_full vs Python __post_init__ (tools_screens.py:501)
  Rendered-character parity deviation: Python's at-rest label is a symmetric split f"{i}:{addr[:half]}...{addr[-half:]}" and its reveal label is the plain f"{i}:{address}"; the C++ at-rest label is a prefix-aware priority truncation with internal spaces and the reveal is space-formatted ('<prefix> <first-7> <middle> <last-7>'). The banner comment claims the truncation is 'exactly like Python's __post_init__' but only the width MATH matches — the emitted characters differ, so this screen cannot be pixel-identical to the Python reference. Also contradicts components.h's label_set_focus_reveal doc ('full "{i}:{address}" on focus'). Deliberate-looking, but it violates the recorded pixel-parity mandate and should be adjudicated

- **screen_scaffold.cpp** — create_top_nav_screen_scaffold lines 282 (const auto &tn = cfg["top_nav"]) and 289 (tn["title"].get<std::string>())
  Unchecked const nlohmann operator[] on possibly-missing keys: for a const json, operator[] with a missing key is documented undefined behavior (assert in debug builds). parse_screen_json_ctx does NOT validate top_nav; the validation ('top_nav object is required', 'top_nav.title is required…') lives only in the dead static top_nav_from_screen_json. A host cfg missing top_nav or title hits UB/assert instead of the intended catchable std::runtime_error — an error path lost in the monolith split.

- **gui_constants.cpp** — seedsigner_latin_font(), lines 377-402: static Entry cache[16] with silent overflow
  When cache_count reaches 16 the newly created tiny_ttf font is returned but NOT cached, so every subsequent call at an uncached px creates and leaks a fresh font instance (never destroyed by policy). Screens already request ≥10 distinct base sizes (15,18,19,20,22,24,26,28,30 + variables); on desktop tools that switch among 3 multipliers at runtime the distinct scaled-px count exceeds 16, after which every screen render leaks a font + its glyph cache. Device builds (single multiplier) stay under the cap today.

- **overlay_manager.cpp** — screensaver_activate()/dismiss() vs screen_scaffold.cpp load_screen_and_cleanup_previous()
  If the host loads a new screen while the manager screensaver is active (async flow: camera done, timed transition — the manager saver swallows wake input so no button callback resets things first), load_screen_and_cleanup_previous() deletes lv_scr_act() == the saver screen. s_ss_saver_screen then dangles, s_ss_active stays true, and s_ss_saved_screen (the old app screen) leaks undeleted. The next dismiss calls lv_obj_delete on the freed saver (use-after-free) and re-loads the STALE saved screen over the host's new screen. Nothing in the load path consults overlay_manager_is_screensaver_active() (its only callers are the tests).

- **locale_fonts.cpp** — locale_role_render_px (lines 134-142)
  The Fallback large_button quirk unconditionally overrides base to 18 at non-240 profiles — including for a runtime pack whose manifest carried an EXPLICIT self-describing roles[] entry for large_button. ss_register_pack_manifest documents that an explicit roles array is honored, but this override silently discards the manifest's large_button base_size at 320/480 (and the size guard in seedsigner_register_font will then hard-reject the host's correctly-manifest-derived font size, since both sides re-derive through this same function — masking the contradiction rather than surfacing it).

- **camera_preview_overlay.cpp** — camera_preview_overlay_destroy + header's destroy doc (see camera_preview_overlay.h line 95-96)
  The header tells hosts destroy() is 'safe to skip if the parent screen is about to be deleted anyway', but skipping it leaves any in-flight 300ms completion glide registered on the heap handle: the anim exec callback then calls lv_obj_set_width/lv_label_set_text on widgets deleted with the parent (use-after-free), plus the handle leaks. The risky window coincides exactly with the documented common case — the glide is designed to play 'right before the camera tears down'. Only destroy() cancels the anim (widget deletion does not, since the anim var is the handle, not an lv_obj).

- **camera_preview_overlay.h** — camera_preview_overlay_destroy doc, lines 94-96
  'Safe to skip if the parent screen is about to be deleted anyway' is unsafe as documented: destroy() is the ONLY place the in-flight completion-glide lv_anim (var = the heap handle) is cancelled, so a host that follows this advice during the ~300ms glide window — which by design runs right at teardown — leaves an anim whose exec callback writes to freed widgets (use-after-free) and leaks the handle. The .cpp's own destroy() comment acknowledges the UAF hazard this doc waves off.

- **camera_entropy_overlay.h** — capture_icon field doc, lines 75-78
  States 'NULL → no icon', but the implementation (camera_entropy_overlay.cpp lines 189-191) defaults NULL/empty capture_icon to FontAwesomeIconConstants::CAMERA — a host cannot actually get an icon-less shutter via NULL. Header contract and renderer behavior disagree; one of them is wrong.


### Low severity

- **screens/button_list_screen.cpp** — button_list_screen.cpp:59 via screen_scaffold.cpp:282/289
  No top_nav validation or default: a cfg without "top_nav" (or without top_nav.title) reaches `const auto &tn = cfg["top_nav"]` / `tn["title"].get<std::string>()` — const nlohmann operator[] on a missing key, which is an assertion abort (debug) or UB (NDEBUG) instead of the clean runtime_error every other malformed key gets. Notably, a fully-validated reader top_nav_from_screen_json() exists at screen_scaffold.cpp:53 with the right throws — and is dead code (zero callers). donate/reset/main_menu are immune because they inject top_nav defaults locally.

- **screens/main_menu_screen.cpp** — main_menu_screen.cpp:66 (cfg.merge_patch)
  merge_patch's RFC 7396 null-deletes: a host sending {"top_nav":{"title":null}} deletes the defaulted title, and {"top_nav":"x"} replaces the object wholesale — both then hit the unguarded tn["title"] const operator[] in create_top_nav_screen_scaffold (assert/UB) instead of a clean runtime_error. The contains-based defaulting used by donate/reset cannot be null-deleted.

- **screens/main_menu_screen.cpp** — main_menu_screen.cpp:77 (count==4 gate)
  A button_list with any count other than exactly 4 silently reverts ALL FOUR labels to English — on a localized build a 3- or 5-entry cfg renders an untranslated home menu with no error, inconsistent with the throw-on-malformed policy applied to every other cfg key in the codebase. Documented as intentional ('keeps the screen legible'), but the failure is invisible to the host.

- **screens/loading_spinner_screen.cpp** — loading_spinner_screen.cpp:51-64 (banner doc comment) vs lines 79-116/205-207 (implementation)
  Stale/contradictory documentation: the banner claims the animation is an 'lv_anim' whose deletion is automatic ('no explicit teardown is needed'), but the code uses an lv_timer that is NOT object-owned and relies on the explicit loading_cleanup_cb DELETE handler (as the constant-block comment correctly says). An AI maintainer trusting the banner could remove the 'redundant' cleanup and leak/UAF the timer.

- **screens/screensaver_screen.cpp** — screensaver_timer_cb (lines 101-110) and screensaver_key_handler (lines 173-176)
  No emit-once guard on dismiss: a touch held across multiple 16 ms timer ticks, or key auto-repeat, fires SEEDSIGNER_RET_SCREENSAVER_DISMISS repeatedly until the host tears the screen down — the host may dequeue several dismiss events. opening_splash_screen solved the identical pattern with a ctx->emitted latch; the screensaver predates/lacks it.

- **screens/settings_qr_confirmation_screen.cpp** — find_first_button(screen) used to bound the centering region (lines 227-234)
  DFS returns the FIRST button in tree order; if the host passes show_back_button or show_power_button true (the cfg contract only says the reference uses false), the top_nav back/power button is found instead of the Home button, so region_bottom becomes the top-nav button's y and the px_mult>100 centering shifts labels wildly upward/off-region.

- **screens/settings_qr_confirmation_screen.cpp** — place_centered_body_text → tight_body_line_space(&BODY_FONT, text) (line 121)
  Measures the RAW cfg text rather than the label's STORED text (lv_label_get_text) — screen_helpers.cpp's own NOTE (tight_line_space:474) says callers must measure the stored text because LV_USE_ARABIC_PERSIAN_CHARS rewrites Arabic/Persian into presentation forms; measuring the logical string under-counts ink for fa and falls to the -(line_h/4) clamp instead of the correct spacing. The shared apply_body_tight_line_spacing already does this correctly.

- **screens/psbt_math_screen.cpp** — Lines 70-71, 91-93 (std::max) vs the include block (lines 1-13)
  Uses std::max five times but never includes <algorithm>; compiles only via transitive inclusion through nlohmann/json or <string>, which is toolchain/libc++-version dependent — a latent build break on a stricter standard library (this repo targets ESP-IDF, MinGW, macOS, Ubuntu, and Emscripten toolchains).

- **screens/psbt_op_return_screen.cpp** — Lines 49-51 (payload extraction, no validation)
  Neither cfg.text nor cfg.hex is validated as present: a host that sends neither (contract violation per the file's own header doc) gets a silently blank screen with only a Done button, rather than the runtime_error the sibling detail screens raise for their required fields — inconsistent failure surface that can mask host-side bugs.

- **screens/psbt_overview_screen.cpp** — psram_alloc<T>::allocate (lines 90-98)
  On ESP32, if both heap_caps_malloc attempts fail it returns nullptr instead of throwing std::bad_alloc; the C++ Allocator contract requires a throw, so std::vector will treat nullptr as valid storage and write through it (UB) on OOM instead of failing cleanly.

- **screens/qr_display_screen.cpp** — qr_display_screen() lines 457-466 (ctx, ctx->tmp, ctx->out)
  Three lv_malloc results are used unchecked (lv_memzero on ctx, qrcodegen writes into tmp/out); on ESP32-pool exhaustion this is a NULL-deref crash. Sibling seed_transcribe_whole_qr_screen NULL-checks the same allocations and throws — inconsistent hardening for the same risk.

- **screens/seed_transcribe_whole_qr_screen.cpp** — seed_transcribe_whole_qr_screen() line 343
  cfg 'border' is accepted unvalidated (qr_display clamps to 0..20); a negative value makes cell_off map modules outside the object area — rendering is clipped (not memory-unsafe) but the QR silently draws wrong.

- **screens/seed_transcribe_zoomed_qr_screen.cpp** — seed_transcribe_zoomed_qr_screen() lines 449-453 (ctx/tmp/out lv_malloc)
  Same unchecked-lv_malloc pattern as qr_display: on ESP32 pool exhaustion, lv_memzero(NULL) or encoding into a NULL buffer crashes; sibling whole_qr checks and throws.

- **screens/seed_transcribe_zoomed_qr_screen.cpp** — ctx->tmp lifetime (allocated line 452, freed only in zoomqr_cleanup_cb line 368)
  The qrcodegen scratch buffer (~qrcodegen_BUFFER_LEN_MAX bytes) is held for the entire screen lifetime although the encode is one-shot at build time; on the RAM-constrained ESP32 LVGL pool this is avoidable retention (whole_qr frees its scratch immediately after encoding).

- **screens/seed_transcribe_zoomed_qr_screen.cpp** — zoomqr_is_registration() lines 144-152
  Alignment-pattern position is hard-coded for sizes 25/29 only, but the screen accepts arbitrary payloads/modes (auto/byte can produce larger versions); for any QR above 29 modules the alignment blocks silently render as data dots and multi-alignment versions are ignored. Fine for the SeedQR domain (21/25/29) but nothing rejects out-of-domain sizes.

- **screens/keyboard_screen.cpp** — keyboard_screen() lines 218 vs 228-233
  Error-path leak: ctx is `new`ed (218) and the scaffold screen tree is created (210) before the required-"keys" validation throws (229/233); neither is freed on throw. seed_mnemonic_entry deletes its ctx before throwing (but also leaks the scaffold); seed_add_passphrase validates before allocating (no leak) — three different error-path orderings, two of which leak.

- **screens/keyboard_screen.cpp** — keyboard_update_title line 122 and keyboard_value_changed_cb line 169
  Entered-length counted with std::strlen (bytes, not UTF-8 chars). If a caller supplies multi-byte key labels WITHOUT keys_to_values (the documented 'absent => the label IS the value' path), each press adds 3 bytes, so return_after_n_chars fires early and the {n} counter jumps by 3. Current consumers (dice/coin/derivation) map to ASCII, so latent.

- **screens/keyboard_screen.cpp** — Banner cfg contract, lines 57-71
  Contract comment omits two live config keys: show_cursor_keys (default true, line 248) and guidance_text (line 314). An AI author regenerating a scenario from the contract would silently get cursor keys it may not want and lose the legend.

- **screens/seed_add_passphrase_screen.cpp** — passphrase_kb_key_cb lines 414-427
  Aux keys matched against ASCII '1'/'2'/'3' only, while navigation.cpp/is_aux_key and seed_mnemonic_entry also accept LV_KEY_F1..F3 (currently defined nowhere, so dead). If a platform indev ever delivers F1-F3 codes (the mnemonic comment claims 'on-device' does), the passphrase side panel silently stops responding while button_list and mnemonic screens keep working. Divergence risk, not a today-bug.

- **screens/seed_mnemonic_entry_screen.cpp** — Entry function lines 536 + 550-553
  Scaffold screen tree is created before the wordlist validation throws; the ctx is deleted but the never-loaded lv screen object leaks (LVGL objects aren't freed by unwinding).

- **screens/seed_mnemonic_entry_screen.cpp** — mnemonic_kb_preprocess_cb (no LEFT/RIGHT branch)
  Joystick LEFT/RIGHT on the 5x6 grid uses LVGL's default linear cross-row move instead of the row-wrap every other keyboard screen applies via kb_handle_directional — and RIGHT from DEL can walk onto the HIDDEN filler keys appended at lines 628-631 (LVGL skips hidden keys for indev moves in most paths, but the divergence is undocumented). If intentional Python parity, it needs a comment; if not, it is a behavior gap.

- **screens/tools_calc_final_word_screen.cpp** — Lines 196-218 bit decomposition
  num_keeper = 11 - checksum_bits.size() goes negative for a malformed host payload with >11 checksum bits; std::string(num_keeper, '_') then converts to a huge size_t and throws std::length_error (or aborts on small heaps) instead of the screen's own descriptive runtime_error — the validation at line 191 checks emptiness but not length sanity.

- **screens/seed_words_screen.cpp** — line 65, start_number default: `page_index * (int)words.size() + 1`
  words.size() is the count on THIS page, not Python's constant words_per_page; on a partial final page (e.g. page_index 3 with 2 words when earlier pages held 4) the default numbers the words 7-8 instead of 13-14. Correct only when the host always passes start_number explicitly or pages are always full.

- **screens/seed_review_passphrase_screen.cpp** — doc comment lines 180-181 vs code lines 193-194
  fingerprint_without / fingerprint_with are documented as required ('str,req') but are read with silent "" defaults — a bare cfg renders a meaningless ' >> ' fingerprint line instead of throwing like the passphrase key does; comment and behavior disagree.

- **screens/seed_export_xpub_details_screen.cpp** — line 64: `derivation_path = cfg.value("derivation_path", std::string("m/84'/0'/0'"))`
  A security-display screen silently fabricates a plausible derivation path when the host omits the key, while its sibling keys (fingerprint, xpub) throw. A host bug would show the user a confident, wrong derivation instead of failing loudly — inconsistent required/optional policy on security-relevant data.

- **screens/seed_export_xpub_details_screen.cpp** — lines 126-129: truncation always appends "..."
  Even when num_chars is clamped to xpub.size() (the whole xpub fits), "..." is still appended, showing an ellipsis after a complete xpub. Likely inherited from Python's unconditional f"{xpub[:num_chars]}...", so it may be intentional parity — flagging for confirmation only.

- **screens/seed_address_verification_screen.cpp** — banner cfg contract block, lines 46-53
  The contract table omits two keys the code actually reads: cfg["network"] (line 126) and cfg["progress_text"] (line 207). An AI author extending the screen (or a host developer) reading only the contract block will miss both.

- **screens/seed_sign_message_confirm_address_screen.cpp** — lines 117-123 (formatted_address accent hard-defaulted to mainnet)
  The address head/tail highlight is always mainnet orange: no cfg["network"] read and no resolve_address_network() call, so a testnet/regtest address in the sign-message flow renders with the mainnet accent while the PSBT screens (and seed_address_verification) color the same address green/cyan. If Python's SeedSignMessageConfirmAddressScreen inherits the network-aware coloring, this is a parity gap; even if Python hardcodes accent, it is a cross-screen behavioral inconsistency within this repo for identical data.

- **screens/seed_sign_message_confirm_message_screen.cpp** — lines 47-52 (`cfg["text"] = message;` and its justifying comment)
  The assignment is dead config mutation: the scaffold consults cfg["text"] (has_intro_text, screen_scaffold.cpp:393) ONLY in the `!is_bottom_list && !has_intro_text` Mode-2 branch, and this screen forces is_bottom_list=true two lines earlier, which alone guarantees the separate upper_body. The comment claims the assignment is what 'takes that path'. Risk: a maintainer trusts the comment and drops the is_bottom_list default, or a future scaffold change starts rendering cfg["text"] itself and this screen double-renders the message.

- **screens/seed_sign_message_confirm_message_screen.cpp** — line 40 vs banner line 16 ('message (str, req.)')
  The documented-required `message` is read via cfg.value with a silent empty default and never validated, unlike every required field in the sibling screens (which throw with a screen-named error). A host omitting the key gets a blank review screen whose Next button signs an unreviewed message — the contract says the library should have rejected the cfg.

- **screens/multisig_wallet_descriptor_screen.cpp** — line 90, `leading_residual = (3 * COMPONENT_PADDING) / 8`
  The comment claims the 3px @240 trim 'scales w/ profile', but integer division makes it plateau: at the 133x profile COMPONENT_PADDING is ~10, so 30/8 = 3 — the same 3px as 240, not the ~4px linear scaling implies — while at 200x (16) it correctly doubles to 6. The 320-tall profile's inter-component gap is therefore ~1px looser than the stated intent. Pixel-parity is only gated at 240 so this is cosmetic, but the comment misdescribes the arithmetic.

- **screens/multisig_wallet_descriptor_screen.cpp** — lines 52-65 (all-optional parsing)
  No required-field enforcement at all: a cfg with neither signing_keys nor fingerprints (or empty policy) renders a blank-valued confirmation screen instead of throwing, diverging from the throw-on-required policy of the sibling screens and hiding host wiring mistakes on a security-relevant confirmation. The banner's cfg table doesn't mark any field required, so it is at least self-consistent.

- **screens/camera_entropy_overlay_screen.cpp** — Banner doc block (lines 49-57) vs parsing code (lines 124, 136-140)
  Doc/code drift: the 'JSON config (all optional)' key list omits `capture_icon` and `capture_style`, both of which the function parses and forwards into the spec — an AI author extending this screen from the doc block would miss two live config keys

- **screens/tools_address_explorer_address_list_screen.cpp** — cfg["is_bottom_list"] = false (line 183) vs Python is_bottom_list = True
  The equivalence argument holds only when the page overflows; a short final page (host pages a remainder of 1-3 addresses) would render top-aligned under the nav where Python bottom-pins it. Nothing enforces the 'always overflows' assumption the comment relies on

- **screens/tools_address_explorer_address_list_screen.cpp** — Missing required-field validation for cfg["addresses"] (lines 106-111)
  Doc block marks addresses '(required)' but absent/malformed input silently renders an empty list whose only button is 'Next 0' — diverges from the corpus `throw std::runtime_error("<screen> requires ...")` idiom and masks host bugs

- **screen_helpers.cpp** — Lines 52-60, orphaned comment block above apply_rtl_text_to_labels
  Stale documentation: an 8-line rationale block for 'Switch to a newly built LVGL screen and dispose of the old one' (load_screen_and_cleanup_previous) sits directly above apply_rtl_text_to_labels; the function it documents moved to screen_scaffold.cpp during the split. An AI editor pattern-matching comment-to-function here would mis-attribute teardown semantics to the RTL pass.

- **screen_helpers.h** — Line 58-59: comment on parse_hex_color: '"#rrggbb" (or 0xAARRGGBB-ish) -> packed color.'
  Contract comment contradicts the implementation: parse_hex_color throws on anything but exactly 6 hex digits after stripping #/0x — no 8-digit AARRGGBB form is accepted. An AI author trusting this header would emit an alpha-carrying color string and get a runtime throw.

- **components.cpp** — large_icon_button, line 1234: lv_obj_t* text_label = lv_obj_get_child(lv_button, 0);
  Positional child lookup for the text label in the one file that built a tag system (BUTTON_TEXT_LABEL_TAG / find_button_text_label) precisely because positional lookups break when icon siblings exist. Correct today only because it runs before the icon label is created; any reorder inside button() (e.g. adding a leading element) silently grabs the wrong child and restyles it with LARGE_BUTTON_FONT.

- **components.cpp** — label_set_line_autoscroll, lines 294-299: static lv_anim_t scroll_feel_template shared across all autoscrolling labels
  lv_obj_set_style_anim stores a pointer and reads the template lazily at deferred label refresh; two same-frame callers with DIFFERENT hold values would cross-contaminate. The code documents exactly why it is safe today and what a future caller must do — flagged so the cross-comparison stage knows this invariant is load-bearing and unenforced (no assert).

- **gui_constants.cpp** — fonts_for_multiplier icon_large slot across profiles (lines 48, 66, 83) + make_profile icon_large_button_size (line 117)
  Non-geometric icon_large scaling: 240-height uses the 48px bake (seedsigner_icons_48_4bpp), 320 uses 36×1.333=48px, 480 uses 36×2=72px — so 240→320 doesn't scale (48→48) and 240→480 scales ×1.5 not ×2 (48→72), while the profile field icon_large_button_size = px_scale(36) says 36 at 240, disagreeing with the 48px font actually installed. The unused-at-240 seedsigner_icons_36_4bpp is still declared/baked. May be a deliberate hand-tune ('icon_large unchanged' comment at 200x), but the constants and fonts tell contradictory stories.

- **navigation.cpp** — line 7 (weak seedsigner_lvgl_on_aux_key) + seedsigner.h
  seedsigner_lvgl_on_aux_key is never declared in seedsigner.h (only mentioned in comments there), unlike the other three host callbacks. io_test_screen.cpp:47 re-declares it locally with extern \"C\". A host or screen with a drifted signature would not be caught by the compiler, and hosts reading seedsigner.h cannot discover the hook.

- **navigation.cpp** — nav_bind() line 412
  ctx->top_item = back ?: power assumes back and power buttons are mutually exclusive but nothing enforces it; a cfg with both show_back_button and show_power_button true renders both, yet joystick focus can only ever reach the back button — the power button becomes hardware-unreachable.

- **navigation.cpp** — nav_cleanup_handler() comment, line 386
  Comment justifies lv_group_del by LVGL 8 behavior ('clears the indev's group pointer in LVGL 8') but the repo pins LVGL v9.5 and lv_group_del is only a v8 compat alias (lv_api_map_v8.h:164). The claim should be re-anchored to v9 semantics; if v9's lv_group_delete did not clear indev pointers the cleanup would leave a dangling group on every screen swap.

- **keyboard_core.cpp** — kb_side_button(), line 282
  Reads text[3] after only confirming text[0]==0xEE; a malformed/truncated 1- or 2-byte string starting 0xEE would index past the NUL terminator (and kb_icon_draw_cb similarly reads txt[1]/txt[2] unguarded). Inputs are internal glyph constants today, so this is latent, not live.

- **keyboard_core.cpp** — kb_icon_draw_cb(), line 171
  Detects 'key is selected/pressed' by comparing the incoming draw color against BUTTON_SELECTED_FONT_COLOR (black) — a palette-coupled heuristic: any future theme where a resting key's text color equals the selected color silently disables the control-glyph recolor.

- **overlay_manager.cpp** — current_keypad_group(), line 37
  Saves only the FIRST keypad/encoder indev's group but set_keypad_group() restores to ALL such indevs — correct only under the de facto invariant that all keypad indevs share one group; a host with heterogeneous groups would get the second indev's group clobbered on dismiss.

- **glyph_runs.cpp** — metrics_for (lines 159-166)
  The stb metrics cache is keyed on the byte POINTER only (g_metrics_bytes == bytes), with no length check. If a font buffer is freed and a different buffer is later allocated at the same address without an intervening seedsigner_clear_glyph_runs (the current call ordering happens to always clear in between, but nothing enforces it), the stale handle would be reused against different bytes. Fragile invariant enforced only by call-order convention.

- **locale_loader.cpp** — ss_register_pack_manifest, chain_role parse (lines 148-149)
  Unknown "chain" values fail OPEN to Primary ((value == "fallback") ? Fallback : Primary) while every other field in this deliberately fail-closed function rejects malformed input — a typo'd or future chain value in a user-supplied SD manifest silently becomes a CJK-style Primary chain with bumped sizes instead of being rejected.

- **locale_loader.cpp** — ss_load_locale, g_owned.reserve(4) (line 75)
  The comment claims reserve() is what 'keep[s] .data() pointers stable across the registrations below', but inner-vector data pointers survive outer reallocation anyway (vector move semantics); reserve(4) neither guarantees stability beyond 4 files nor is it needed for it. A misleading load-bearing-looking invariant comment in a repo where comments are the primary reviewer surface.

- **locale_picker.cpp** — locale_picker.h banner + locale_picker_attach_endonym doc comment (header lines 16-17, 48-49) vs implementation
  The header documents that attaching an endonym will 'suppress the button's live text label' and paint the image over the row — but the implementation (and the consuming screen) deliberately KEEPS the live English text and draws the image AFTER it ('English | <native>' rows; .cpp comment line 137 says 'The label keeps its live English-name text'). Stale design-generation doc: an agent implementing against the header contract would suppress the label and regress the rendered rows.

- **locale_picker.h** — Banner lines 16-17 and locale_picker_attach_endonym comment (line 48)
  Header says the attach 'suppress[es] the button's live text label' — the implementation keeps the live label and paints the image after it. Same stale-doc defect flagged on the .cpp; recorded on both since either file alone would mislead.

- **font_registry.cpp** — seedsigner_register_font, Fallback branch (lines 197-200)
  heap_copy = new lv_font_t(*original) then heap_copy->fallback = script unconditionally overwrites whatever fallback the copied baseline font carried. Today the tiny_ttf baseline fonts have no fallback so nothing is lost, but if install_western_baseline ever chains one (e.g. an icon or symbol fallback), a Fallback-pack locale would silently sever it. Preserving it (script->fallback = original->fallback) costs nothing; the current code encodes an unstated no-baseline-fallback invariant.

- **font_registry.cpp** — seedsigner_register_font re-registration of an already-registered role
  If a host registers the same role twice within one locale (no guard prevents it), the second Registration captures `original` = the FIRST registration's installed script/heap_copy (the field was already mutated). clear() then restores the field to a font that is simultaneously being retired, leaving the profile pointing at a font the next reap destroys — a use-after-free armed entirely by host call order. A cheap same-role-already-registered rejection (or restore-in-reverse-order) would close it.

- **qr_core.cpp** — qr_decode_payload, lines 151 and 156
  Both exception messages hardcode the prefix 'qr_display_screen:' but the function is shared — a malformed payload from seed_transcribe_zoomed_qr_screen reports itself as a qr_display_screen error, misdirecting host-side debugging.

- **qr_core.cpp** — qr_base64_decode, lines 31-51
  Silently skips EVERY invalid character (comment claims only whitespace/newlines) and ignores truncated final groups, so malformed base64 yields silently-wrong bytes instead of an error — asymmetric with the hex path, which throws on any defect. For a QR whose payload the user may transcribe or scan, silent corruption is worse than a thrown error.

- **qr_core.cpp** — qr_encode_bytes, lines 192-204
  In the explicit NUMERIC/ALNUM modes, s.c_str() truncates at an embedded NUL: qrcodegen_isNumeric/isAlphanumeric validate only the pre-NUL prefix and qrcodegen_makeNumeric/makeAlphanumeric then encode ONLY that prefix — silent payload truncation. The AUTO path guards with the 'clean' no-NUL check (line 184) but the explicit-mode path re-derives use_numeric/use_alnum without it.

- **camera_preview_overlay.cpp** — camera_preview_overlay_create, lines 103-105
  lv_malloc result is used (lv_memzero, member writes) without a NULL check; on an OOM-prone ESP32 heap this is a straight null-deref instead of a graceful NULL return, even though the function already has a NULL-returning guard path for bad arguments.

- **camera_preview_overlay.cpp** — struct camera_preview_overlay, line 22 comment
  'Exactly one is non-NULL per mode' is false: hardware mode sets two (instr_shadow AND instr_label), or zero when instructions_text is NULL/empty. Misleads a maintainer reasoning about which affordance widgets exist.

- **camera_entropy_overlay.cpp** — Line 20 (weak declaration) + action_clicked_cb line 92
  seedsigner_lvgl_on_button_selected is re-declared locally as extern "C" weak and then called unguarded: in any build that links this TU without components.cpp (which provides the weak default definition), the weak DECLARATION resolves to address 0 and a shutter/accept tap is a null-pointer call — a silent link-time trap rather than a link error. All current in-repo builds link components.cpp, so this is latent; a null check (as the weak-declaration idiom normally demands) or including seedsigner.h's declaration would remove the trap.

- **camera_entropy_overlay.cpp** — camera_entropy_overlay_create, lines 157-159
  lv_malloc result used without a NULL check (lv_memzero + member writes) — same OOM null-deref latent bug as the sibling overlay.

- **camera_entropy_overlay.cpp** — Banner comment, line 13
  Claims the geometry 'holds across 240/320/480/800' — 800 is not a supported display-height profile (gui_constants.h supports 240/320/480; 800 is the 4.3" panel's WIDTH). The sibling banner correctly says 240/320/480. Copy-edit drift that could mislead an AI author into believing a 800-height profile exists.

- **camera_entropy_overlay.h** — Banner, line 18
  'models the image-entropy flow's TWO phases' immediately precedes a three-item phase list (PREVIEW/CAPTURING/CONFIRM) and a three-value enum; the count is stale/wrong.


---

## Rollout completion addendum (2026-07-10, same day)

The full rollout ran later the same day with user approval: **all 36 screens are
now conformed** (5 pilots + waves of 11, 8, and 12) plus a corpus-wide banner
normalization sweep, on the same branch.

- **Wave 1** (508575e): power_off_not_required, donate, reset,
  psbt_address_details, psbt_op_return, psbt_math, seed_finalize,
  seed_export_xpub_details, seed_words, seed_review_passphrase,
  multisig_wallet_descriptor.
- **Wave 2** (5e98083): seed_address_verification, both sign-message screens,
  tools_address_explorer_address_list, settings_locale_picker,
  settings_qr_confirmation, tools_calc_final_word, io_test. (psbt_overview's
  agent was stopped pre-edit at a user pause and re-ran in wave 3;
  tools_calc_final_word's agent was stopped post-edit pre-report — file is
  gate-verified.)
- **Wave 3** (46d37f3): keyboard, seed_add_passphrase, seed_mnemonic_entry,
  qr_display, both transcribe screens, psbt_overview on Fable agents;
  screensaver, opening_splash, main_menu, both camera overlay wrappers on
  Opus agents (hybrid model split — all five Opus reports met wave standard).
- **Normalization sweep** (c28a984): spec §3 register rulings (layer-marked
  universal cfg keys on scaffold tier, omitted on chrome-free tier; mandatory
  lifecycle-tier line) applied comment-only across 20 files.

**Verification:** every wave gated at zero diffs; 9 gate runs total across the
whole effort, all clean (3,180 images each against the pre-change baseline).

**Content-policy retentions (kept-and-flagged, scenario- or contract-driven):**
psbt_op_return `hex_label` (raw_hex variation omits it), settings_qr_confirmation
`["Home"]` chrome default (no scenario supplies it), psbt_overview's 10 chart
label templates (no scenario supplies `labels`), camera_preview
`instructions_text` (module NULL-contract + touch mode), seed_address_verification
`progress_text` (blank-until-host-push contract), io_test `camera_glyph`
(icon-font codepoint, structural). Boot tier (main_menu, opening_splash) keeps
English defaults by the documented §5 exception.

**Bug-ledger notes from the rollout:** seed_words' `start_number` flag is
DISPUTED (Python computes `words_per_page` from the rendered page identically);
seed_add_passphrase's aux-key flag was already resolved by the Group B
extraction. New report-only Tier-2 audit findings: unchecked `lv_malloc`
(opening_splash ctx, in addition to the flagged qr_display/zoomed trios),
psbt_overview cleanup callback missing the LV_EVENT_DELETE re-check,
loading_spinner POD ctx on new/delete, and psbt_overview's cfg-`animate` vs
`seedsigner_lvgl_is_static_render()` dual mechanism for stills.

**Cost (measured, whole effort):** pilot phase ~4.1M subagent tokens; rollout
waves + sweep ~4.3M (wave 1 ≈ 1.2M, wave 2 ≈ 1.0M, wave 3 ≈ 2.0M, sweep ≈ 0.2M);
total ≈ 8.4M subagent tokens plus main-session orchestration.

**Remaining (unchanged from the deferred-by-design list):** DOCUMENTED
extraction clusters #10-#16, §11 pixel-policy decisions, §12 consolidations,
status-family content defaults, and the flagged-bug ledger adjudication.
