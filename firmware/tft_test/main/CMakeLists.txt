idf_component_register(
    SRCS "tft_test.c"
    PRIV_REQUIRES
        esp_driver_spi
        esp_driver_gpio
    INCLUDE_DIRS ""
)
