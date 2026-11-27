set(BOARD_SRCS
    "src/board_s31_mosaico.cc"
    "src/board_s31_coreboard.cc"
    "src/board_s31_korvo.cc"
    "src/chime_pcm.cc"
)

idf_component_register(
    SRCS ${BOARD_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2s esp_driver_gpio
    PRIV_REQUIRES esp_driver_i2c esp_driver_spi esp_driver_cam esp_driver_ppa esp_lcd esp_codec_dev esp_hw_support esp_mm espressif__esp_tinyusb espressif__esp_lcd_co5300 espressif__bmi270 espressif__esp_video espressif__esp_new_jpeg freertos heap log
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
