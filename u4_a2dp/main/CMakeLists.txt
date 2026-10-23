idf_component_register(SRCS "bt_app_core.c"
                            "main.c"
                            "sdio_link.c"
                    PRIV_REQUIRES bt nvs_flash esp_driver_sdio esp_ringbuf
                    INCLUDE_DIRS ".")
