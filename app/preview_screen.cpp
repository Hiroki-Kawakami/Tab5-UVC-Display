#include "preview_screen.hpp"
#include "platform_port.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_ppa_pipeline.hpp"
#include "bsp_tab5.h"
#include "uvc_display.hpp"
#include <cassert>
#include <cstring>

static const char *TAG = "preview";

namespace {

constexpr int STREAM_W = 1280;
constexpr int STREAM_H = 720;
constexpr int DISP_W = 720;
constexpr int DISP_H = 1280;

// strip_h must be a multiple of the JPEG MCU height. The UVC camera here
// outputs YUV422 (mcu_y = 8); we use STRIP_H=16 = 2 MCU rows so each strip
// covers more pixels and we issue fewer PPA submissions per frame (45
// strips instead of 90). 16 lines × 1280 px × 3 B (RGB888) = 60 KiB per
// slot; RING_COUNT=5 fits in 300 KiB of MALLOC_CAP_INTERNAL.
// Backpressure: a descriptor is only re-linked into the chain after PPA
// finishes its previous strip in the same ring slot. We need
// ring_count > PPA-strip / JPEG-strip ratio.
constexpr int STRIP_H = 16;
constexpr int RING_COUNT = 5;

usb_host::UVC uvc;
usb_host::UAC uac;
jpeg_ppa::Pipeline *pipeline;
QueueHandle_t frame_queue;

void renderer_task(void *) {
    int fb_index = 0;
    int frame_count = 0;
    int64_t fps_start = esp_timer_get_time();
    while (true) {
        const uvc_host_frame_t *frame;
        // Short timeout so the GUI can still refresh (slider feedback,
        // connection status) when no UVC frame is arriving — e.g. before
        // the camera connects or during a stall.
        bool got_frame = xQueueReceive(frame_queue, &frame, pdMS_TO_TICKS(33)) == pdTRUE;
        // Snapshot GUI visibility for the whole frame: a flip mid-render
        // would leave a partially-painted fb (clipped UVC + skipped overlay
        // or full UVC + skipping the LVGL overwrite).
        bool gui_v = gui_is_visible();

        int next = (fb_index + 1) % 3;
        void *out_fb = pf_port::display_get_frame_buffer(next);
        bool flush_next = false;

        if (got_frame) {
            jpeg_ppa::RenderOpts opts;
            if (gui_v) {
                opts.out_y_start = GUI_PANEL_H;
                opts.out_y_end   = DISP_H;
            }
            esp_err_t err = pipeline->process(frame->data, frame->data_len, out_fb, opts);
            uvc.returnFrame(frame);
            if (err == ESP_OK) {
                if (gui_v) gui_compose(out_fb);
                flush_next = true;
            } else {
                ESP_LOGW(TAG, "pipeline err=%s", esp_err_to_name(err));
            }

            frame_count++;
            int64_t now = esp_timer_get_time();
            if (now - fps_start >= 1000000) {
                ESP_LOGI(TAG, "%dfps", frame_count);
                frame_count = 0;
                fps_start = now;
            }
        } else if (gui_v) {
            // No UVC frame within the timeout. Compose LVGL on the next fb
            // so slider drags and status changes stay responsive without a
            // camera. Carry the previous fb's UVC band forward so we don't
            // expose whatever this slot held three cycles ago.
            void *cur_fb = pf_port::display_get_frame_buffer(fb_index);
            size_t uvc_off  = (size_t)GUI_PANEL_H * DISP_W * 3;
            size_t uvc_size = (size_t)(DISP_H - GUI_PANEL_H) * DISP_W * 3;
            memcpy((uint8_t*)out_fb + uvc_off, (uint8_t*)cur_fb + uvc_off, uvc_size);
            gui_compose(out_fb);
            flush_next = true;
        }

        if (flush_next) {
            pf_port::display_flush(next);
            fb_index = next;
        }
    }
}

} // namespace

void PreviewScreen::set_status_ui(bool connected) {
    if (!status_container_ || !status_label_) return;
    lv_obj_set_style_bg_color(status_container_, lv_color_hex(connected ? 0x0EBC00 : 0xC20000), 0);
    lv_label_set_text(status_label_, connected ? "Connected" : "Disconnected");
}

void PreviewScreen::build() {
    lv_obj_set_style_bg_color(root_, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(root_, 20, 0);

    status_container_ = lv_obj_create(root_);
    lv_obj_remove_style_all(status_container_);
    lv_obj_set_size(status_container_, LV_PCT(100), 100);
    lv_obj_set_style_radius(status_container_, 15, 0);
    lv_obj_set_style_bg_opa(status_container_, LV_OPA_COVER, 0);
    status_label_ = lv_label_create(status_container_);
    lv_obj_center(status_label_);
    lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
    lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_20, 0);
    set_status_ui(false);

    auto vol_lbl = lv_label_create(root_);
    lv_label_set_text(vol_lbl, "Volume");
    lv_obj_set_width(vol_lbl, LV_PCT(100));
    lv_obj_set_style_margin_top(vol_lbl, 20, 0);

    volume_slider_ = lv_slider_create(root_);
    lv_obj_set_width(volume_slider_, LV_PCT(100));
    lv_slider_set_range(volume_slider_, 1, 100);
    lv_slider_set_value(volume_slider_, 50, LV_ANIM_OFF);
    lv_obj_add_event_fn(volume_slider_, LV_EVENT_VALUE_CHANGED, [](lv_event_t *e){
        auto s = (lv_obj_t*)lv_event_get_target(e);
        bsp_tab5_audio_set_volume(lv_slider_get_value(s));
    });

    auto br_lbl = lv_label_create(root_);
    lv_label_set_text(br_lbl, "Brightness");
    lv_obj_set_width(br_lbl, LV_PCT(100));
    lv_obj_set_style_margin_top(br_lbl, 20, 0);

    brightness_slider_ = lv_slider_create(root_);
    lv_obj_set_width(brightness_slider_, LV_PCT(100));
    lv_slider_set_range(brightness_slider_, 1, 100);
    lv_slider_set_value(brightness_slider_, 50, LV_ANIM_OFF);
    lv_obj_add_event_fn(brightness_slider_, LV_EVENT_VALUE_CHANGED, [](lv_event_t *e){
        auto s = (lv_obj_t*)lv_event_get_target(e);
        pf_port::display_set_brightness(lv_slider_get_value(s));
    });

    bsp_tab5_audio_set_volume(lv_slider_get_value(volume_slider_));
    pf_port::display_set_brightness(lv_slider_get_value(brightness_slider_));
}

void PreviewScreen::onEnter() {
    usb_host::install();
    uvc.install();
    uvc.setCallback(this);
    uac.install();
    uac.setCallback(this);

    jpeg_ppa::Config pcfg{};
    pcfg.pic_w = STREAM_W;
    pcfg.pic_h = STREAM_H;
    pcfg.strip_h = STRIP_H;
    pcfg.ring_count = RING_COUNT;
    pcfg.input_color_mode = PPA_SRM_COLOR_MODE_RGB888;
    pcfg.out_pic_w = DISP_W;
    pcfg.out_pic_h = DISP_H;
    pcfg.out_color_mode = PPA_SRM_COLOR_MODE_RGB888;
    pcfg.rotation = PPA_SRM_ROTATION_ANGLE_90;
    pcfg.scale_x = 1.0f;
    pcfg.scale_y = 1.0f;
    pcfg.yuv_rgb_conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

    pipeline = new jpeg_ppa::Pipeline();
    ESP_ERROR_CHECK(pipeline->init(pcfg));

    frame_queue = xQueueCreate(1, sizeof(uvc_host_frame_t*));
    xTaskCreatePinnedToCore(renderer_task, "renderer", 4096, nullptr, 16, nullptr, 0);

    xTaskCreatePinnedToCore([](void*){
        while (uvc.open(STREAM_W, STREAM_H, 50) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        uvc.start();

        // After UVC enumerated, give the UAC driver a moment to enumerate and
        // open the RX side (USB capture audio → ES8388 speaker).
        if (uac.openRx(16 * 1024, 4096, 5000) == ESP_OK) {
            uac_host_dev_info_t info = {};
            uint32_t rate = 48000;
            uint8_t  ch   = 2;
            uint8_t  bps  = 16;
            if (uac.getDeviceInfo(&info) == ESP_OK) {
                ESP_LOGI(TAG, "UAC dev VID=0x%04x PID=0x%04x", info.VID, info.PID);
                if (info.VID == 0x534d && info.PID == 0x2109) {  // MS2109 HDMI→USB
                    rate = 96000; ch = 1; bps = 16;
                }
            }
            uac.start(rate, ch, bps);
        }
        vTaskDelete(NULL);
    }, "uvc_open", 4096, nullptr, 5, nullptr, 0);
}

void PreviewScreen::onRxData(const uint8_t *data, size_t len) {
    bsp_tab5_audio_write(data, len);
}

void PreviewScreen::onEvent(const uvc_host_stream_event_data_t *event) {
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED && connected_) {
        connected_ = false;
        lv_async_call([this](){ set_status_ui(false); });
    }
}

bool PreviewScreen::onFrame(const uvc_host_frame_t *frame) {
    if (!connected_) {
        connected_ = true;
        lv_async_call([this](){ set_status_ui(true); });
    }
    if (xQueueSend(frame_queue, &frame, 0) != pdTRUE) {
        return true;
    }
    return false;
}
