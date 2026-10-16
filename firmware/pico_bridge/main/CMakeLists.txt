idf_component_register(SRCS "main.c" "bridge.c" "pico_logic.c" "cc1101.c" "pico_decode.c" "rx_pipeline.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_timer esp_driver_gpio esp_driver_spi esp_driver_rmt)
