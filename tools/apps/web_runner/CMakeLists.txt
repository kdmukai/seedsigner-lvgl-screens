cmake_minimum_required(VERSION 3.16)
project(web_runner LANGUAGES C CXX)

# This project is built with the Emscripten toolchain (emcmake cmake ...).
# Use the official emscripten/emsdk Docker image — see build.sh.
if(NOT EMSCRIPTEN)
  message(FATAL_ERROR
    "web_runner must be configured with the Emscripten toolchain.\n"
    "Use the wrapper:  bash tools/apps/web_runner/build.sh\n"
    "or:  emcmake cmake -S tools/apps/web_runner -B tools/apps/web_runner/build-wasm ...")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Default display dimensions (compile-time initial resolution; the page can
# switch at runtime via ss_set_resolution).
# ---------------------------------------------------------------------------
if(NOT DEFINED DISPLAY_WIDTH OR NOT DEFINED DISPLAY_HEIGHT)
  set(DISPLAY_WIDTH 240)
  set(DISPLAY_HEIGHT 240)
endif()

# Desktop/web tools support all display heights so the user can switch at runtime.
if(NOT DEFINED SUPPORT_DISPLAY_HEIGHT_240)
  set(SUPPORT_DISPLAY_HEIGHT_240 ON)
endif()
if(NOT DEFINED SUPPORT_DISPLAY_HEIGHT_320)
  set(SUPPORT_DISPLAY_HEIGHT_320 ON)
endif()
if(NOT DEFINED SUPPORT_DISPLAY_HEIGHT_480)
  set(SUPPORT_DISPLAY_HEIGHT_480 ON)
endif()

set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
set(SEEDSIGNER_DIR "${PROJECT_ROOT}/components/seedsigner")
set(COMMON_DIR "${PROJECT_ROOT}/tools/apps/runner_core")
set(NLOHMANN_JSON_INCLUDE_DIR "${PROJECT_ROOT}/components/nlohmann_json/include")

if(NOT DEFINED LVGL_ROOT OR LVGL_ROOT STREQUAL "")
  set(LVGL_ROOT "${PROJECT_ROOT}/third_party/lvgl")
endif()
if(NOT EXISTS "${LVGL_ROOT}/lvgl.h")
  message(FATAL_ERROR "LVGL root not found at ${LVGL_ROOT}. Pass -DLVGL_ROOT=<path>.")
endif()
if(NOT EXISTS "${NLOHMANN_JSON_INCLUDE_DIR}/nlohmann/json.hpp")
  message(FATAL_ERROR "nlohmann/json.hpp not found at: ${NLOHMANN_JSON_INCLUDE_DIR}")
endif()

file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS "${LVGL_ROOT}/src/*.c")

# Resolution-scaled fonts (mirrors tools/apps/screen_runner/CMakeLists.txt).
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

set(SEEDSIGNER_CORE_SOURCES
  "${SEEDSIGNER_DIR}/gui_constants.cpp"
  "${SEEDSIGNER_DIR}/components.cpp"
  "${SEEDSIGNER_DIR}/input_profile.cpp"
  "${SEEDSIGNER_DIR}/navigation.cpp"
  "${SEEDSIGNER_DIR}/seedsigner.cpp"
  "${SEEDSIGNER_DIR}/images/seedsigner_logo_img.c")

set(RUNNER_COMMON_SOURCES
  "${COMMON_DIR}/runner_core.cpp"
  "${COMMON_DIR}/runner_sdl.cpp")

# ---------------------------------------------------------------------------
# Generate the embedded scenario catalog (--pre-js) from tools/scenarios/scenarios.json.
# ---------------------------------------------------------------------------
set(SCENARIOS_JSON "${PROJECT_ROOT}/tools/scenarios/scenarios.json")
set(SCENARIOS_PREJS "${CMAKE_CURRENT_BINARY_DIR}/scenarios.gen.js")
add_custom_command(
  OUTPUT "${SCENARIOS_PREJS}"
  COMMAND ${CMAKE_COMMAND} -E env python3
          "${CMAKE_CURRENT_LIST_DIR}/gen_scenarios.py" "${SCENARIOS_JSON}" "${SCENARIOS_PREJS}"
  DEPENDS "${SCENARIOS_JSON}" "${CMAKE_CURRENT_LIST_DIR}/gen_scenarios.py"
  COMMENT "Generating embedded scenario catalog (scenarios.gen.js)")
add_custom_target(web_runner_scenarios DEPENDS "${SCENARIOS_PREJS}")

add_executable(web_runner
  ${LVGL_SOURCES}
  ${SEEDSIGNER_FONT_SOURCES}
  ${SEEDSIGNER_CORE_SOURCES}
  ${RUNNER_COMMON_SOURCES}
  "${CMAKE_CURRENT_LIST_DIR}/web_runner.cpp")

add_dependencies(web_runner web_runner_scenarios)

target_compile_definitions(web_runner PRIVATE
  LV_CONF_SKIP
  LV_USE_DRAW_SW_ASM=LV_DRAW_SW_ASM_NONE
  DISPLAY_WIDTH=${DISPLAY_WIDTH}
  DISPLAY_HEIGHT=${DISPLAY_HEIGHT}
  $<$<BOOL:${SUPPORT_DISPLAY_HEIGHT_240}>:SUPPORT_DISPLAY_HEIGHT_240>
  $<$<BOOL:${SUPPORT_DISPLAY_HEIGHT_320}>:SUPPORT_DISPLAY_HEIGHT_320>
  $<$<BOOL:${SUPPORT_DISPLAY_HEIGHT_480}>:SUPPORT_DISPLAY_HEIGHT_480>)

target_include_directories(web_runner PRIVATE
  "${LVGL_ROOT}"
  "${SEEDSIGNER_DIR}"
  "${COMMON_DIR}"
  "${NLOHMANN_JSON_INCLUDE_DIR}")

# Emscripten: use the bundled SDL2 port for the canvas + input, enable C++
# exceptions (the screens throw on malformed JSON), and emit a single
# self-contained index.html (wasm inlined) generated from shell.html.
target_compile_options(web_runner PRIVATE
  -sUSE_SDL=2
  -fexceptions
  -O2)

target_link_options(web_runner PRIVATE
  -sUSE_SDL=2
  -sSINGLE_FILE=1
  -sALLOW_MEMORY_GROWTH=1
  -fexceptions
  -O2
  "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString']"
  "-sEXPORTED_FUNCTIONS=['_main','_ss_load_screen','_ss_set_resolution','_ss_set_input_mode','_ss_send_key','_ss_scroll','_ss_get_width','_ss_get_height']"
  "--shell-file" "${CMAKE_CURRENT_LIST_DIR}/shell.html"
  "--pre-js" "${SCENARIOS_PREJS}")

# Force a relink when the shell or the generated catalog changes.
set_target_properties(web_runner PROPERTIES
  OUTPUT_NAME "index"
  SUFFIX ".html"
  LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/shell.html;${SCENARIOS_PREJS}")
