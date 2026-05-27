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
    virtual void onDeviceFormats(const uvc_host_frame_info_t *frames, size_t count);

private:
    // Distilled view of the camera's MJPEG frame descriptors — one entry per
    // (w, h), each with the list of fps values the device exposes. Fixed
    // upper bounds keep the table off the heap. Populated on the LVGL task
    // via lv_async_call from the UVC driver callback.
    static constexpr size_t kMaxFormats     = 12;
    static constexpr size_t kMaxFpsPerRes   = 8;
    struct CameraFormat {
        uint16_t w;
        uint16_t h;
        uint8_t  fps_count;
        uint8_t  fps[kMaxFpsPerRes];
    };

    lv_obj_t *status_container_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *volume_label_ = nullptr;
    lv_obj_t *volume_slider_ = nullptr;
    lv_obj_t *brightness_slider_ = nullptr;
    lv_obj_t *res_dd_ = nullptr;
    lv_obj_t *fps_dd_ = nullptr;
    uint8_t  speaker_volume_ = 50;   // active when HP unplugged  (NVS: "vol")
    uint8_t  hp_volume_      = 50;   // active when HP plugged-in (NVS: "hp_vol")
    bool     hp_connected_   = false;
    volatile bool connected_ = false;
    // UVC stream parameters, loaded from NVS in build(); consumed in onEnter
    // when configuring the pipeline + opening the camera. Changes go through
    // save-to-NVS + bsp_tab5_restart, so these are effectively immutable
    // after build().
    uint16_t stream_w_   = 1280;
    uint16_t stream_h_   = 720;
    uint8_t  stream_fps_ = 30;

    CameraFormat camera_formats_[kMaxFormats] = {};
    size_t       camera_formats_count_ = 0;

    void set_status_ui(bool connected);
    void apply_active_volume();   // push current mode's volume to slider/label/codec
    void apply_active_eq();       // push current mode's EQ stages to the BSP
    void apply_active_mono_mix(); // mono mix on for speaker (L-only wired), off for HP
    void apply_camera_formats(const uvc_host_frame_info_t *frames, size_t count);
    void refresh_format_dropdowns();
    void refresh_fps_dropdown(int res_idx);
    int  find_resolution_idx(uint16_t w, uint16_t h) const;
};
