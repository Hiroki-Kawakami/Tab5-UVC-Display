idf_component_register(
    SRCS
        "src/jpeg_strip_decode.c"
        "src/jpeg_ppa_pipeline.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_jpeg esp_driver_ppa hal esp_mm esp_hw_support driver
)

# NOTE: depends on ESP-IDF internal headers (jpeg_private.h, private/*.h,
# esp_private/dma2d.h, hal/jpeg_ll.h). These live inside the esp_driver_jpeg
# and esp_hw_support components and are not exported as public include dirs,
# so we add them explicitly.
target_include_directories(${COMPONENT_LIB} PRIVATE
    "$ENV{IDF_PATH}/components/esp_driver_jpeg"
    "$ENV{IDF_PATH}/components/esp_driver_jpeg/private"
)
