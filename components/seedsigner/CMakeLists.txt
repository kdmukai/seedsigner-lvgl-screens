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
    "fonts/inconsolata_semibold_24_4bpp.c"
    "fonts/inconsolata_semibold_24_4bpp_133x.c"
    "fonts/inconsolata_semibold_24_4bpp_200x.c"
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
    "seedsigner.cpp"
    "components.cpp"
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
)


idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${component_requires}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)
