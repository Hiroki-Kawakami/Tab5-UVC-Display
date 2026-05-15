#include "usb_host.hpp"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "usb_host";

namespace usb_host {

void install() {
    usb_host_config_t config = {};
    config.intr_flags = ESP_INTR_FLAG_LEVEL1;
    ESP_ERROR_CHECK(usb_host_install(&config));
    xTaskCreatePinnedToCore([](void*){
        while (true) {
            uint32_t event_flags = 0;
            usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
                usb_host_device_free_all();
            }
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
                ESP_LOGI(TAG, "All devices freed");
            }
        }
    }, "usb_host_lib", 4096, nullptr, 10, nullptr, 0);
    ESP_LOGI(TAG, "USB host driver installed");
}

void UVC::install() {
    uvc_host_driver_config_t config = {};
    config.driver_task_stack_size = 6 * 1024;
    config.driver_task_priority = 6;
    config.xCoreID = 0;
    config.create_background_task = true;
    config.event_cb = [](const uvc_host_driver_event_data_t *event, void *user_ctx){
        switch (event->type) {
        case UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED: {
            ESP_LOGI(TAG, "Device connected, addr: %d, stream index: %d", event->device_connected.dev_addr, event->device_connected.uvc_stream_index);
            break;
        }
        default:
            break;
        }
    };
    config.user_ctx = this;
    ESP_ERROR_CHECK(uvc_host_install(&config));
    ESP_LOGI(TAG, "UVC host driver installed");
}

esp_err_t UVC::open(int width, int height, float fps) {
    uvc_host_stream_config_t config = {};
    config.event_cb = [](const uvc_host_stream_event_data_t *event, void *user_ctx){
        auto uvc = static_cast<UVC*>(user_ctx);
        if (uvc->callback_) uvc->callback_->onEvent(event);
    };
    config.frame_cb = [](const uvc_host_frame_t *frame, void *user_ctx){
        auto uvc = static_cast<UVC*>(user_ctx);
        if (uvc->callback_) return uvc->callback_->onFrame(frame);
        return true;
    };
    config.user_ctx = this;
    config.vs_format.h_res = width;
    config.vs_format.v_res = height;
    config.vs_format.fps = fps;
    config.vs_format.format = UVC_VS_FORMAT_MJPEG;
    config.advanced.number_of_frame_buffers = 4;
    config.advanced.frame_size = 2048 * 1024;
    config.advanced.frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED;
    config.advanced.number_of_urbs = 3;
    config.advanced.urb_size = 4 * 1024;
    auto res = uvc_host_stream_open(&config, pdMS_TO_TICKS(1000), &stream_);
    ESP_LOGI(TAG, "uvc_host_stream_open: result=%s", esp_err_to_name(res));
    return res;
}

void UVC::start() {
    ESP_ERROR_CHECK(uvc_host_stream_start(stream_));
}

void UVC::returnFrame(const uvc_host_frame_t *frame) {
    uvc_host_frame_return(stream_, const_cast<uvc_host_frame_t*>(frame));
}

}
