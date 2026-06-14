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
    "fonts/seedsigner_icons_24_4bpp_150x.c"
    "fonts/seedsigner_icons_36_4bpp_150x.c"
    "fonts/seedsigner_icons_48_4bpp_150x.c"
    "fonts/seedsigner_icons_24_4bpp_200x.c"
    "fonts/seedsigner_icons_36_4bpp_200x.c"
    "fonts/seedsigner_icons_48_4bpp_200x.c"
    "fonts/inconsolata_semibold_24_4bpp.c"
    "fonts/inconsolata_semibold_24_4bpp_150x.c"
    "fonts/inconsolata_semibold_24_4bpp_200x.c"
    "gui_constants.cpp"
    "seedsigner.cpp"
    "components.cpp"
    "input_profile.cpp"
    "navigation.cpp"
    "images/seedsigner_logo_img.c"
)


idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${component_requires}
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)
