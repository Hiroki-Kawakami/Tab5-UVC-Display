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

// 1280x720 YUV420 → RGB565 decode emits MCUs in 16-line bands. Pick the
// smallest legal strip height (one MCU row = 16 lines) so each SRAM ring slot
// stays cheap (40KB) and the decoder can hand off the very first strip to PPA
// as early as possible. ring_count buffers consume ring_count * 40KB of
// internal SRAM; the remaining strips of a frame cycle through the same
// buffers via dma2d_append() backpressure — each buffer is re-linked into
// the chain only after PPA finishes consuming the previous frame's strip in
// that ring slot. We need ring_count strictly larger than
// (PPA-strip-time / JPEG-strip-time + 1) so DMA never hits next=NULL before
// PPA frees a slot.
constexpr int STRIP_H = 16;
constexpr int RING_COUNT = 8;

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
    pcfg.input_color_mode = PPA_SRM_COLOR_MODE_RGB565;
    pcfg.out_pic_w = DISP_W;
    pcfg.out_pic_h = DISP_H;
    pcfg.out_color_mode = PPA_SRM_COLOR_MODE_RGB565;
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
