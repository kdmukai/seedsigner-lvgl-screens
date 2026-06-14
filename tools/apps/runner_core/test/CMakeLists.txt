cmake_minimum_required(VERSION 3.16)
project(test_runner_core LANGUAGES C CXX)

# Headless test for tools/apps/runner_core — no SDL, no window. Builds LVGL +
# the seedsigner screens + runner_core and renders into the RGB565 framebuffer.
#
#   cmake -B build -DDISPLAY_WIDTH=240 -DDISPLAY_HEIGHT=240 tools/apps/runner_core/test
#   cmake --build build && ./build/test_runner_core tools/scenarios/scenarios.json

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED DISPLAY_WIDTH OR NOT DEFINED DISPLAY_HEIGHT)
  set(DISPLAY_WIDTH 240)
  set(DISPLAY_HEIGHT 240)
endif()
foreach(flag SUPPORT_DISPLAY_HEIGHT_240 SUPPORT_DISPLAY_HEIGHT_320 SUPPORT_DISPLAY_HEIGHT_480)
  if(NOT DEFINED ${flag})
    set(${flag} ON)
  endif()
endforeach()

set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(SEEDSIGNER_DIR "${PROJECT_ROOT}/components/seedsigner")
set(COMMON_DIR "${PROJECT_ROOT}/tools/apps/runner_core")
set(NLOHMANN_JSON_INCLUDE_DIR "${PROJECT_ROOT}/components/nlohmann_json/include")
set(LVGL_ROOT "${PROJECT_ROOT}/third_party/lvgl")

file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS "${LVGL_ROOT}/src/*.c")

set(SEEDSIGNER_FONT_SOURCES "")
if(SUPPORT_DISPLAY_HEIGHT_240)
  list(APPEND SEEDSIGNER_FONT_SOURCES
    "${SEEDSIGNER_DIR}/fonts/opensans_semibold_26_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/opensans_semibold_20_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/opensans_semibold_18_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/opensans_regular_17_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_48_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_24_4bpp.c")
endif()
if(SUPPORT_DISPLAY_HEIGHT_320)
  list(APPEND SEEDSIGNER_FONT_SOURCES
    "${SEEDSIGNER_DIR}/fonts/opensans_semibold_26_4bpp_150x.c"
    "${SEEDSIGNER_DIR}/fonts/opensans_semibold_20_4bpp_150x.c"
    "${SEEDSIGNER_DIR}/fonts/opensans_semibold_18_4bpp_150x.c"
    "${SEEDSIGNER_DIR}/fonts/opensans_regular_17_4bpp_150x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp_150x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp_150x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_48_4bpp_150x.c"
    "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_24_4bpp_150x.c")
endif()
if(SUPPORT_DISPLAY_HEIGHT_480)
  list(APPEND SEEDSIGNER_FONT_SOURCES
    "${SEEDSIGNER_DIR}/fonts/opensans_semibold_26_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/opensans_semibold_20_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/opensans_semibold_18_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/opensans_regular_17_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_48_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_24_4bpp_200x.c")
endif()

add_executable(test_runner_core
  ${LVGL_SOURCES}
  ${SEEDSIGNER_FONT_SOURCES}
  "${SEEDSIGNER_DIR}/gui_constants.cpp"
  "${SEEDSIGNER_DIR}/components.cpp"
  "${SEEDSIGNER_DIR}/input_profile.cpp"
  "${SEEDSIGNER_DIR}/navigation.cpp"
  "${SEEDSIGNER_DIR}/seedsigner.cpp"
  "${SEEDSIGNER_DIR}/images/seedsigner_logo_img.c"
  "${COMMON_DIR}/runner_core.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/test_runner_core.cpp")

target_compile_definitions(test_runner_core PRIVATE
  LV_CONF_SKIP
  LV_USE_DRAW_SW_ASM=LV_DRAW_SW_ASM_NONE
  DISPLAY_WIDTH=${DISPLAY_WIDTH}
  DISPLAY_HEIGHT=${DISPLAY_HEIGHT}
  $<$<BOOL:${SUPPORT_DISPLAY_HEIGHT_240}>:SUPPORT_DISPLAY_HEIGHT_240>
  $<$<BOOL:${SUPPORT_DISPLAY_HEIGHT_320}>:SUPPORT_DISPLAY_HEIGHT_320>
  $<$<BOOL:${SUPPORT_DISPLAY_HEIGHT_480}>:SUPPORT_DISPLAY_HEIGHT_480>)

target_include_directories(test_runner_core PRIVATE
  "${LVGL_ROOT}" "${SEEDSIGNER_DIR}" "${COMMON_DIR}" "${NLOHMANN_JSON_INCLUDE_DIR}")
