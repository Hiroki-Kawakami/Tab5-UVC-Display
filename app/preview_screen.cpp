#include "preview_screen.hpp"
#include "platform_port.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_ppa_pipeline.h"
#include "bsp_tab5.h"
#include "uvc_display.hpp"
#include "nvs.hpp"
#include <cassert>
#include <cstring>

static const char *TAG = "preview";

namespace {

constexpr int DISP_W = 720;
constexpr int DISP_H = 1280;

// Strip height hint; the pipeline rounds it to the frame's MCU height (8 for
// the YUV422 this camera emits). 16 = 2 MCU rows, so each strip covers more
// pixels and we issue fewer PPA submissions per frame (45 strips instead of
// 90). 16 lines × 1280 px × 3 B (RGB888) = 60 KiB per slot; RING_COUNT=5
// fits in 300 KiB of MALLOC_CAP_INTERNAL.
// Backpressure: a descriptor is only re-linked into the chain after PPA
// finishes its previous strip in the same ring slot. We need
// ring_count > PPA-strip / JPEG-strip ratio.
constexpr int STRIP_H = 16;
constexpr int RING_COUNT = 5;

usb_host::UVC uvc;
usb_host::UAC uac;
jpeg_ppa_pipeline_handle_t pipeline;
// Per-frame transforms, fixed by the stream geometry at onEnter(). The GUI
// variant restricts rendering to the output band below the GUI panel (only
// possible at scale 1; otherwise it's a copy of the full transform and the
// GUI is simply composed over the video).
jpeg_ppa_transform_t pipeline_transform;
jpeg_ppa_transform_t pipeline_transform_gui;
QueueHandle_t frame_queue;

// Mirrors PreviewScreen::connected_ so the renderer task (in this namespace)
// can pick the right no-frame behaviour without poking the screen instance.
volatile bool uvc_streaming = false;
// Signaled from PreviewScreen::onEvent when UVC_HOST_DEVICE_DISCONNECTED
// arrives; the supervisor task takes it to close the stream and re-open.
SemaphoreHandle_t uvc_reopen_sem = nullptr;

NVS settings_nvs("preview");
constexpr const char *NVS_KEY_VOLUME     = "vol";      // speaker (HP unplugged)
constexpr const char *NVS_KEY_HP_VOLUME  = "hp_vol";   // headphones (HP plugged in)
constexpr const char *NVS_KEY_BRIGHTNESS = "brt";
constexpr const char *NVS_KEY_PIX_FMT    = "pixfmt";   // 0=RGB888, 1=RGB565
constexpr const char *NVS_KEY_RES_W      = "res_w";    // uint16 px (UVC width)
constexpr const char *NVS_KEY_RES_H      = "res_h";    // uint16 px (UVC height)
constexpr const char *NVS_KEY_FPS        = "fps";      // uint8 frames/sec

// EQ presets. Both target the 3.5mm line-out measurement (rising AC-coupling
// HPF shape, peak near 12 kHz). For now Speaker and Headphone share the same
// coefficients — tune separately once each path has been measured on its own.
constexpr uint32_t kEqFs = 48000;
const audio_eq_biquad_t *speaker_eq_stages(size_t *n) {
    // 1W / 8Ω small driver. Bass-only lift, mid/high left untouched. The
    // 80 Hz HPF protects the cone from sub-bass excursion (the driver can't
    // reproduce it anyway). The 150 Hz peaking sits on top of the shelf to
    // put extra punch near the driver's Fs, where small speakers actually
    // radiate efficiently.
    static const audio_eq_biquad_t stages[] = {
        audio_eq_design_highpass (kEqFs,  80.0f, 0.707f),
        audio_eq_design_low_shelf(kEqFs, 300.0f, 0.707f, +7.0f),
        audio_eq_design_peaking  (kEqFs, 150.0f, 1.20f,  +3.0f),
    };
    *n = sizeof(stages) / sizeof(stages[0]);
    return stages;
}
const audio_eq_biquad_t *headphone_eq_stages(size_t *n) {
    static const audio_eq_biquad_t stages[] = {
        audio_eq_design_highpass (kEqFs,   50.0f, 0.707f),
        audio_eq_design_low_shelf(kEqFs,  150.0f, 0.707f, +10.0f),
        audio_eq_design_peaking  (kEqFs, 1000.0f, 0.80f,  -4.0f),
        audio_eq_design_peaking  (kEqFs, 2500.0f, 1.00f,  -3.0f),
    };
    *n = sizeof(stages) / sizeof(stages[0]);
    return stages;
}

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

ppa_srm_color_mode_t to_ppa_color_mode(pf_port::PixelFormat pf) {
    switch (pf) {
        case pf_port::PixelFormat::RGB565: return PPA_SRM_COLOR_MODE_RGB565;
        case pf_port::PixelFormat::RGB888: return PPA_SRM_COLOR_MODE_RGB888;
    }
    return PPA_SRM_COLOR_MODE_RGB888;
}

void renderer_task(void *) {
    const size_t fb_bpp = pf_port::bytes_per_pixel(pf_port::display_pixel_format());
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
            jpeg_ppa_output_t out = {};
            out.buffer = out_fb;
            out.pic_w = DISP_W;
            out.pic_h = DISP_H;
            out.color_mode = to_ppa_color_mode(pf_port::display_pixel_format());
            const jpeg_ppa_transform_t *t = gui_v ? &pipeline_transform_gui
                                                  : &pipeline_transform;
            esp_err_t err = jpeg_ppa_pipeline_process(pipeline, frame->data, frame->data_len,
                                                      &out, t, nullptr);
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
            size_t uvc_off  = (size_t)GUI_PANEL_H * DISP_W * fb_bpp;
            size_t uvc_size = (size_t)(DISP_H - GUI_PANEL_H) * DISP_W * fb_bpp;
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
            memset(out_fb, 0, (size_t)DISP_W * DISP_H * fb_bpp);
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

void PreviewScreen::apply_active_volume() {
    uint8_t v = hp_connected_ ? hp_volume_ : speaker_volume_;
    // lv_slider_set_value does not fire LV_EVENT_VALUE_CHANGED — programmatic
    // updates here won't recurse into the slider handler.
    if (volume_slider_) lv_slider_set_value(volume_slider_, v, LV_ANIM_OFF);
    if (volume_label_)  lv_label_set_text(volume_label_,
                                          hp_connected_ ? "Volume (Headphones)"
                                                        : "Volume (Speaker)");
    bsp_tab5_audio_set_volume(v);
}

void PreviewScreen::apply_active_eq() {
    size_t n;
    const audio_eq_biquad_t *stages = hp_connected_ ? headphone_eq_stages(&n)
                                                    : speaker_eq_stages(&n);
    bsp_tab5_audio_eq_set_biquads(stages, n);
}

void PreviewScreen::apply_active_mono_mix() {
    // Tab5 wires only L into the speaker amp; mix L+R into both channels so
    // mono speakers don't drop R-side content. HP jack carries both — keep
    // stereo there.
    bsp_tab5_audio_set_mono_mix(!hp_connected_);
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

    speaker_volume_ = load_setting(NVS_KEY_VOLUME, 50);
    hp_volume_      = load_setting(NVS_KEY_HP_VOLUME, 40);
    hp_connected_   = bsp_tab5_audio_headphone_inserted();

    // Pull the saved UVC stream parameters (defaults to 1280x720@30 on first
    // boot / when the value isn't one of the known dropdown entries).
    {
        uint16_t w = 0, h = 0;
        if (settings_nvs.get(NVS_KEY_RES_W, &w) == NVS::Error::OK &&
            settings_nvs.get(NVS_KEY_RES_H, &h) == NVS::Error::OK) {
            stream_w_ = w;
            stream_h_ = h;
        }
        uint8_t f = 0;
        if (settings_nvs.get(NVS_KEY_FPS, &f) == NVS::Error::OK && f > 0) {
            stream_fps_ = f;
        }
    }

    volume_label_ = lv_label_create(root_);
    lv_obj_set_width(volume_label_, LV_PCT(100));
    lv_obj_set_style_margin_top(volume_label_, 20, 0);

    volume_slider_ = lv_slider_create(root_);
    lv_obj_set_width(volume_slider_, LV_PCT(100));
    lv_slider_set_range(volume_slider_, 0, 100);
    lv_obj_add_event_fn(volume_slider_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *e){
        auto s = (lv_obj_t*)lv_event_get_target(e);
        uint8_t v = (uint8_t)lv_slider_get_value(s);
        (hp_connected_ ? hp_volume_ : speaker_volume_) = v;
        bsp_tab5_audio_set_volume(v);
    });
    // Persist only on release so we don't hammer NVS with every drag tick.
    lv_obj_add_event_fn(volume_slider_, LV_EVENT_RELEASED, [this](lv_event_t *e){
        auto s = (lv_obj_t*)lv_event_get_target(e);
        save_setting(hp_connected_ ? NVS_KEY_HP_VOLUME : NVS_KEY_VOLUME,
                     (uint8_t)lv_slider_get_value(s));
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

    auto if_lbl = lv_label_create(root_);
    lv_label_set_text(if_lbl, "Input Format");
    lv_obj_set_width(if_lbl, LV_PCT(100));
    lv_obj_set_style_margin_top(if_lbl, 20, 0);

    // Two dropdowns side by side under one heading. Each takes ~half the
    // row via flex_grow. The container has no styling so it doesn't add a
    // background card around the dropdowns.
    auto fmt_row = lv_obj_create(root_);
    lv_obj_remove_style_all(fmt_row);
    lv_obj_set_width(fmt_row, LV_PCT(100));
    lv_obj_set_height(fmt_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fmt_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fmt_row, 10, 0);

    res_dd_ = lv_dropdown_create(fmt_row);
    lv_obj_set_flex_grow(res_dd_, 1);
    fps_dd_ = lv_dropdown_create(fmt_row);
    lv_obj_set_flex_grow(fps_dd_, 1);

    // Pre-connect: show the saved values as the sole entry of each dropdown
    // so the UI reflects what's about to be used. The lists get replaced
    // with the camera's actual capabilities in onDeviceFormats once the
    // device enumerates.
    refresh_format_dropdowns();

    // Pipeline strip buffers, PPA scale, and the UVC stream config are all
    // fixed by onEnter() at boot. Save + reboot for the new params to take
    // effect, mirroring the pixel-format dropdown below.
    lv_obj_add_event_fn(res_dd_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *e){
        auto d = (lv_obj_t*)lv_event_get_target(e);
        uint32_t sel = lv_dropdown_get_selected(d);
        if (sel >= camera_formats_count_) return;  // pre-connect / stale event
        const auto &cf = camera_formats_[sel];
        // Preserve the user's preferred fps when possible: pick the entry
        // of cf.fps[] closest to the currently-saved stream_fps_.
        uint8_t best_fps = cf.fps[0];
        int best_diff = 256;
        for (uint8_t j = 0; j < cf.fps_count; j++) {
            int diff = (int)cf.fps[j] - (int)stream_fps_;
            if (diff < 0) diff = -diff;
            if (diff < best_diff) { best_diff = diff; best_fps = cf.fps[j]; }
        }
        settings_nvs.set(NVS_KEY_RES_W, cf.w);
        settings_nvs.set(NVS_KEY_RES_H, cf.h);
        settings_nvs.set(NVS_KEY_FPS,   best_fps);
        settings_nvs.commit();
        bsp_tab5_restart();
    });
    lv_obj_add_event_fn(fps_dd_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *e){
        auto d = (lv_obj_t*)lv_event_get_target(e);
        uint32_t sel = lv_dropdown_get_selected(d);
        int res_idx = find_resolution_idx(stream_w_, stream_h_);
        if (res_idx < 0) return;  // current res not in camera list (yet)
        const auto &cf = camera_formats_[res_idx];
        if (sel >= cf.fps_count) return;
        save_setting(NVS_KEY_FPS, cf.fps[sel]);
        bsp_tab5_restart();
    });

    auto pf_lbl = lv_label_create(root_);
    lv_label_set_text(pf_lbl, "Optimize For");
    lv_obj_set_width(pf_lbl, LV_PCT(100));
    lv_obj_set_style_margin_top(pf_lbl, 20, 0);

    auto pf_dd = lv_dropdown_create(root_);
    lv_obj_set_width(pf_dd, LV_PCT(100));
    lv_dropdown_set_options_static(pf_dd, "Image Quality\nFramerate");
    lv_dropdown_set_selected(pf_dd,
        pf_port::display_pixel_format() == pf_port::PixelFormat::RGB565 ? 1 : 0);
    // The pixel format is fixed by pf_port::init at boot — switching it
    // requires re-allocating framebuffers and re-configuring PPA/JPEG. Save
    // and reboot rather than try to tear the pipeline down at runtime.
    lv_obj_add_event_fn(pf_dd, LV_EVENT_VALUE_CHANGED, [](lv_event_t *e){
        auto d = (lv_obj_t*)lv_event_get_target(e);
        save_setting(NVS_KEY_PIX_FMT, (uint8_t)lv_dropdown_get_selected(d));
        bsp_tab5_restart();
    });

    apply_active_volume();
    apply_active_eq();
    apply_active_mono_mix();
    pf_port::display_set_brightness(lv_slider_get_value(brightness_slider_));

    // BSP polls HP_DET ~5 Hz and fires this from the bsp_spk task on change.
    // Bounce the actual UI/codec update onto the LVGL thread.
    bsp_tab5_audio_set_headphone_callback([](bool inserted, void *user){
        auto self = static_cast<PreviewScreen*>(user);
        lv_async_call([self, inserted]{
            self->hp_connected_ = inserted;
            self->apply_active_volume();
            self->apply_active_eq();
            self->apply_active_mono_mix();
        });
    }, this);
}

void PreviewScreen::onEnter() {
    usb_host::install();
    uvc.install();
    uvc.setCallback(this);
    uac.install();
    uac.setCallback(this);

    jpeg_ppa_pipeline_cfg_t pcfg = {};
    pcfg.max_pic_w = stream_w_;
    pcfg.max_pic_h = stream_h_;
    pcfg.strip_h_hint = STRIP_H;
    pcfg.ring_count = RING_COUNT;
    pcfg.strip_color_mode = PPA_SRM_COLOR_MODE_RGB888;
    pcfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    pcfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    // MJPEG is full-range YUV; the limited-range default would wash blacks out.
    pcfg.yuv_full_range = true;
    pcfg.worker_core = 0;  // share core 0 with the UVC callbacks + renderer
    ESP_ERROR_CHECK(jpeg_ppa_pipeline_new(&pcfg, &pipeline));

    // Stretch-to-fill. After ANGLE_90, the input's pic_h column maps to the
    // output's width (driven by scale_y) and the pic_w row maps to the
    // output's height (driven by scale_x). Scaling each axis independently
    // fills the full DISP_W × DISP_H without preserving aspect ratio, so
    // sub-1280×720 sources stretch to the panel instead of letterboxing.
    // 1280×720 → both scales == 1.0, behaviour is unchanged.
    pipeline_transform = {};
    pipeline_transform.rotation = PPA_SRM_ROTATION_ANGLE_90;
    pipeline_transform.scale_x = (float)DISP_H / (float)stream_w_;
    pipeline_transform.scale_y = (float)DISP_W / (float)stream_h_;
    pipeline_transform_gui = pipeline_transform;
    if (stream_w_ == DISP_H) {
        // GUI visible: skip the camera columns that would land under the GUI
        // panel (rot-90 maps input x to output y) and render only the band
        // [GUI_PANEL_H, DISP_H). Only expressible at scale 1.
        pipeline_transform_gui.in_crop = { 0, 0, (uint32_t)(DISP_H - GUI_PANEL_H),
                                           (uint32_t)stream_h_ };
        pipeline_transform_gui.out_offset_y = GUI_PANEL_H;
    }

    frame_queue = xQueueCreate(1, sizeof(uvc_host_frame_t*));
    uvc_reopen_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(renderer_task, "renderer", 4096, nullptr, 16, nullptr, 0);

    // Supervisor: opens the UVC stream, sets up UAC, waits for disconnect,
    // tears both down, and loops forever so the device can be unplugged and
    // re-plugged at runtime.
    xTaskCreatePinnedToCore([](void *arg){
        auto self = static_cast<PreviewScreen*>(arg);
        const uint16_t open_w   = self->stream_w_;
        const uint16_t open_h   = self->stream_h_;
        const uint8_t  open_fps = self->stream_fps_;
        // The MS2109 sometimes corrupts its streaming endpoint during
        // enumeration (manifests as "Enqueue URB error: ESP_ERR_INVALID_STATE"
        // inside uvc_host_stream_start). That error isn't propagated to us, so
        // the only observable signal is "no frame ever arrives". Treat
        // start-without-a-frame-within-this-window as a failure and force a
        // teardown/reopen cycle.
        constexpr TickType_t FIRST_FRAME_TIMEOUT = pdMS_TO_TICKS(3000);
        while (true) {
            // Drain any stale give carried over from a previous iteration
            // (e.g. a late disconnect that arrived after a forced teardown).
            xSemaphoreTake(uvc_reopen_sem, 0);

            while (uvc.open(open_w, open_h, open_fps) != ESP_OK) {
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

            // Wait until either a frame starts flowing (then we just block
            // for the eventual disconnect), or no frame arrives within the
            // startup window (force-teardown and retry).
            bool started = false;
            TickType_t deadline = xTaskGetTickCount() + FIRST_FRAME_TIMEOUT;
            while (true) {
                if (xSemaphoreTake(uvc_reopen_sem, pdMS_TO_TICKS(100)) == pdTRUE) {
                    break;  // disconnect arrived
                }
                if (uvc_streaming) {
                    started = true;
                    break;
                }
                if (xTaskGetTickCount() >= deadline) {
                    ESP_LOGW(TAG, "UVC start timeout: no frame within %dms, forcing reopen",
                             (int)pdTICKS_TO_MS(FIRST_FRAME_TIMEOUT));
                    break;
                }
            }
            if (started) {
                // Frames flowing — wait indefinitely for the disconnect event.
                xSemaphoreTake(uvc_reopen_sem, portMAX_DELAY);
            }
            ESP_LOGI(TAG, "UVC disconnected, tearing down for reopen");
            uac.close();
            uvc.close();
        }
    }, "uvc_supervisor", 4096, this, 5, nullptr, 0);
}

void PreviewScreen::onRxData(const uint8_t *data, size_t len) {
    // EQ runs in-place; the UAC consumer's resampler output buffer is mutable.
    bsp_tab5_audio_write(const_cast<uint8_t *>(data), len);
}

void PreviewScreen::onEvent(const uvc_host_stream_event_data_t *event) {
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
        bool was_connected = connected_;
        connected_ = false;
        uvc_streaming = false;
        if (was_connected) {
            // Was actually streaming — reset the GUI to disconnected state.
            // Auto-show the GUI so the user sees "Disconnected" + controls
            // without needing to tap blindly on the (now blank) screen.
            gui_set_visible(true);
            lv_async_call([this](){ set_status_ui(false); });
        }
        // Always wake the supervisor — needed even if no frame ever arrived
        // (silent URB enqueue failures during stream_start leave the device
        // "open" but never streaming; the disconnect event is our only
        // bus-side signal that the broken session must be torn down).
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

void PreviewScreen::onDeviceFormats(const uvc_host_frame_info_t *frames, size_t count) {
    // Fires from the UVC driver task; `frames` is only valid during this
    // call. Snapshot into PSRAM and defer the parse + LVGL update to the
    // GUI task so the dropdown widgets are touched from their owning thread.
    if (count == 0) return;
    auto *copy = new uvc_host_frame_info_t[count];
    memcpy(copy, frames, count * sizeof(uvc_host_frame_info_t));
    lv_async_call([this, copy, count](){
        apply_camera_formats(copy, count);
        delete[] copy;
    });
}

void PreviewScreen::apply_camera_formats(const uvc_host_frame_info_t *frames, size_t count) {
    camera_formats_count_ = 0;
    for (size_t i = 0; i < count; i++) {
        const auto &fi = frames[i];
        if (fi.format != UVC_VS_FORMAT_MJPEG) continue;  // pipeline only handles MJPEG
        // Drop anything larger than 1280x720 (by pixel area). The SRAM
        // strip ring (RING_COUNT * STRIP_H * pic_w * 3 B) is already at
        // the edge of the MALLOC_CAP_INTERNAL pool at 1280 wide, so
        // bigger pictures won't fit.
        if ((uint32_t)fi.h_res * (uint32_t)fi.v_res > 1280u * 720u) continue;

        // Find-or-insert by (w, h).
        int idx = -1;
        for (size_t k = 0; k < camera_formats_count_; k++) {
            if (camera_formats_[k].w == fi.h_res && camera_formats_[k].h == fi.v_res) {
                idx = (int)k;
                break;
            }
        }
        if (idx < 0) {
            if (camera_formats_count_ >= kMaxFormats) continue;
            idx = (int)camera_formats_count_++;
            camera_formats_[idx].w = fi.h_res;
            camera_formats_[idx].h = fi.v_res;
            camera_formats_[idx].fps_count = 0;
        }
        auto &cf = camera_formats_[idx];

        auto add_fps = [&](uint32_t iv) {
            if (!iv) return;
            // UVC intervals are 100ns ticks; round to nearest integer fps.
            uint32_t fps = (uint32_t)(1e7f / (float)iv + 0.5f);
            if (fps == 0 || fps > 255) return;
            for (uint8_t j = 0; j < cf.fps_count; j++) {
                if (cf.fps[j] == (uint8_t)fps) return;  // dedup
            }
            if (cf.fps_count >= kMaxFpsPerRes) return;
            cf.fps[cf.fps_count++] = (uint8_t)fps;
        };

        if (fi.interval_type == 0) {
            // Continuous range — sample at step (or the endpoints).
            if (fi.interval_step == 0) {
                add_fps(fi.interval_min);
                add_fps(fi.interval_max);
            } else {
                for (uint32_t iv = fi.interval_min;
                     iv <= fi.interval_max;
                     iv += fi.interval_step) {
                    add_fps(iv);
                }
            }
        } else {
            int n = fi.interval_type;
            if (n > CONFIG_UVC_INTERVAL_ARRAY_SIZE) n = CONFIG_UVC_INTERVAL_ARRAY_SIZE;
            for (int j = 0; j < n; j++) add_fps(fi.interval[j]);
        }
    }

    // Sort each entry's fps list descending (insertion sort — fps_count ≤ 8).
    for (size_t i = 0; i < camera_formats_count_; i++) {
        auto &cf = camera_formats_[i];
        for (uint8_t a = 1; a < cf.fps_count; a++) {
            uint8_t v = cf.fps[a];
            int b = (int)a - 1;
            while (b >= 0 && cf.fps[b] < v) {
                cf.fps[b + 1] = cf.fps[b];
                b--;
            }
            cf.fps[b + 1] = v;
        }
    }

    refresh_format_dropdowns();
}

int PreviewScreen::find_resolution_idx(uint16_t w, uint16_t h) const {
    for (size_t i = 0; i < camera_formats_count_; i++) {
        if (camera_formats_[i].w == w && camera_formats_[i].h == h) return (int)i;
    }
    return -1;
}

void PreviewScreen::refresh_format_dropdowns() {
    if (!res_dd_ || !fps_dd_) return;

    // Resolution dropdown — concat "WxH" entries separated by '\n'. Falls
    // back to the saved value as a single entry pre-connect.
    char res_opts[256];
    int off = 0;
    if (camera_formats_count_ == 0) {
        off = snprintf(res_opts, sizeof(res_opts), "%dx%d", stream_w_, stream_h_);
    } else {
        for (size_t i = 0; i < camera_formats_count_; i++) {
            int n = snprintf(res_opts + off, sizeof(res_opts) - off,
                             "%s%dx%d", (i == 0 ? "" : "\n"),
                             camera_formats_[i].w, camera_formats_[i].h);
            if (n < 0 || (size_t)n >= sizeof(res_opts) - (size_t)off) break;
            off += n;
        }
    }
    lv_dropdown_set_options(res_dd_, res_opts);

    int sel = find_resolution_idx(stream_w_, stream_h_);
    if (sel < 0) sel = 0;
    lv_dropdown_set_selected(res_dd_, sel);

    refresh_fps_dropdown(sel);
}

void PreviewScreen::refresh_fps_dropdown(int res_idx) {
    if (!fps_dd_) return;
    char fps_opts[96];
    int off = 0;
    int selected = 0;
    if (res_idx >= 0 && (size_t)res_idx < camera_formats_count_) {
        const auto &cf = camera_formats_[res_idx];
        for (uint8_t j = 0; j < cf.fps_count; j++) {
            int n = snprintf(fps_opts + off, sizeof(fps_opts) - off,
                             "%s%ufps", (j == 0 ? "" : "\n"), (unsigned)cf.fps[j]);
            if (n < 0 || (size_t)n >= sizeof(fps_opts) - (size_t)off) break;
            off += n;
            if (cf.fps[j] == stream_fps_) selected = j;
        }
    }
    if (off == 0) {
        off = snprintf(fps_opts, sizeof(fps_opts), "%ufps", (unsigned)stream_fps_);
    }
    lv_dropdown_set_options(fps_dd_, fps_opts);
    lv_dropdown_set_selected(fps_dd_, selected);
}
