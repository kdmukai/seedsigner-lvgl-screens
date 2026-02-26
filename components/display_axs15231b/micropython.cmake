# MicroPython user C module: lcdaxs
add_library(usermod_lcdaxs INTERFACE)

target_sources(usermod_lcdaxs INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src/display_axs15231b_core.c
    ${CMAKE_CURRENT_LIST_DIR}/modlcdaxs_bindings.c
    ${CMAKE_CURRENT_LIST_DIR}/pmu/bsp_i2c.c
    ${CMAKE_CURRENT_LIST_DIR}/pmu/bsp_axp2101.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pmu/pmu_bridge.cpp
    ${CMAKE_CURRENT_LIST_DIR}/third_party/XPowersLib/src/XPowersLibInterface.cpp
)

target_include_directories(usermod_lcdaxs INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/pmu
    ${CMAKE_CURRENT_LIST_DIR}/third_party/XPowersLib/src
)

target_link_libraries(usermod INTERFACE usermod_lcdaxs)
