#pragma once
#include <cstdint>
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

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
    void returnFrame(const uvc_host_frame_t *frame);
    void setCallback(Callback *callback) { callback_ = callback; }

private:
    Callback *callback_{nullptr};
    uvc_host_stream_hdl_t stream_{nullptr};
};

}
