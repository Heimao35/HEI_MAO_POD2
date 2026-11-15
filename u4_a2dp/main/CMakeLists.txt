idf_component_register(SRCS "bt_app_core.c"
                            "main.c"
                            "sdio_link.c"
                            "u4_ota.c"
                    PRIV_REQUIRES bt nvs_flash esp_driver_sdio esp_ringbuf
                                  app_update esp_partition
                                  esp_app_format esp_bootloader_format
                    INCLUDE_DIRS ".")
