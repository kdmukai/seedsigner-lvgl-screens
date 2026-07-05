/*
 * locales.h — master locale policy for the SeedSigner language packs.
 * =========================================================================
 *
 * This is the SINGLE SOURCE OF TRUTH for which non-baked-floor locales exist
 * and how each one's script font relates to the baked-in OpenSans + icon floor.
 * It is an X-macro table so ONE file serves two worlds with no codegen:
 *
 *   • C / C++ (the screens render layer) #includes it after defining SS_LOCALE
 *     to expand the rows into a `LocaleFontEntry` table — type-safe, zero parse.
 *   • Python (the pack builder's `locales_h.py` reader) parses the same
 *     `SS_LOCALE(...)` / `SS_DISPLAY_PROFILE(...)` lines textually.
 *
 * It REPLACES `screenshot_gen --dump-locales`: the builder no longer compiles
 * the render layer to learn locale policy — it reads this file directly.
 *
 * Ownership: this file lives in the `seedsigner-language-packs` repo (the pack
 * producer). The screens repo VENDORS a copy and #includes it; downstream
 * consumers learn per-locale policy from the pack manifests at runtime.
 *
 * A locale absent from this table is covered by the baked English/Latin floor
 * (its `.mo` still ships as a catalog-only pack; its native name is live text).
 * English is the source language and has no pack at all.
 *
 * ---------------------------------------------------------------------------
 * SS_LOCALE(id, source_family, chain, unicode_range, rtl, shaping, script, endonym)
 * ---------------------------------------------------------------------------
 *   id            locale code, e.g. "zh_Hans_CN"
 *   source_family source TTF family (fonts/<family>-Regular.ttf, +-SemiBold for
 *                 the OpenSans Fallback packs), for offline subsetting
 *   chain         ChainRole::Primary   — script font becomes the PRIMARY for each
 *                                         text role at a legibility-bumped size
 *                                         (CJK/Arabic/complex); baked OpenSans
 *                                         stays as its fallback for embedded ASCII
 *                 ChainRole::Fallback  — same-size script (block-range OpenSans
 *                                         subset) chained UNDER the baked baseline
 *   unicode_range Fallback packs: the fixed Unicode block(s) to subset OpenSans by
 *                 (pyftsubset --unicodes form). "" ⇒ corpus-subset (the .po glyphs).
 *   rtl           right-to-left base direction (layout mirrors; LVGL does bidi)
 *   shaping       rendered from OFFLINE-shaped glyph runs (runs.bin) rather than by
 *                 codepoint (Devanagari/Thai/Nastaliq). `fa` is false — it uses
 *                 LVGL's presentation-form path, not glyph runs.
 *   script        ISO-15924 tag for the offline shaper ("Deva"/"Thai"/"Arab");
 *                 only meaningful when shaping is true.
 *   endonym       native language name (its own script), pre-rendered into the
 *                 pack's endonym image so the locale picker needs no runtime font.
 */

#ifdef SS_LOCALE
/*        id            source_family         chain                unicode_range                                       rtl    shaping script  endonym          */
SS_LOCALE("zh_Hans_CN", "NotoSansSC",         ChainRole::Primary,  "",                                                 false, false,  "",     "简体中文")
SS_LOCALE("ja",         "NotoSansJP",         ChainRole::Primary,  "",                                                 false, false,  "",     "日本語")
SS_LOCALE("ko",         "NotoSansKR",         ChainRole::Primary,  "",                                                 false, false,  "",     "한국어")
SS_LOCALE("fa",         "NotoSansAR",         ChainRole::Primary,  "",                                                 true,  false,  "",     "فارسی")
SS_LOCALE("hi",         "NotoSansDevanagari", ChainRole::Primary,  "",                                                 false, true,   "Deva", "हिन्दी")
SS_LOCALE("th",         "NotoSansTH",         ChainRole::Primary,  "",                                                 false, true,   "Thai", "ไทย")
SS_LOCALE("ur",         "NotoNastaliqUrdu",   ChainRole::Primary,  "",                                                 true,  true,   "Arab", "اردو")
SS_LOCALE("el",         "OpenSans",           ChainRole::Fallback, "U+0370-03FF",                                      false, false,  "",     "Ελληνικά")
SS_LOCALE("ru",         "OpenSans",           ChainRole::Fallback, "U+0400-04FF",                                      false, false,  "",     "Русский")
SS_LOCALE("vi",         "OpenSans",           ChainRole::Fallback, "U+01A0-01A1,U+01AF-01B0,U+0300-036F,U+1E00-1EFF",  false, false,  "",     "Tiếng Việt")
#undef SS_LOCALE
#endif /* SS_LOCALE */

/*
 * ---------------------------------------------------------------------------
 * Display profiles — SS_DISPLAY_PROFILE(width, height, px_multiplier)
 * ---------------------------------------------------------------------------
 * The pack builder pre-renders each locale's endonym image at the locale's BUTTON
 * px for every DISTINCT display height (rows dedupe by height). This is the second
 * thing `--dump-locales` used to supply. Mirrors gui_constants.cpp make_profile():
 * height 240 → ×100 (Pi Zero, no scaling), 320 → ×133, 480 → ×200.
 *   endonym_px(locale) = SS_ENDONYM_BUTTON_BASE_{PRIMARY,FALLBACK} * px_multiplier / 100
 */

#ifdef SS_DISPLAY_PROFILE
SS_DISPLAY_PROFILE(240, 240, 100)
SS_DISPLAY_PROFILE(320, 240, 100)
SS_DISPLAY_PROFILE(480, 320, 133)
SS_DISPLAY_PROFILE(800, 480, 200)
#undef SS_DISPLAY_PROFILE
#endif /* SS_DISPLAY_PROFILE */

/*
 * Button-role base px (pre-multiplier) per chain — the size an endonym renders at.
 * Primary packs take the CJK legibility bump (button 18→20); Fallback packs match
 * the baked OpenSans baseline (button 18). Kept here so the builder never re-derives
 * the role-size policy the render layer owns.
 */
#ifndef SS_ENDONYM_BUTTON_BASE_PRIMARY
#define SS_ENDONYM_BUTTON_BASE_PRIMARY  20
#define SS_ENDONYM_BUTTON_BASE_FALLBACK 18
#endif
