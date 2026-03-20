cmake_minimum_required(VERSION 3.16)
project(screenshot_generator LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PNG REQUIRED)


# Repo layout assumptions:
# - this file lives at seedsigner-c-modules/tools/screenshot_generator
# - LVGL headers are provided via -DLVGL_ROOT=<path> (preferred), or auto-detected below.
set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(SEEDSIGNER_DIR "${CMAKE_CURRENT_LIST_DIR}/../../components/seedsigner")
set(NLOHMANN_JSON_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../components/nlohmann_json/include")

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

# SeedSigner font sources used by screenshot_gen
set(SEEDSIGNER_FONT_SOURCES
  "${SEEDSIGNER_DIR}/fonts/opensans_semibold_20_4bpp_125x.c"
  "${SEEDSIGNER_DIR}/fonts/opensans_semibold_18_4bpp_125x.c"
  "${SEEDSIGNER_DIR}/fonts/opensans_regular_17_4bpp_125x.c"
  "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp_125x.c"
  "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp_125x.c"
)

set(SEEDSIGNER_IMAGE_SOURCES
  "${SEEDSIGNER_DIR}/images/seedsigner_logo_img.c"
)

set(SEEDSIGNER_CORE_SOURCES
  "${SEEDSIGNER_DIR}/components.cpp"
  "${SEEDSIGNER_DIR}/input_profile.cpp"
  "${SEEDSIGNER_DIR}/navigation.cpp"
  "${SEEDSIGNER_DIR}/seedsigner.cpp"
)

set(SCREENSHOT_GEN_SOURCE
  "${CMAKE_CURRENT_LIST_DIR}/screenshot_gen.cpp"
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
)

target_include_directories(screenshot_gen PRIVATE
  "${LVGL_ROOT}"
  "${SEEDSIGNER_DIR}"
  "${NLOHMANN_JSON_INCLUDE_DIR}"
)


target_link_libraries(screenshot_gen PRIVATE PNG::PNG)
