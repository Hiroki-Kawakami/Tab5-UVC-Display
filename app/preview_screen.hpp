#pragma once
#include "screen_manager.hpp"
#include "usb_host.hpp"

class PreviewScreen: public Screen, usb_host::UVC::Callback {
public:
    virtual void build();
    virtual void onEnter();
    virtual void onEvent(const uvc_host_stream_event_data_t *event);
    virtual bool onFrame(const uvc_host_frame_t *frame);
};
