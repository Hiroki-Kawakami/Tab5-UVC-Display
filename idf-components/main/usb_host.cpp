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
        if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
            // Per the IDF UVC example, the stream must be closed from this
            // callback so the USB host can free the device record. Skipping
            // this leaves the device stuck and every subsequent
            // uvc_host_stream_open() returns ESP_ERR_INVALID_STATE.
            uvc_host_stream_close(event->device_disconnected.stream_hdl);
            if (uvc->stream_ == event->device_disconnected.stream_hdl) {
                uvc->stream_ = nullptr;
            }
        }
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

void UVC::close() {
    if (!stream_) return;
    uvc_host_stream_stop(stream_);
    uvc_host_stream_close(stream_);
    stream_ = nullptr;
}

void UVC::returnFrame(const uvc_host_frame_t *frame) {
    uvc_host_frame_return(stream_, const_cast<uvc_host_frame_t*>(frame));
}

// --- UAC ---------------------------------------------------------------

void UAC::install(size_t stack_size, size_t priority, int core_id) {
    rx_detected_sem_   = xSemaphoreCreateBinary();
    close_pending_sem_ = xSemaphoreCreateBinary();
    // Priority must be higher than the UAC driver task so this preempts
    // and finishes uac_host_device_close before the driver's disconnect
    // loop spins another iteration.
    xTaskCreatePinnedToCore(&UAC::closerTaskFn, "uac_closer", 4096, this,
                            priority + 5, &closer_task_, core_id);

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

void UAC::closerTaskFn(void *arg) {
    auto self = static_cast<UAC*>(arg);
    while (true) {
        if (xSemaphoreTake(self->close_pending_sem_, portMAX_DELAY) != pdTRUE) continue;
        auto h = self->pending_close_;
        if (!h) continue;
        self->pending_close_ = nullptr;
        // Outside the UAC driver task's call stack, so try_lock doesn't hit
        // the recursive/corrupted-lock path that crashes when invoked from
        // the disconnect callback context.
        uac_host_device_close(h);
    }
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
        // The IDF UAC driver loops in _uac_host_device_disconnected, firing
        // this callback every iteration, until we ack with
        // uac_host_device_close(). Calling that directly from here corrupts
        // the interface lock state in our setup, so we hand it to the
        // closer task (higher priority) instead. Guard with device_ so the
        // repeat callbacks the driver fires before the closer runs don't
        // re-queue the same handle.
        if (self->device_) {
            ESP_LOGW(TAG, "UAC device disconnected");
            self->pending_close_ = handle;
            self->device_ = nullptr;
            if (self->close_pending_sem_) xSemaphoreGive(self->close_pending_sem_);
        }
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
    if (!tx_buf_) return;

    // Non-blocking push. If the consumer can't keep up (codec writes blocking on
    // I2S DMA), drop the tail of this chunk instead of stalling the UAC driver
    // task — back-pressure into UAC's internal ring would cause sample loss
    // anyway, and a brief audible glitch beats accumulating drift.
    size_t free = xStreamBufferSpacesAvailable(tx_buf_);
    size_t to_send = bytes_read < free ? bytes_read : free;
    if (to_send > 0) {
        xStreamBufferSend(tx_buf_, rx_buf_, to_send, 0);
    }
}

void UAC::consumerTaskFn(void *arg) {
    auto self = static_cast<UAC*>(arg);
    constexpr size_t CHUNK = 4096;
    uint8_t *chunk = static_cast<uint8_t*>(heap_caps_malloc(CHUNK,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
    if (!chunk) {
        ESP_LOGE(TAG, "UAC consumer: chunk alloc failed");
        vTaskDelete(nullptr);
        return;
    }
    while (true) {
        size_t got = xStreamBufferReceive(self->tx_buf_, chunk, CHUNK, portMAX_DELAY);
        if (got > 0 && self->callback_) {
            self->callback_->onRxData(chunk, got);
        }
    }
}

esp_err_t UAC::openRx(uint32_t buffer_size, uint32_t buffer_threshold,
                     uint32_t timeout_ms, size_t stream_buf_size,
                     size_t consumer_task_priority, int consumer_task_core_id) {
    if (device_) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(rx_detected_sem_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "No UAC RX device detected within %lums", (unsigned long)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    rx_buf_size_ = buffer_size;
    rx_buf_ = static_cast<uint8_t*>(heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
    if (!rx_buf_) return ESP_ERR_NO_MEM;

    tx_buf_ = xStreamBufferCreate(stream_buf_size, 1);
    if (!tx_buf_) {
        heap_caps_free(rx_buf_);
        rx_buf_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(&UAC::consumerTaskFn, "uac_play", 4096, this,
                                consumer_task_priority, &consumer_task_,
                                consumer_task_core_id) != pdPASS) {
        vStreamBufferDelete(tx_buf_); tx_buf_ = nullptr;
        heap_caps_free(rx_buf_);      rx_buf_ = nullptr;
        return ESP_ERR_NO_MEM;
    }

    uac_host_device_config_t config = {};
    config.addr = rx_addr_;
    config.iface_num = rx_iface_;
    config.buffer_size = buffer_size;
    config.buffer_threshold = buffer_threshold;
    config.callback = &UAC::deviceEventCb;
    config.callback_arg = this;

    esp_err_t err = uac_host_device_open(&config, &device_);
    if (err != ESP_OK) {
        vTaskDelete(consumer_task_);  consumer_task_ = nullptr;
        vStreamBufferDelete(tx_buf_); tx_buf_ = nullptr;
        heap_caps_free(rx_buf_);      rx_buf_ = nullptr;
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
    // If a disconnect hasn't fired yet (proactive close), hand the device
    // off to the closer task so uac_host_device_close runs out of any
    // potentially-locked context.
    if (device_) {
        pending_close_ = device_;
        device_ = nullptr;
        if (close_pending_sem_) xSemaphoreGive(close_pending_sem_);
    }
    if (consumer_task_) {
        vTaskDelete(consumer_task_);
        consumer_task_ = nullptr;
    }
    if (tx_buf_) {
        vStreamBufferDelete(tx_buf_);
        tx_buf_ = nullptr;
    }
    if (rx_buf_) {
        heap_caps_free(rx_buf_);
        rx_buf_ = nullptr;
    }
    // Reset the "RX-capable device detected" latch so a subsequent openRx()
    // properly blocks for a new enumeration after reconnect.
    rx_detected_ = false;
    if (rx_detected_sem_) xSemaphoreTake(rx_detected_sem_, 0);
}

}
