#pragma once
#include <cstdint>
#include <cstddef>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "usb/uac_host.h"

namespace usb_host {

enum class Error {
    Ok,
};

void install();

class UVC {
public:
    struct Callback {
        virtual void onEvent(const uvc_host_stream_event_data_t *event) = 0;
        virtual bool onFrame(const uvc_host_frame_t *frame) = 0;
    };

    void install();
    esp_err_t open(int width, int height, float frame_rate);
    void start();
    void close();
    void returnFrame(const uvc_host_frame_t *frame);
    void setCallback(Callback *callback) { callback_ = callback; }

private:
    Callback *callback_{nullptr};
    uvc_host_stream_hdl_t stream_{nullptr};
};

class UAC {
public:
    struct Callback {
        /*!< Invoked from the UAC driver background task on every RX_DONE
         *   event with already-read PCM data. */
        virtual void onRxData(const uint8_t *data, size_t len) = 0;
    };

    void install(size_t stack_size = 4 * 1024, size_t priority = 5, int core_id = 0);

    /*!< Wait up to timeout_ms for an RX-capable UAC interface to be enumerated,
     *   then open it. Allocates the internal RX buffer (buffer_size bytes).
     *   stream_buf_size: SPSC buffer between RX_DONE and the consumer task,
     *   absorbs jitter so I2S blocking does not back up the UAC driver. */
    esp_err_t openRx(uint32_t buffer_size, uint32_t buffer_threshold,
                     uint32_t timeout_ms = 5000,
                     size_t stream_buf_size = 32 * 1024,
                     size_t consumer_task_priority = 10,
                     int consumer_task_core_id = 1);

    esp_err_t getDeviceInfo(uac_host_dev_info_t *info);
    esp_err_t start(uint32_t sample_rate, uint8_t channels, uint8_t bit_resolution);
    esp_err_t setVolume(uint8_t volume);
    esp_err_t setMute(bool mute);
    void close();

    void setCallback(Callback *callback) { callback_ = callback; }

private:
    static void driverEventCb(uint8_t addr, uint8_t iface_num,
                              uac_host_driver_event_t event, void *arg);
    static void deviceEventCb(uac_host_device_handle_t handle,
                              uac_host_device_event_t event, void *arg);
    static void consumerTaskFn(void *arg);
    void handleRxDone();

    Callback *callback_{nullptr};
    uac_host_device_handle_t device_{nullptr};
    uint8_t *rx_buf_{nullptr};
    size_t   rx_buf_size_{0};

    // Decouple RX_DONE (UAC driver task) from the codec write (consumer task)
    StreamBufferHandle_t tx_buf_{nullptr};
    TaskHandle_t consumer_task_{nullptr};

    // Detection synchronization
    SemaphoreHandle_t rx_detected_sem_{nullptr};
    uint8_t rx_addr_{0};
    uint8_t rx_iface_{0};
    bool    rx_detected_{false};

    // The IDF UAC driver requires the user to call uac_host_device_close()
    // to ack a DISCONNECT event before its internal disconnect loop will
    // terminate, but calling it directly from the callback corrupts the
    // interface lock in our environment. We hand the handle off to a
    // separate task whose priority is higher than the UAC driver's so it
    // preempts and acks promptly.
    static void closerTaskFn(void *arg);
    SemaphoreHandle_t close_pending_sem_{nullptr};
    uac_host_device_handle_t pending_close_{nullptr};
    TaskHandle_t closer_task_{nullptr};
};

}
