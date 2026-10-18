# Keep the SDK installation untouched. Compile its pinned DPI driver with the
# project DMA-boundary suspend/synchronized-resume extension in the same translation unit.
# This avoids an ABI-dependent cast of the private panel structure.
if(NOT IDF_VERSION_MAJOR EQUAL 5 OR NOT IDF_VERSION_MINOR EQUAL 5 OR NOT IDF_VERSION_PATCH EQUAL 4)
    message(FATAL_ERROR "Review DPI sleep extension before upgrading ESP-IDF v5.5.4")
endif()
idf_component_get_property(player_lcd_lib esp_lcd COMPONENT_LIB)
get_target_property(player_lcd_sources ${player_lcd_lib} SOURCES)
set(player_dpi_found FALSE)
foreach(player_lcd_source IN LISTS player_lcd_sources)
    if(player_lcd_source MATCHES "(^|/)esp_lcd_panel_dpi\\.c$")
        list(REMOVE_ITEM player_lcd_sources "${player_lcd_source}")
        set(player_dpi_found TRUE)
    endif()
endforeach()
if(NOT player_dpi_found)
    message(FATAL_ERROR "DPI sleep extension: cannot find original driver source")
endif()
set(player_dpi_sdk "$ENV{IDF_PATH}/components/esp_lcd/dsi/esp_lcd_panel_dpi.c")
set(player_dpi_generated "${CMAKE_BINARY_DIR}/player_display/esp_lcd_panel_dpi.c")
execute_process(COMMAND "${PYTHON}" "${CMAKE_SOURCE_DIR}/scripts/patch_dpi_sleep.py"
    "${player_dpi_sdk}" "${CMAKE_SOURCE_DIR}/main/display_dpi_sleep.inc" "${player_dpi_generated}"
    RESULT_VARIABLE player_patch_result)
if(NOT player_patch_result EQUAL 0)
    message(FATAL_ERROR "DPI sleep extension generation failed")
endif()
set_property(TARGET ${player_lcd_lib} PROPERTY SOURCES "${player_lcd_sources}")
target_sources(${player_lcd_lib} PRIVATE "${player_dpi_generated}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${player_dpi_sdk}" "${CMAKE_SOURCE_DIR}/scripts/patch_dpi_sleep.py"
    "${CMAKE_SOURCE_DIR}/main/display_dpi_sleep.inc")

target_include_directories(${player_lcd_lib} PRIVATE "$ENV{IDF_PATH}/components/esp_lcd/dsi")
