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
# The five translated text roles are rasterized at runtime from the compiled-in
# OpenSans Western TTF (one Regular + one SemiBold subset, all sizes); only the
# baked icon + Inconsolata keyboard fonts are listed per height below.
set(SEEDSIGNER_FONT_SOURCES
  "${SEEDSIGNER_DIR}/fonts/opensans_western_regular.c"
  "${SEEDSIGNER_DIR}/fonts/opensans_western_semibold.c")
if(SUPPORT_DISPLAY_HEIGHT_240)
  list(APPEND SEEDSIGNER_FONT_SOURCES
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_48_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_26_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_24_4bpp.c"
    "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_22_4bpp.c")
endif()
if(SUPPORT_DISPLAY_HEIGHT_320)
  list(APPEND SEEDSIGNER_FONT_SOURCES
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp_133x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_26_4bpp_133x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp_133x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_48_4bpp_133x.c"
    "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_24_4bpp_133x.c"
    "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_22_4bpp_133x.c")
endif()
if(SUPPORT_DISPLAY_HEIGHT_480)
  list(APPEND SEEDSIGNER_FONT_SOURCES
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_24_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_26_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_36_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/seedsigner_icons_48_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_24_4bpp_200x.c"
    "${SEEDSIGNER_DIR}/fonts/inconsolata_semibold_22_4bpp_200x.c")
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
  "${SEEDSIGNER_DIR}/components.cpp"
  "${SEEDSIGNER_DIR}/camera_preview_overlay.cpp"
  "${SEEDSIGNER_DIR}/keyboard_core.cpp"
  "${SEEDSIGNER_DIR}/input_profile.cpp"
  "${SEEDSIGNER_DIR}/navigation.cpp"
  "${SEEDSIGNER_DIR}/seedsigner.cpp"
  # Shared overlay manager: screensaver idle-watch dispatcher (toast queue later).
  "${SEEDSIGNER_DIR}/overlay_manager.cpp"
  # Locale font seam + the shared locale-pack loader (ss_load_locale): the web host
  # pre-fetches each pack blob and stages it, then the loader chains the fetched
  # script/CJK packs over the baked baseline and installs complex-script glyph runs.
  "${SEEDSIGNER_DIR}/font_registry.cpp"
  "${SEEDSIGNER_DIR}/locale_fonts.cpp"
  "${SEEDSIGNER_DIR}/locale_loader.cpp"
  # Complex-script (Devanagari/Thai/Nastaliq) glyph-run render layer.
  "${SEEDSIGNER_DIR}/glyph_runs.cpp"
  "${SEEDSIGNER_DIR}/stb_glyph_metrics.c")

set(RUNNER_COMMON_SOURCES
  "${COMMON_DIR}/runner_core.cpp"
  "${COMMON_DIR}/runner_sdl.cpp")

# Scenarios + fonts are NOT baked into the bundle. They are served as static
# assets (assets/scenarios/<locale>.json, assets/lang-packs/<locale>/*.ttf) and
# fetched at runtime, so a translation/font/scenario edit is a file swap with no
# WASM rebuild. The build is the engine only: index.{html,js,wasm}. Asset staging
# lives in stage_assets.py (called by build.sh and the CI assemble-site step).

add_executable(web_runner
  ${LVGL_SOURCES}
  ${SEEDSIGNER_FONT_SOURCES}
  ${SEEDSIGNER_IMAGE_SOURCES}
  ${SEEDSIGNER_CORE_SOURCES}
  ${RUNNER_COMMON_SOURCES}
  "${CMAKE_CURRENT_LIST_DIR}/web_runner.cpp")

target_compile_definitions(web_runner PRIVATE
  LV_CONF_SKIP
  LV_USE_DRAW_SW_ASM=LV_DRAW_SW_ASM_NONE
  # The five translated text roles render from the compiled-in OpenSans Western
  # TTF via lv_tiny_ttf (installed in set_display()); per-locale script fonts also
  # load through tiny_ttf. Use the C library allocator rather than LVGL's 64KB
  # builtin pool, which tiny_ttf's stb rasterizer can exhaust/fragment.
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

# Multi-file output (index.html + index.js + index.wasm), served over HTTP — no
# -sSINGLE_FILE. ALLOW_MEMORY_GROWTH lets tiny_ttf's rasterizer grow the heap.
# JS marshals fetched pack bytes through _malloc/_free into ss_stage_blob, then
# calls ss_load_staged once to drive the shared loader.
target_link_options(web_runner PRIVATE
  -sUSE_SDL=2
  -sALLOW_MEMORY_GROWTH=1
  -fexceptions
  -O2
  "-sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','HEAPU8']"
  "-sEXPORTED_FUNCTIONS=['_main','_ss_load_screen','_ss_set_resolution','_ss_set_input_mode','_ss_send_key','_ss_scroll','_ss_get_width','_ss_get_height','_ss_pack_files','_ss_stage_blob','_ss_load_staged','_ss_reload','_malloc','_free']"
  "--shell-file" "${CMAKE_CURRENT_LIST_DIR}/shell.html")

# Force a relink when the shell changes.
set_target_properties(web_runner PROPERTIES
  OUTPUT_NAME "index"
  SUFFIX ".html"
  LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/shell.html")
