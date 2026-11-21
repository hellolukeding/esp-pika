set(BOARD_SRCS
    "src/board_s31_mosaico.cc"
    "src/board_s31_coreboard.cc"
    "src/board_s31_korvo.cc"
)

idf_component_register(
    SRCS ${BOARD_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2s esp_driver_gpio
    PRIV_REQUIRES esp_driver_i2c esp_driver_spi esp_lcd esp_codec_dev espressif__esp_tinyusb espressif__esp_lcd_co5300 espressif__bmi270 freertos heap log
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
