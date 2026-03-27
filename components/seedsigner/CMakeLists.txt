set(srcs "")
set(include_dirs ".")
set(component_requires "lvgl" "nlohmann_json")


list(APPEND srcs
    "fonts/opensans_regular_17_4bpp.c"
    "fonts/opensans_semibold_18_4bpp.c"
    "fonts/opensans_semibold_20_4bpp.c"
    "fonts/seedsigner_icons_24_4bpp.c"
    "fonts/opensans_regular_17_4bpp_150x.c"
    "fonts/opensans_semibold_18_4bpp_150x.c"
    "fonts/opensans_semibold_20_4bpp_150x.c"
    "fonts/seedsigner_icons_24_4bpp_150x.c"
    "fonts/seedsigner_icons_36_4bpp.c"
    "fonts/seedsigner_icons_36_4bpp_150x.c"
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
