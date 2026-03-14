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
  "${SEEDSIGNER_DIR}/images/seedsigner_logo_img.c"
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
