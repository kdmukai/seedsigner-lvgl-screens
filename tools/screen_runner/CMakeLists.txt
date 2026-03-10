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

# Repo layout assumptions:
# - this file lives at seedsigner-c-modules/tools/screen_runner
# - LVGL headers are provided via -DLVGL_ROOT=<path> (preferred), or auto-detected below.
set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(SEEDSIGNER_DIR "${PROJECT_ROOT}/components/seedsigner")
set(NLOHMANN_JSON_INCLUDE_DIR "${PROJECT_ROOT}/components/nlohmann_json/include")

if(NOT DEFINED LVGL_ROOT OR LVGL_ROOT STREQUAL "")
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

set(SEEDSIGNER_FONT_SOURCES
  "${SEEDSIGNER_DIR}/fonts/opensans_semibold_20_4bpp_125x.c"
  "${SEEDSIGNER_DIR}/fonts/opensans_semibold_18_4bpp_125x.c"
  "${SEEDSIGNER_DIR}/fonts/opensans_regular_17_4bpp_125x.c"
  "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp_125x.c"
  "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp_125x.c"
)

set(SEEDSIGNER_CORE_SOURCES
  "${SEEDSIGNER_DIR}/components.cpp"
  "${SEEDSIGNER_DIR}/input_profile.cpp"
  "${SEEDSIGNER_DIR}/navigation.cpp"
  "${SEEDSIGNER_DIR}/seedsigner.cpp"
)

set(SCREEN_RUNNER_SOURCE
  "${CMAKE_CURRENT_LIST_DIR}/screen_runner.cpp"
)

add_executable(screen_runner
  ${LVGL_SOURCES}
  ${SEEDSIGNER_FONT_SOURCES}
  ${SEEDSIGNER_CORE_SOURCES}
  ${SCREEN_RUNNER_SOURCE}
)

target_compile_definitions(screen_runner PRIVATE LV_CONF_SKIP)

target_include_directories(screen_runner PRIVATE
  "${LVGL_ROOT}"
  "${SEEDSIGNER_DIR}"
  "${NLOHMANN_JSON_INCLUDE_DIR}"
)

if(TARGET SDL2::SDL2)
  target_link_libraries(screen_runner PRIVATE SDL2::SDL2)
else()
  target_include_directories(screen_runner PRIVATE ${SDL2_INCLUDE_DIRS})
  target_link_libraries(screen_runner PRIVATE ${SDL2_LIBRARIES})
endif()
