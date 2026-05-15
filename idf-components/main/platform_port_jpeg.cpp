#include "platform_port.hpp"
#include "esp_log.h"

static const char *TAG = "JPEG";

namespace pf_port {

JpegDecoder::JpegDecoder() {
    jpeg_decode_engine_cfg_t engine_cfg = {};
    engine_cfg.intr_priority = 0;
    engine_cfg.timeout_ms = 100;
    ESP_ERROR_CHECK(jpeg_new_decoder_engine(&engine_cfg, &decoder_));

    cfg_.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
    cfg_.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    cfg_.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
}

JpegDecoder::~JpegDecoder() {
    if (decoder_) {
        jpeg_del_decoder_engine(decoder_);
    }
}

std::optional<JpegDecoder::PictureInfo> JpegDecoder::getInfo(const void *stream, size_t stream_size) {
    jpeg_decode_picture_info_t info = {};
    esp_err_t err = jpeg_decoder_get_info(
        static_cast<const uint8_t *>(stream),
        static_cast<uint32_t>(stream_size),
        &info
    );
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_decoder_get_info failed: %s", esp_err_to_name(err));
        return std::nullopt;
    }
    return PictureInfo{info.width, info.height};
}

void JpegDecoder::setOutputFormat(PixelFormat pixel_format) {
    switch (pixel_format) {
    case PixelFormat::RGB565:
        cfg_.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
        break;
    case PixelFormat::RGB888:
        cfg_.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
        break;
    }
}

Error JpegDecoder::decode(const void *stream, size_t stream_size,
                          void *output, size_t output_size,
                          size_t *out_size) {
    uint32_t produced = 0;
    esp_err_t err = jpeg_decoder_process(
        decoder_, &cfg_,
        static_cast<const uint8_t *>(stream),
        static_cast<uint32_t>(stream_size),
        static_cast<uint8_t *>(output),
        static_cast<uint32_t>(output_size),
        &produced
    );
    if (out_size) *out_size = produced;
    switch (err) {
    case ESP_OK: return Error::Ok;
    case ESP_ERR_INVALID_ARG: return Error::InvalidArgument;
    default: return Error::Fail;
    }
}

}
