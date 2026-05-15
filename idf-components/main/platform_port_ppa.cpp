#include "platform_port.hpp"
#include "esp_log.h"
#include <assert.h>

static const char *TAG = "PPA";

namespace pf_port {

SRMClient::SRMClient() {
    ppa_client_config_t config = {};
    config.oper_type = PPA_OPERATION_SRM;
    ESP_ERROR_CHECK(ppa_register_client(&config, &ppa_client_));
}

SRMClient::~SRMClient() {
    ppa_unregister_client(ppa_client_);
}

void SRMClient::setInputBlock(
    uint32_t pic_w,
    uint32_t pic_h,
    uint32_t block_w,
    uint32_t block_h,
    uint32_t block_offset_x,
    uint32_t block_offset_y,
    PixelFormat pixel_format
) {
    oper_config_.in.pic_w = pic_w;
    oper_config_.in.pic_h = pic_h;
    oper_config_.in.block_w = block_w;
    oper_config_.in.block_h = block_h;
    oper_config_.in.block_offset_x = block_offset_x;
    oper_config_.in.block_offset_y = block_offset_y;
    switch (pixel_format) {
    case pf_port::PixelFormat::RGB565:
        oper_config_.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        break;
    case pf_port::PixelFormat::RGB888:
        oper_config_.in.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
        break;
    }
}

void SRMClient::setOutputBlock(
    uint32_t pic_w,
    uint32_t pic_h,
    uint32_t block_offset_x,
    uint32_t block_offset_y,
    PixelFormat pixel_format
) {
    oper_config_.out.buffer_size = pic_w * pic_h;
    oper_config_.out.pic_w = pic_w;
    oper_config_.out.pic_h = pic_h;
    oper_config_.out.block_offset_x = block_offset_x;
    oper_config_.out.block_offset_y = block_offset_y;
    switch (pixel_format) {
    case pf_port::PixelFormat::RGB565:
        oper_config_.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        oper_config_.out.buffer_size *= 2;
        break;
    case pf_port::PixelFormat::RGB888:
        oper_config_.out.srm_cm = PPA_SRM_COLOR_MODE_RGB888;
        oper_config_.out.buffer_size *= 3;
        break;
    }
}

void SRMClient::setRotation(int rotation) {
    switch (rotation) {
    case 0:
        oper_config_.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
        break;
    case 90:
        oper_config_.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;
        break;
    case 180:
        oper_config_.rotation_angle = PPA_SRM_ROTATION_ANGLE_180;
        break;
    case 270:
        oper_config_.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
        break;
    default:
        ESP_LOGE(TAG, "Invalid PPA SRM Rotation Angle: %d\n", rotation);
        break;
    }
}

void SRMClient::setScale(float scale_x, float scale_y) {
    oper_config_.scale_x = scale_x;
    oper_config_.scale_y = scale_y;
}

Error SRMClient::do_scale_rotate_mirror(const void *input, void *output) {
    oper_config_.in.buffer = input;
    oper_config_.out.buffer = output;
    auto err = ppa_do_scale_rotate_mirror(ppa_client_, &oper_config_);
    switch (err) {
    case ESP_OK: return Error::Ok;
    case ESP_ERR_INVALID_ARG: return Error::InvalidArgument;
    default: return Error::Fail;
    }
}

}
