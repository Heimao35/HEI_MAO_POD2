idf_component_register(SRCS "main.c"
                              "sd_card.c"
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
                    INCLUDE_DIRS "." "ui"
                    PRIV_REQUIRES fatfs sdmmc esp_driver_jpeg
                                  esp_lcd esp_lcd_touch_cst820
                                  esp_hw_support driver
                                  esp_lvgl_port)