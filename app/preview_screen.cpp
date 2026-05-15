#include "preview_screen.hpp"
#include "platform_port.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_ppa_pipeline.hpp"
#include <cassert>

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
jpeg_ppa::Pipeline *pipeline;
QueueHandle_t frame_queue;

void renderer_task(void *) {
    int fb_index = 0;
    int frame_count = 0;
    int64_t fps_start = esp_timer_get_time();
    while (true) {
        const uvc_host_frame_t *frame;
        if (xQueueReceive(frame_queue, &frame, portMAX_DELAY) != pdTRUE) continue;

        int next = (fb_index + 1) % 2;
        void *out_fb = pf_port::display_get_frame_buffer(next);
        esp_err_t err = pipeline->process(frame->data, frame->data_len, out_fb);
        uvc.returnFrame(frame);
        if (err == ESP_OK) {
            pf_port::display_flush(next);
            fb_index = next;
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
    }
}

} // namespace

void PreviewScreen::build() {
}

void PreviewScreen::onEnter() {
    usb_host::install();
    uvc.install();
    uvc.setCallback(this);

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

    pipeline = new jpeg_ppa::Pipeline();
    ESP_ERROR_CHECK(pipeline->init(pcfg));

    frame_queue = xQueueCreate(1, sizeof(uvc_host_frame_t*));
    xTaskCreatePinnedToCore(renderer_task, "renderer", 4096, nullptr, 16, nullptr, 0);

    xTaskCreatePinnedToCore([](void*){
        while (uvc.open(STREAM_W, STREAM_H, 30) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        uvc.start();
        vTaskDelete(NULL);
    }, "uvc_open", 4096, nullptr, 5, nullptr, 0);
}

void PreviewScreen::onEvent(const uvc_host_stream_event_data_t *event) {
}

bool PreviewScreen::onFrame(const uvc_host_frame_t *frame) {
    if (xQueueSend(frame_queue, &frame, 0) != pdTRUE) {
        return true;
    }
    return false;
}
