#include <cstdio>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "bsp_tab5.h"
#include "uvc_display.hpp"
#include "platform_port.hpp"

static const char *TAG = "main";

namespace pf_port {

void init(int fb_num, PixelFormat pixel_format) {
    bsp_tab5_config_t bsp_config = {};
    bsp_config.display.fb_num = fb_num;
    bsp_config.display.pixel_format = BSP_PIXEL_FORMAT_RGB888;
    bsp_config.usb.usb5v_en = true;
    bsp_config.audio.eq.enable     = true;
    bsp_config.audio.eq.max_stages = 8;
    bsp_tab5_init(&bsp_config);

    // Speaker-out EQ for the 3.5mm jack. A swept-sine measurement showed the
    // line-out rising ~35 dB from 50 Hz to a peak near 12 kHz (typical AC-
    // coupling HPF shape). Bass-up / mid-down (V-shape) approach: a modest
    // low-shelf boost lifts the bottom, broad mid cuts push the 0.8–3 kHz
    // region down, and the highs are left alone so the result stays open
    // without sounding muffled. Cutting mids instead of boosting bass + highs
    // also saves headroom (lower clipping risk). A 50 Hz HPF keeps the shelf
    // from spending headroom on unrecoverable sub-bass.
    const uint32_t fs = 48000;
    const audio_eq_biquad_t eq_stages[] = {
        audio_eq_design_highpass (fs,   50.0f, 0.707f),
        audio_eq_design_low_shelf(fs,  150.0f, 0.707f, +11.0f),
        audio_eq_design_peaking  (fs, 1000.0f, 0.80f,  -5.0f),
        audio_eq_design_peaking  (fs, 2500.0f, 1.00f,  -3.0f),
    };
    bsp_tab5_audio_eq_set_biquads(eq_stages, sizeof(eq_stages) / sizeof(eq_stages[0]));

    lvgl_port_cfg_t config = {
        .task_priority = 4,
        .task_stack = 7168,
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    esp_err_t err = lvgl_port_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL: %s", esp_err_to_name(err));
        assert(0);
    }
}

void display_set_brightness(int value) {
    bsp_tab5_display_set_brightness(value);
}

void *display_get_frame_buffer(int fb_index) {
    return bsp_tab5_display_get_frame_buffer(fb_index);
}

void display_flush(int fb_index) {
    bsp_tab5_display_flush(fb_index);
}

std::optional<std::tuple<int, int>> touch_get_point() {
    esp_lcd_touch_point_data_t touch;
    int touch_num = bsp_tab5_touch_read(&touch, 1);
    if (touch_num > 0) {
        return std::make_tuple(touch.x, touch.y);
    }
    return std::nullopt;
}

void *psram_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}
void *psram_malloc_dma(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
}

}

extern "C" void app_main() {
    uvc_display_app();
}
