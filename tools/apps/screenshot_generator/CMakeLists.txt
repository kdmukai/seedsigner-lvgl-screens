cmake_minimum_required(VERSION 3.16)
project(screenshot_generator LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PNG REQUIRED)

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
# - this file lives at seedsigner-lvgl-screens/tools/apps/screenshot_generator
# - LVGL headers are provided via -DLVGL_ROOT=<path> (preferred), or auto-detected below.
set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(SEEDSIGNER_DIR "${PROJECT_ROOT}/components/seedsigner")
set(NLOHMANN_JSON_INCLUDE_DIR "${PROJECT_ROOT}/components/nlohmann_json/include")

if(NOT DEFINED LVGL_ROOT OR LVGL_ROOT STREQUAL "")
    # Preferred: pinned clone in third_party/
    set(LVGL_ROOT "${PROJECT_ROOT}/third_party/lvgl")
endif()

if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
    # Builder-local fallback
    set(LVGL_ROOT "${PROJECT_ROOT}/../micropython/ports/esp32/managed_components/lvgl__lvgl")
endif()

if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
    # Legacy fallback (older layout)
    set(LVGL_ROOT "${PROJECT_ROOT}/references/micropython/ports/esp32/managed_components/lvgl__lvgl")
endif()

if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
    message(FATAL_ERROR "LVGL root not found. Pass -DLVGL_ROOT=<path to .../managed_components/lvgl__lvgl>")
endif()

if(NOT EXISTS "${NLOHMANN_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
  message(FATAL_ERROR "nlohmann/json.hpp not found at: ${NLOHMANN_JSON_INCLUDE_DIR}")
endif()

# LVGL source tree
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS
  "${LVGL_ROOT}/src/*.c"
)

# SeedSigner font sources — include fonts for all supported display heights
# The five translated text roles are rasterized at runtime from the compiled-in
# OpenSans Western TTF (one Regular + one SemiBold subset, all sizes), so only the
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
# font blocks above). The runtime selectors (seedsigner_logo_for_active_profile)
# pick the variant matching the active profile's px_multiplier.
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
  "${SEEDSIGNER_DIR}/locale_loader.cpp"
  "${SEEDSIGNER_DIR}/glyph_runs.cpp"
  "${SEEDSIGNER_DIR}/stb_glyph_metrics.c"
  "${SEEDSIGNER_DIR}/components.cpp"
  "${SEEDSIGNER_DIR}/camera_preview_overlay.cpp"
  "${SEEDSIGNER_DIR}/keyboard_core.cpp"
  "${SEEDSIGNER_DIR}/input_profile.cpp"
  "${SEEDSIGNER_DIR}/navigation.cpp"
  "${SEEDSIGNER_DIR}/seedsigner.cpp"
)

set(SCREENSHOT_GEN_SOURCE
  "${CMAKE_CURRENT_LIST_DIR}/screenshot_gen.cpp"
  # THROWAWAY offline-HarfBuzz shaping spike (--shape-spike). shape_spike.cpp
  # renders pre-shaped glyph runs by glyph-id; it reuses the production
  # stb_glyph_metrics (promoted to components/seedsigner/) for glyph boxes.
  "${CMAKE_CURRENT_LIST_DIR}/shape_spike.cpp"
)

add_executable(screenshot_gen
  ${LVGL_SOURCES}
  ${SEEDSIGNER_FONT_SOURCES}
  ${SEEDSIGNER_IMAGE_SOURCES}
  ${SEEDSIGNER_CORE_SOURCES}
  ${SCREENSHOT_GEN_SOURCE}
)

target_compile_definitions(screenshot_gen PRIVATE
  LV_CONF_SKIP
  LV_USE_DRAW_SW_ASM=LV_DRAW_SW_ASM_NONE
  # Use the C library allocator instead of LVGL's built-in fixed pool. With
  # LV_CONF_SKIP the default is LV_STDLIB_BUILTIN with a 64KB LV_MEM_SIZE, and
  # tiny_ttf's stb rasterizer allocates from that same tiny pool. Rendering the
  # whole scenario gallery in one process (many screens back-to-back, plus CJK
  # glyph rasterization) fragments/exhausts 64KB and the allocation-failure path
  # spins — locales that sit near the edge (e.g. ja/ko) hang mid-run. This is a
  # desktop tool on a machine with gigabytes of RAM, so use real malloc/free.
  LV_USE_STDLIB_MALLOC=LV_STDLIB_CLIB
  # Runtime font loading for the registration seam: per-locale subset TTFs are
  # loaded from a buffer via lv_tiny_ttf_create_data() (no filesystem needed).
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

target_include_directories(screenshot_gen PRIVATE
  "${LVGL_ROOT}"
  "${SEEDSIGNER_DIR}"
  "${NLOHMANN_JSON_INCLUDE_DIR}"
)


target_link_libraries(screenshot_gen PRIVATE PNG::PNG)
