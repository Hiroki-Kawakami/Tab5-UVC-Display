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
#include "nvs.hpp"
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

// Mirrors PreviewScreen::connected_ so the renderer task (in this namespace)
// can pick the right no-frame behaviour without poking the screen instance.
volatile bool uvc_streaming = false;
// Signaled from PreviewScreen::onEvent when UVC_HOST_DEVICE_DISCONNECTED
// arrives; the supervisor task takes it to close the stream and re-open.
SemaphoreHandle_t uvc_reopen_sem = nullptr;

NVS settings_nvs("preview");
constexpr const char *NVS_KEY_VOLUME     = "vol";
constexpr const char *NVS_KEY_BRIGHTNESS = "brt";

uint8_t load_setting(const char *key, uint8_t fallback) {
    uint8_t v = fallback;
    if (settings_nvs.get(key, &v) != NVS::Error::OK) v = fallback;
    return v;
}

void save_setting(const char *key, uint8_t value) {
    if (settings_nvs.set(key, value) == NVS::Error::OK) {
        settings_nvs.commit();
    }
}

void renderer_task(void *) {
    int fb_index = 0;
    int frame_count = 0;
    int64_t fps_start = esp_timer_get_time();
    // Counts down full-screen black flushes we still owe after the screen
    // transitions to "nothing to show" (no UVC + no GUI). Three drains all
    // triple-buffer slots, after which we stay idle until something needs
    // refreshing again.
    int dirty_fbs = 0;
    bool was_showing = true;  // gui_visible starts true
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

        // Edge-detect the transition into "blank screen" so we know how
        // many slots still hold stale frames that need clearing.
        bool now_showing = uvc_streaming || gui_v;
        if (was_showing && !now_showing) dirty_fbs = 3;
        was_showing = now_showing;

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
            dirty_fbs = 0;

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
            // camera. When streaming, carry the previous fb's UVC band
            // forward (covers brief stalls). When disconnected, zero the
            // UVC band so the last frozen frame doesn't stay on screen.
            size_t uvc_off  = (size_t)GUI_PANEL_H * DISP_W * 3;
            size_t uvc_size = (size_t)(DISP_H - GUI_PANEL_H) * DISP_W * 3;
            if (uvc_streaming) {
                void *cur_fb = pf_port::display_get_frame_buffer(fb_index);
                memcpy((uint8_t*)out_fb + uvc_off, (uint8_t*)cur_fb + uvc_off, uvc_size);
            } else {
                memset((uint8_t*)out_fb + uvc_off, 0, uvc_size);
            }
            gui_compose(out_fb);
            flush_next = true;
            // gui_compose painted the GUI band and we just refreshed the
            // UVC band, so the slot is no longer "stale".
            if (dirty_fbs > 0) dirty_fbs--;
        } else if (dirty_fbs > 0) {
            // No UVC, no GUI — drain stale frames so the screen actually
            // goes black instead of holding the last UVC frame frozen.
            memset(out_fb, 0, (size_t)DISP_W * DISP_H * 3);
            flush_next = true;
            dirty_fbs--;
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
    lv_slider_set_value(volume_slider_, load_setting(NVS_KEY_VOLUME, 50), LV_ANIM_OFF);
    lv_obj_add_event_fn(volume_slider_, LV_EVENT_VALUE_CHANGED, [](lv_event_t *e){
        auto s = (lv_obj_t*)lv_event_get_target(e);
        bsp_tab5_audio_set_volume(lv_slider_get_value(s));
    });
    // Persist only on release so we don't hammer NVS with every drag tick.
    lv_obj_add_event_fn(volume_slider_, LV_EVENT_RELEASED, [](lv_event_t *e){
        auto s = (lv_obj_t*)lv_event_get_target(e);
        save_setting(NVS_KEY_VOLUME, (uint8_t)lv_slider_get_value(s));
    });

    auto br_lbl = lv_label_create(root_);
    lv_label_set_text(br_lbl, "Brightness");
    lv_obj_set_width(br_lbl, LV_PCT(100));
    lv_obj_set_style_margin_top(br_lbl, 20, 0);

    brightness_slider_ = lv_slider_create(root_);
    lv_obj_set_width(brightness_slider_, LV_PCT(100));
    lv_slider_set_range(brightness_slider_, 1, 100);
    lv_slider_set_value(brightness_slider_, load_setting(NVS_KEY_BRIGHTNESS, 50), LV_ANIM_OFF);
    lv_obj_add_event_fn(brightness_slider_, LV_EVENT_VALUE_CHANGED, [](lv_event_t *e){
        auto s = (lv_obj_t*)lv_event_get_target(e);
        pf_port::display_set_brightness(lv_slider_get_value(s));
    });
    lv_obj_add_event_fn(brightness_slider_, LV_EVENT_RELEASED, [](lv_event_t *e){
        auto s = (lv_obj_t*)lv_event_get_target(e);
        save_setting(NVS_KEY_BRIGHTNESS, (uint8_t)lv_slider_get_value(s));
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
    uvc_reopen_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(renderer_task, "renderer", 4096, nullptr, 16, nullptr, 0);

    // Supervisor: opens the UVC stream, sets up UAC, waits for disconnect,
    // tears both down, and loops forever so the device can be unplugged and
    // re-plugged at runtime.
    xTaskCreatePinnedToCore([](void*){
        while (true) {
            while (uvc.open(STREAM_W, STREAM_H, 30) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            uvc.start();

            // After UVC enumerated, give the UAC driver a moment to enumerate
            // and open the RX side (USB capture audio → ES8388 speaker).
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

            // Block until UVC_HOST_DEVICE_DISCONNECTED gets posted to the sem.
            xSemaphoreTake(uvc_reopen_sem, portMAX_DELAY);
            ESP_LOGI(TAG, "UVC disconnected, tearing down for reopen");
            uac.close();
            uvc.close();
        }
    }, "uvc_supervisor", 4096, nullptr, 5, nullptr, 0);
}

void PreviewScreen::onRxData(const uint8_t *data, size_t len) {
    // EQ runs in-place; the UAC consumer's resampler output buffer is mutable.
    bsp_tab5_audio_write(const_cast<uint8_t *>(data), len);
}

void PreviewScreen::onEvent(const uvc_host_stream_event_data_t *event) {
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED && connected_) {
        connected_ = false;
        uvc_streaming = false;
        // Auto-show the GUI so the user sees "Disconnected" + controls
        // without needing to tap blindly on the (now blank) screen.
        gui_set_visible(true);
        lv_async_call([this](){ set_status_ui(false); });
        if (uvc_reopen_sem) xSemaphoreGive(uvc_reopen_sem);
    }
}

bool PreviewScreen::onFrame(const uvc_host_frame_t *frame) {
    if (!connected_) {
        connected_ = true;
        uvc_streaming = true;
        // The camera just started streaming — auto-hide the overlay so the
        // user gets the full video frame. They can still tap to bring the
        // controls back up.
        gui_set_visible(false);
        lv_async_call([this](){ set_status_ui(true); });
    }
    if (xQueueSend(frame_queue, &frame, 0) != pdTRUE) {
        return true;
    }
    return false;
}
