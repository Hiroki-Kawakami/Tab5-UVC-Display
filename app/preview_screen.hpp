#pragma once
#include "screen_manager.hpp"
#include "usb_host.hpp"

class PreviewScreen: public Screen, usb_host::UVC::Callback, usb_host::UAC::Callback {
public:
    virtual void build();
    virtual void onEnter();
    virtual void onEvent(const uvc_host_stream_event_data_t *event);
    virtual bool onFrame(const uvc_host_frame_t *frame);
    virtual void onRxData(const uint8_t *data, size_t len);

private:
    lv_obj_t *status_container_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *volume_label_ = nullptr;
    lv_obj_t *volume_slider_ = nullptr;
    lv_obj_t *brightness_slider_ = nullptr;
    uint8_t  speaker_volume_ = 50;   // active when HP unplugged  (NVS: "vol")
    uint8_t  hp_volume_      = 50;   // active when HP plugged-in (NVS: "hp_vol")
    bool     hp_connected_   = false;
    volatile bool connected_ = false;

    void set_status_ui(bool connected);
    void apply_active_volume();   // push current mode's volume to slider/label/codec
    void apply_active_eq();       // push current mode's EQ stages to the BSP
};
