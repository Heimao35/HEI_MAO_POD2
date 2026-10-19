# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32p4-idf5_co5300-mipi_esp-lvgl-port_lvgl9)

include("${CMAKE_SOURCE_DIR}/scripts/display_sleep.cmake")

include("${CMAKE_SOURCE_DIR}/scripts/jpeg_cleanup.cmake")

# Keep custom memory core in LVGL's archive so its internal references extract it
# regardless of main/LVGL static-library ordering. Managed sources stay untouched.
idf_component_get_property(player_lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_sources(${player_lvgl_lib} PRIVATE "${CMAKE_SOURCE_DIR}/main/ui/ui_memory.c")
