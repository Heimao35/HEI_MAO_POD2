idf_component_register(SRCS "main.c"
                              "sd_card.c"
                              "lock_animation.c"
                              "pmic_axp2101.c"
                              "screen_power.c"
                              "ambient_light.c"
                              "system_power.c"
                              "player_settings.c"
                              "volume_keys.c"
                              "music_library.c"
                              "audio_player.c"
                              "audio_output.c"
                              "audio_pcm.c"
                              "audio_mp3.c"
                              "cs43131.c"
                              "lyric_parser.c"
                              "cover_loader.c"
                              "music_background.c"
                              "music_player_adapter.c"
                              "ui/ui_fonts.c"
                              "ui/ui_common.c"
                              "ui/ui_statusbar.c"
                              "ui/ui_lockscreen.c"
                              "ui/ui_music_player.c"
                              "ui/ui_video_player.c"
                              "ui/ui_image_viewer.c"
                              "ui/ui_ebook.c"
                              "ui/ui_mode_panel.c"
                              "ui/ui_adj_panel.c"
                              "ui/ui_settings.c"
                              "ui/ui_manager.c"
                              "ui/ui_power_dialog.c"
                    INCLUDE_DIRS "." "ui"
                    PRIV_REQUIRES fatfs sdmmc esp_driver_jpeg esp_partition
                                  esp_lcd esp_lcd_touch_cst820
                                  esp_hw_support driver
                                  esp_lvgl_port
                                  esp_audio_codec spiffs nvs_flash esp_adc esp_driver_i2s)

# Font assets are packaged into a separate read-only-at-runtime SPIFFS image.
# The image is flashed by `idf.py flash` together with the application.
spiffs_create_partition_image(fontfs "${CMAKE_CURRENT_LIST_DIR}/../font" FLASH_IN_PROJECT)
