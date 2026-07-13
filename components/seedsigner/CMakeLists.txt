set(srcs "")
set(include_dirs ".")
set(component_requires "lvgl" "nlohmann_json")


list(APPEND srcs
    # Translated text roles: compiled-in OpenSans Western TTF, rasterized at
    # runtime via lv_tiny_ttf and installed by set_display(). One Regular + one
    # SemiBold subset cover every role/size (replaces the baked opensans bitmaps).
    "fonts/opensans_western_regular.c"
    "fonts/opensans_western_semibold.c"
    "fonts/seedsigner_icons_24_4bpp.c"
    "fonts/seedsigner_icons_48_4bpp.c"
    "fonts/seedsigner_icons_36_4bpp.c"
    "fonts/seedsigner_icons_24_4bpp_133x.c"
    "fonts/seedsigner_icons_36_4bpp_133x.c"
    "fonts/seedsigner_icons_48_4bpp_133x.c"
    "fonts/seedsigner_icons_24_4bpp_200x.c"
    "fonts/seedsigner_icons_36_4bpp_200x.c"
    "fonts/seedsigner_icons_48_4bpp_200x.c"
    # top_nav contextual title icon (26px) — one per display-height profile;
    # --gc-sections drops the unused-resolution variants at link.
    "fonts/seedsigner_icons_26_4bpp.c"
    "fonts/seedsigner_icons_26_4bpp_133x.c"
    "fonts/seedsigner_icons_26_4bpp_200x.c"
    "fonts/inconsolata_semibold_24_4bpp.c"
    "fonts/inconsolata_semibold_22_4bpp.c"
    "fonts/inconsolata_semibold_24_4bpp_133x.c"
    "fonts/inconsolata_semibold_22_4bpp_133x.c"
    "fonts/inconsolata_semibold_24_4bpp_200x.c"
    "fonts/inconsolata_semibold_22_4bpp_200x.c"
    "gui_constants.cpp"
    "locale_fonts.cpp"
    "font_registry.cpp"
    # Host-agnostic locale-pack loader (manifest walk -> register fonts + glyph
    # runs, via a per-host byte provider). Shared by every host.
    "locale_loader.cpp"
    # Complex-script glyph-run render layer (Devanagari/Thai/Nastaliq): draws
    # offline-shaped runs by glyph-id. stb_glyph_metrics.c re-inits a metrics-only
    # stb_truetype over the resident subset to recover glyph boxes; it includes
    # LVGL's bundled stb headers (src/libs/tiny_ttf/...), so the lvgl component's
    # root must be on the include path on this target (ESP32 footprint pass).
    "glyph_runs.cpp"
    "stb_glyph_metrics.c"
    # Locale-picker endonym images: parse the pre-rendered A8 endonym blob and paint
    # it (recolored to the row's live text color) over a picker row — the "list every
    # language in its own script with no runtime font" path (locale_picker_screen).
    "locale_picker.cpp"
    # Shared QR encode/decode core + gutter close button, compiled once and used by
    # both chrome-free QR screens (qr_display_screen, seed_transcribe_zoomed_qr_screen).
    "qr_core.cpp"
    # Cross-cutting scaffold + render/layout helper definitions (declarations in
    # screen_scaffold.h / screen_helpers.h).
    "screen_scaffold.cpp"
    "screen_helpers.cpp"
    # Per-screen entry points (screens/*_screen.cpp) are appended below from the
    # shared screen_sources.cmake glob — the single source of truth also used by
    # the four desktop-tool build lists.
    # Shared LVGL keyboard/text-entry mechanics extracted from the passphrase
    # screen: cursor-styled text entry, key theming + icon recolor, 240px side
    # panel, joystick directional nav. Consumed by every keyboard-style screen.
    "keyboard_core.cpp"
    # Shared overlay manager: the screensaver idle-watch dispatcher + the toast
    # queue. Runs on the LVGL loop via an lv_timer, so it links into every host
    # that pumps lv_timer_handler().
    "overlay_manager.cpp"
    # Toast notification banner: portable native LVGL overlay (bottom banner on the
    # display top layer) that composites over the live screen — the native
    # replacement for the Pi Zero's PIL toast (which blacked out the LVGL UI).
    "toast_overlay.cpp"
    "components.cpp"
    # Camera live-preview overlay: portable spec + LVGL-widget renderer for the
    # UI drawn over the camera square (back affordance + animated-QR status bar).
    "camera_preview_overlay.cpp"
    # Image-entropy capture overlay: two-phase (preview/confirm) sibling of the
    # scan overlay — shutter capture + accept, or hardware-mode instruction text.
    "camera_entropy_overlay.cpp"
    # Portable image-entropy display processing (crop-to-fill + luminance contrast
    # stretch) that produces the CONFIRM-phase final image; LVGL-free helper.
    "image_entropy.cpp"
    "input_profile.cpp"
    "navigation.cpp"
    # Baked RGB565 logo + HRF partner-logo assets, one per display-height profile
    # (selected at runtime by px_multiplier). Listed flat here like the fonts
    # above; the firmware build drops the unused-resolution variants at link via
    # --gc-sections (the #ifdef-gated declarations/selector leave them
    # unreferenced). A single-resolution firmware can also gate these via cache
    # vars — coordinated with the builder (see opening-splash plan, Part 2).
    "images/seedsigner_logo_img.c"
    "images/seedsigner_logo_img_133x.c"
    "images/seedsigner_logo_img_200x.c"
    "images/hrf_logo_img.c"
    "images/hrf_logo_img_133x.c"
    "images/hrf_logo_img_200x.c"
    "images/btc_logo_img.c"
    "images/btc_logo_img_133x.c"
    "images/btc_logo_img_200x.c"
)

# Per-screen entry-point TUs (screens/*_screen.cpp), globbed from the one shared
# list so this stays in sync with the four desktop-tool build lists.
include("${CMAKE_CURRENT_LIST_DIR}/screen_sources.cmake")
list(APPEND srcs ${SEEDSIGNER_SCREEN_SOURCES})


idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${component_requires}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)
