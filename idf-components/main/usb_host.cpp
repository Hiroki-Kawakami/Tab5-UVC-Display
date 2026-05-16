#include "usb_host.hpp"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

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

// --- UAC ---------------------------------------------------------------

void UAC::install(size_t stack_size, size_t priority, int core_id) {
    rx_detected_sem_ = xSemaphoreCreateBinary();
    uac_host_driver_config_t config = {};
    config.create_background_task = true;
    config.task_priority = priority;
    config.stack_size = stack_size;
    config.core_id = core_id;
    config.callback = &UAC::driverEventCb;
    config.callback_arg = this;
    ESP_ERROR_CHECK(uac_host_install(&config));
    ESP_LOGI(TAG, "UAC host driver installed");
}

void UAC::driverEventCb(uint8_t addr, uint8_t iface_num,
                        uac_host_driver_event_t event, void *arg) {
    auto self = static_cast<UAC*>(arg);
    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        ESP_LOGI(TAG, "UAC RX device connected: addr=%d iface=%d", addr, iface_num);
        if (!self->rx_detected_) {
            self->rx_addr_ = addr;
            self->rx_iface_ = iface_num;
            self->rx_detected_ = true;
            xSemaphoreGive(self->rx_detected_sem_);
        }
    } else if (event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
        ESP_LOGI(TAG, "UAC TX device connected: addr=%d iface=%d (ignored)", addr, iface_num);
    }
}

void UAC::deviceEventCb(uac_host_device_handle_t handle,
                        uac_host_device_event_t event, void *arg) {
    auto self = static_cast<UAC*>(arg);
    switch (event) {
    case UAC_HOST_DEVICE_EVENT_RX_DONE:
        self->handleRxDone();
        break;
    case UAC_HOST_DEVICE_EVENT_TX_DONE:
        break;
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "UAC transfer error");
        break;
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "UAC device disconnected");
        uac_host_device_close(handle);
        break;
    default:
        break;
    }
}

void UAC::handleRxDone() {
    if (!device_ || !rx_buf_) return;
    uint32_t bytes_read = 0;
    esp_err_t err = uac_host_device_read(device_, rx_buf_, rx_buf_size_, &bytes_read, 0);
    if (err != ESP_OK || bytes_read == 0) return;
    if (callback_) callback_->onRxData(rx_buf_, bytes_read);
}

esp_err_t UAC::openRx(uint32_t buffer_size, uint32_t buffer_threshold,
                     uint32_t timeout_ms) {
    if (device_) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(rx_detected_sem_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "No UAC RX device detected within %lums", (unsigned long)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    rx_buf_size_ = buffer_size;
    rx_buf_ = static_cast<uint8_t*>(heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
    if (!rx_buf_) return ESP_ERR_NO_MEM;

    uac_host_device_config_t config = {};
    config.addr = rx_addr_;
    config.iface_num = rx_iface_;
    config.buffer_size = buffer_size;
    config.buffer_threshold = buffer_threshold;
    config.callback = &UAC::deviceEventCb;
    config.callback_arg = this;

    esp_err_t err = uac_host_device_open(&config, &device_);
    if (err != ESP_OK) {
        heap_caps_free(rx_buf_);
        rx_buf_ = nullptr;
        ESP_LOGE(TAG, "uac_host_device_open failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "UAC RX device opened (addr=%d iface=%d)", rx_addr_, rx_iface_);
    return ESP_OK;
}

esp_err_t UAC::getDeviceInfo(uac_host_dev_info_t *info) {
    if (!device_ || !info) return ESP_ERR_INVALID_ARG;
    return uac_host_get_device_info(device_, info);
}

esp_err_t UAC::start(uint32_t sample_rate, uint8_t channels, uint8_t bit_resolution) {
    if (!device_) return ESP_ERR_INVALID_STATE;
    uac_host_stream_config_t config = {};
    config.channels = channels;
    config.bit_resolution = bit_resolution;
    config.sample_freq = sample_rate;
    esp_err_t err = uac_host_device_start(device_, &config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UAC stream started: %luHz %dch %dbit",
                 (unsigned long)sample_rate, channels, bit_resolution);
    }
    return err;
}

esp_err_t UAC::setVolume(uint8_t volume) {
    if (!device_) return ESP_ERR_INVALID_STATE;
    return uac_host_device_set_volume(device_, volume);
}

esp_err_t UAC::setMute(bool mute) {
    if (!device_) return ESP_ERR_INVALID_STATE;
    return uac_host_device_set_mute(device_, mute);
}

void UAC::close() {
    if (device_) {
        uac_host_device_stop(device_);
        uac_host_device_close(device_);
        device_ = nullptr;
    }
    if (rx_buf_) {
        heap_caps_free(rx_buf_);
        rx_buf_ = nullptr;
    }
}

}
