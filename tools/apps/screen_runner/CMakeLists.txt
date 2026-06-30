cmake_minimum_required(VERSION 3.16)
project(screen_runner LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDL2 discovery strategy:
# 1) CMake package config (preferred)
# 2) CMake FindSDL2 module fallback
find_package(SDL2 CONFIG QUIET)
if(NOT SDL2_FOUND)
  find_package(SDL2 QUIET)
endif()

if(NOT SDL2_FOUND)
  message(FATAL_ERROR
    "SDL2 not found. Install SDL2 dev files and/or provide CMake path hints.\n"
    "Examples:\n"
    "  macOS (Homebrew): brew install sdl2\n"
    "  cmake ... -DCMAKE_PREFIX_PATH=$(brew --prefix sdl2)\n"
    "  or set -DSDL2_DIR=<path containing SDL2Config.cmake>\n"
  )
endif()

# ---------------------------------------------------------------------------
# Required inputs: default display dimensions
# ---------------------------------------------------------------------------
if(NOT DEFINED DISPLAY_WIDTH OR NOT DEFINED DISPLAY_HEIGHT)
    message(FATAL_ERROR
        "DISPLAY_WIDTH and DISPLAY_HEIGHT must be specified.\n"
        "Example: cmake -DDISPLAY_WIDTH=480 -DDISPLAY_HEIGHT=320 ..."
    )
endif()

# Desktop tools support all display heights by default.
# Pass -DSUPPORT_DISPLAY_HEIGHT_240=OFF or -DSUPPORT_DISPLAY_HEIGHT_320=OFF to restrict.
if(NOT DEFINED SUPPORT_DISPLAY_HEIGHT_240)
    set(SUPPORT_DISPLAY_HEIGHT_240 ON)
endif()
if(NOT DEFINED SUPPORT_DISPLAY_HEIGHT_320)
    set(SUPPORT_DISPLAY_HEIGHT_320 ON)
endif()
if(NOT DEFINED SUPPORT_DISPLAY_HEIGHT_480)
    set(SUPPORT_DISPLAY_HEIGHT_480 ON)
endif()

# Repo layout assumptions:
# - this file lives at seedsigner-lvgl-screens/tools/apps/screen_runner
# - LVGL headers are provided via -DLVGL_ROOT=<path> (preferred), or auto-detected below.
set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(SEEDSIGNER_DIR "${PROJECT_ROOT}/components/seedsigner")
set(COMMON_DIR "${PROJECT_ROOT}/tools/apps/runner_core")
set(NLOHMANN_JSON_INCLUDE_DIR "${PROJECT_ROOT}/components/nlohmann_json/include")

if(NOT DEFINED LVGL_ROOT OR LVGL_ROOT STREQUAL "")
    # Preferred: pinned clone in third_party/
    set(LVGL_ROOT "${PROJECT_ROOT}/third_party/lvgl")
endif()

if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
    set(LVGL_ROOT "${PROJECT_ROOT}/../micropython/ports/esp32/managed_components/lvgl__lvgl")
endif()

if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
    set(LVGL_ROOT "${PROJECT_ROOT}/references/micropython/ports/esp32/managed_components/lvgl__lvgl")
endif()

if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
    message(FATAL_ERROR "LVGL root not found. Pass -DLVGL_ROOT=<path to .../managed_components/lvgl__lvgl>")
endif()

if(NOT EXISTS "${NLOHMANN_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
  message(FATAL_ERROR "nlohmann/json.hpp not found at: ${NLOHMANN_JSON_INCLUDE_DIR}")
endif()

file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS
  "${LVGL_ROOT}/src/*.c"
)

# The five translated text roles are rasterized at runtime from the compiled-in
# OpenSans Western TTF (one Regular + one SemiBold subset, all sizes); only the
# baked icon + Inconsolata keyboard fonts are listed per height below.
set(SEEDSIGNER_FONT_SOURCES
  "${SEEDSIGNER_DIR}/fonts/opensans_western_regular.c"
  "${SEEDSIGNER_DIR}/fonts/opensans_western_semibold.c"
)
if(SUPPORT_DISPLAY_HEIGHT_240)
    list(APPEND SEEDSIGNER_FONT_SOURCES
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_48_4bpp.c"
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp.c"
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_26_4bpp.c"
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp.c"
      "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_24_4bpp.c"
      "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_22_4bpp.c"
    )
endif()
if(SUPPORT_DISPLAY_HEIGHT_320)
    list(APPEND SEEDSIGNER_FONT_SOURCES
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp_133x.c"
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_26_4bpp_133x.c"
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp_133x.c"
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_48_4bpp_133x.c"
      "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_24_4bpp_133x.c"
      "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_22_4bpp_133x.c"
    )
endif()
if(SUPPORT_DISPLAY_HEIGHT_480)
    list(APPEND SEEDSIGNER_FONT_SOURCES
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp_200x.c"
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_26_4bpp_200x.c"
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp_200x.c"
      "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_48_4bpp_200x.c"
      "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_24_4bpp_200x.c"
      "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_22_4bpp_200x.c"
    )
endif()

# Baked logo + HRF partner-logo assets, gated per display height (mirrors the
# font blocks above). Runtime selectors pick the px_multiplier-matched variant.
set(SEEDSIGNER_IMAGE_SOURCES "")
if(SUPPORT_DISPLAY_HEIGHT_240)
  list(APPEND SEEDSIGNER_IMAGE_SOURCES
    "${SEEDSIGNER_DIR}/images/seedsigner_logo_img.c"
    "${SEEDSIGNER_DIR}/images/hrf_logo_img.c")
endif()
if(SUPPORT_DISPLAY_HEIGHT_320)
  list(APPEND SEEDSIGNER_IMAGE_SOURCES
    "${SEEDSIGNER_DIR}/images/seedsigner_logo_img_133x.c"
    "${SEEDSIGNER_DIR}/images/hrf_logo_img_133x.c")
endif()
if(SUPPORT_DISPLAY_HEIGHT_480)
  list(APPEND SEEDSIGNER_IMAGE_SOURCES
    "${SEEDSIGNER_DIR}/images/seedsigner_logo_img_200x.c"
    "${SEEDSIGNER_DIR}/images/hrf_logo_img_200x.c")
endif()

set(SEEDSIGNER_CORE_SOURCES
  "${SEEDSIGNER_DIR}/gui_constants.cpp"
  "${SEEDSIGNER_DIR}/locale_fonts.cpp"
  "${SEEDSIGNER_DIR}/font_registry.cpp"
  # i18n locale loader + complex-script glyph-run render layer (Devanagari/Thai/
  # Nastaliq). seedsigner.cpp calls apply_glyph_runs_to_labels()/ss_reap_retired(),
  # so these must be linked here just as in the screenshot_generator/web_runner.
  "${SEEDSIGNER_DIR}/locale_loader.cpp"
  "${SEEDSIGNER_DIR}/glyph_runs.cpp"
  "${SEEDSIGNER_DIR}/stb_glyph_metrics.c"
  "${SEEDSIGNER_DIR}/components.cpp"
  "${SEEDSIGNER_DIR}/camera_preview_overlay.cpp"
  "${SEEDSIGNER_DIR}/keyboard_core.cpp"
  "${SEEDSIGNER_DIR}/input_profile.cpp"
  "${SEEDSIGNER_DIR}/navigation.cpp"
  "${SEEDSIGNER_DIR}/seedsigner.cpp"
  # Shared overlay manager: screensaver idle-watch dispatcher (toast queue later).
  "${SEEDSIGNER_DIR}/overlay_manager.cpp"
)

# Shared SDL/LVGL glue, also used by the WASM web_runner.
set(RUNNER_COMMON_SOURCES
  "${COMMON_DIR}/runner_core.cpp"
  "${COMMON_DIR}/runner_sdl.cpp"
)

set(SCREEN_RUNNER_SOURCE
  "${CMAKE_CURRENT_LIST_DIR}/screen_runner.cpp"
)

add_executable(screen_runner
  ${LVGL_SOURCES}
  ${SEEDSIGNER_FONT_SOURCES}
  ${SEEDSIGNER_IMAGE_SOURCES}
  ${SEEDSIGNER_CORE_SOURCES}
  ${RUNNER_COMMON_SOURCES}
  ${SCREEN_RUNNER_SOURCE}
)

target_compile_definitions(screen_runner PRIVATE
  LV_CONF_SKIP
  LV_USE_DRAW_SW_ASM=LV_DRAW_SW_ASM_NONE
  # The five translated text roles render from the compiled-in OpenSans Western
  # TTF via lv_tiny_ttf (installed in set_display()); per-locale script fonts also
  # load through tiny_ttf. Use the C library allocator rather than LVGL's 64KB
  # builtin pool, which tiny_ttf's stb rasterizer can exhaust/fragment over a long
  # session (see the screenshot generator's note).
  LV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB
  LV_USE_TINY_TTF=1
  # Farsi/Arabic i18n: BIDI = right-to-left reordering of mixed RTL/LTR text;
  # ARABIC_PERSIAN_CHARS = cursive shaping (base letters -> presentation forms).
  # Both must match the font packs, which subset to the forms the shaper emits.
  LV_USE_BIDI=1
  LV_USE_ARABIC_PERSIAN_CHARS=1
  DISPLAY_WIDTH=${DISPLAY_WIDTH}
  DISPLAY_HEIGHT=${DISPLAY_HEIGHT}
  $<$<BOOL:${SUPPORT_DISPLAY_HEIGHT_240}>:SUPPORT_DISPLAY_HEIGHT_240>
  $<$<BOOL:${SUPPORT_DISPLAY_HEIGHT_320}>:SUPPORT_DISPLAY_HEIGHT_320>
  $<$<BOOL:${SUPPORT_DISPLAY_HEIGHT_480}>:SUPPORT_DISPLAY_HEIGHT_480>
)

target_include_directories(screen_runner PRIVATE
  "${LVGL_ROOT}"
  "${SEEDSIGNER_DIR}"
  "${COMMON_DIR}"
  "${NLOHMANN_JSON_INCLUDE_DIR}"
)

if(TARGET SDL2::SDL2)
  if(TARGET SDL2::SDL2main)
    target_link_libraries(screen_runner PRIVATE SDL2::SDL2main)
  endif()
  target_link_libraries(screen_runner PRIVATE SDL2::SDL2)
else()
  target_include_directories(screen_runner PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(screen_runner PRIVATE ${SDL2_LIBRARIES})
endif()

# SDL2_ttf for chrome sidebar text rendering.
# Discovery strategy mirrors SDL2: CONFIG first, then pkg-config.
find_package(SDL2_ttf CONFIG QUIET)
if(NOT SDL2_ttf_FOUND)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(SDL2_TTF QUIET sdl2-ttf SDL2_ttf)
  endif()
endif()

if(SDL2_ttf_FOUND)
  if(TARGET SDL2_ttf::SDL2_ttf)
    target_link_libraries(screen_runner PRIVATE SDL2_ttf::SDL2_ttf)
  else()
    target_include_directories(screen_runner PRIVATE ${SDL2_TTF_INCLUDE_DIRS})
    target_link_libraries(screen_runner PRIVATE ${SDL2_TTF_LIBRARIES})
  endif()
elseif(SDL2_TTF_FOUND)
  target_include_directories(screen_runner PRIVATE ${SDL2_TTF_INCLUDE_DIRS})
  target_link_libraries(screen_runner PRIVATE ${SDL2_TTF_LIBRARIES})
else()
  message(FATAL_ERROR
    "SDL2_ttf not found. Install SDL2_ttf dev files.\n"
    "  macOS (Homebrew): brew install sdl2_ttf\n"
    "  Linux:            apt install libsdl2-ttf-dev\n"
    "  cmake hint:       -DCMAKE_PREFIX_PATH=$(brew --prefix sdl2_ttf)\n"
  )
endif()

# Copy OpenSans fonts next to the binary so the runner can find them at runtime.
set(FONT_REGULAR_SRC  "${SEEDSIGNER_DIR}/assets/OpenSans-Regular.ttf")
set(FONT_SEMIBOLD_SRC "${SEEDSIGNER_DIR}/assets/OpenSans-SemiBold.ttf")
if(EXISTS "${FONT_REGULAR_SRC}")
  add_custom_command(TARGET screen_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FONT_REGULAR_SRC}"
      "$<TARGET_FILE_DIR:screen_runner>/screen_runner_font_regular.ttf"
    COMMENT "Copying OpenSans Regular to build directory"
  )
else()
  message(WARNING "OpenSans-Regular.ttf not found at ${FONT_REGULAR_SRC}. Chrome text will not render.")
endif()
if(EXISTS "${FONT_SEMIBOLD_SRC}")
  add_custom_command(TARGET screen_runner POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${FONT_SEMIBOLD_SRC}"
      "$<TARGET_FILE_DIR:screen_runner>/screen_runner_font_semibold.ttf"
    COMMENT "Copying OpenSans SemiBold to build directory"
  )
else()
  message(WARNING "OpenSans-SemiBold.ttf not found at ${FONT_SEMIBOLD_SRC}. Chrome text will not render.")
endif()

# Resize SeedSigner logo (BMP, flattened against title bar background) next to
# the binary so the runner can load it at runtime via SDL_LoadBMP().
find_program(MAGICK_EXECUTABLE NAMES magick convert)
set(LOGO_SRC "${SEEDSIGNER_DIR}/assets/seedsigner_logo.png")
if(MAGICK_EXECUTABLE AND EXISTS "${LOGO_SRC}")
  add_custom_command(TARGET screen_runner POST_BUILD
    COMMAND ${MAGICK_EXECUTABLE} "${LOGO_SRC}"
      -resize x80
      -define bmp:format=bmp4
      "$<TARGET_FILE_DIR:screen_runner>/screen_runner_logo.bmp"
    COMMENT "Resizing SeedSigner logo for screen runner title bar"
  )
else()
  message(STATUS "Logo not copied: magick not found or logo source missing (${LOGO_SRC}).")
endif()
